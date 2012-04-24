/*
 * plv8_func.cc : PL/v8 built-in functions.
 */
#include "plv8.h"
#include <sstream>

extern "C" {
#define delete		delete_
#define namespace	namespace_
#define	typeid		typeid_
#define	typename	typename_
#define	using		using_

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"

#undef delete
#undef namespace
#undef typeid
#undef typename
#undef using
} // extern "C"

using namespace v8;

static Handle<v8::Value> PrintInternal(const Arguments& args);
static Handle<v8::Value> ExecuteSqlInternal(const Arguments& args);
static Handle<v8::Value> ExecuteSqlWithArgs(const Arguments& args);
static Handle<v8::Value> CreatePlanInternal(const Arguments& args);
static Handle<v8::Value> ExecutePlanInternal(const Arguments& args);
static Handle<v8::Value> FreePlanInternal(const Arguments& args);
static Handle<v8::Value> CreateCursorInternal(const Arguments& args);
static Handle<v8::Value> FetchCursorInternal(const Arguments& args);
static Handle<v8::Value> CloseCursorInternal(const Arguments& args);
static Handle<v8::Value> YieldInternal(const Arguments& args);
static Handle<v8::Value> SubtransactionInternal(const Arguments& args);

static Handle<v8::Value> plv8_FunctionInvoker(const Arguments& args) throw();
static Handle<v8::Value> plv8_Elog(const Arguments& args);
static Handle<v8::Value> plv8_Execute(const Arguments& args);
static Handle<v8::Value> plv8_Prepare(const Arguments& args);
static Handle<v8::Value> plv8_PlanCursor(const Arguments& args);
static Handle<v8::Value> plv8_PlanExecute(const Arguments& args);
static Handle<v8::Value> plv8_PlanFree(const Arguments& args);
static Handle<v8::Value> plv8_CursorFetch(const Arguments& args);
static Handle<v8::Value> plv8_CursorClose(const Arguments& args);
static Handle<v8::Value> plv8_Subtransaction(const Arguments& args);
static Handle<v8::Value> plv8_FindFunction(const Arguments& args);

static inline Local<v8::Value>
WrapCallback(InvocationCallback func)
{
	return External::Wrap(
			reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(func)));
}

static inline InvocationCallback
UnwrapCallback(Handle<v8::Value> value)
{
	return reinterpret_cast<InvocationCallback>(
			reinterpret_cast<uintptr_t>(External::Unwrap(value)));
}

static inline void
SetCallback(Handle<ObjectTemplate> obj, const char *name,
			InvocationCallback func, PropertyAttribute attr = None)
{
	obj->Set(String::NewSymbol(name),
				FunctionTemplate::New(plv8_FunctionInvoker,
					WrapCallback(func)), attr);
}

class SubTranBlock
{
private:
	ResourceOwner		m_resowner;
	MemoryContext		m_mcontext;
public:
	SubTranBlock();
	void enter();
	void exit(bool success);
};

Persistent<ObjectTemplate> PlanTemplate;
Persistent<ObjectTemplate> CursorTemplate;

/*
 * v8 is not exception-safe! We cannot throw C++ exceptions over v8 functions.
 * So, we catch C++ exceptions and convert them to JavaScript ones.
 */
static Handle<v8::Value>
SafeCall(InvocationCallback fn, const Arguments& args) throw()
{
	HandleScope		handle_scope;
	MemoryContext	ctx = CurrentMemoryContext;

	try
	{
		return fn(args);
	}
	catch (js_error& e)
	{
		return ThrowException(e.error_object());
	}
	catch (pg_error& e)
	{
		MemoryContextSwitchTo(ctx);
		ErrorData *edata = CopyErrorData();
		Handle<String> message = ToString(edata->message);
		// XXX: add other fields? (detail, hint, context, internalquery...)
		FlushErrorState();
		FreeErrorData(edata);

		return ThrowException(Exception::Error(message));
	}
}

Handle<v8::Value>
Print(const Arguments& args) throw()
{
	return SafeCall(PrintInternal, args);
}

static Handle<v8::Value>
PrintInternal(const Arguments& args)
{
	if (args.Length() < 2)
		return ThrowError("usage: print(elevel, ...)");

	int	elevel = args[0]->Int32Value();
	switch (elevel)
	{
	case DEBUG5:
	case DEBUG4:
	case DEBUG3:
	case DEBUG2:
	case DEBUG1:
	case LOG:
	case INFO:
	case NOTICE:
	case WARNING:
		break;
	default:
		return ThrowError("invalid error level for print()");
	}

	if (args.Length() == 2)
	{
		// fast path for single argument
		elog(elevel, "%s", CString(args[1]).str(""));
	}
	else
	{
		std::ostringstream	stream;

		for (int i = 1; i < args.Length(); i++)
		{
			if (i > 1)
				stream << ' ';
			stream << CString(args[i]);
		}
		elog(elevel, "%s", stream.str().c_str());
	}

	return Undefined();
}

Handle<v8::Value>
ExecuteSql(const Arguments& args) throw()
{
	if (args.Length() != 1 && (args.Length() != 2 || !args[1]->IsArray()))
		return ThrowError("usage: executeSql(sql [, args])");

	Handle<v8::Value>	result;

	if (args.Length() == 1)
		result = SafeCall(ExecuteSqlInternal, args);
	else
		result = SafeCall(ExecuteSqlWithArgs, args);

	return result;
}

static Handle<v8::Value>
SPIResultToValue(int status)
{
	Local<v8::Value>	result;

	if (status < 0)
		return ThrowError(FormatSPIStatus(status));

	switch (status)
	{
	case SPI_OK_SELECT:
	case SPI_OK_INSERT_RETURNING:
	case SPI_OK_DELETE_RETURNING:
	case SPI_OK_UPDATE_RETURNING:
	{
		int				nrows = SPI_processed;
		Converter		conv(SPI_tuptable->tupdesc);
		Local<Array>	rows = Array::New(nrows);

		for (uint32 r = 0; r < nrows; r++)
			rows->Set(r, conv.ToValue(SPI_tuptable->vals[r]));

		result = rows;
		break;
	}
	default:
		result = Int32::New(SPI_processed);
		break;
	}

	return result;
}

/*
 * executeSql(string sql) returns array<json> for query, or integer for command.
 */
static Handle<v8::Value>
ExecuteSqlInternal(const Arguments& args)
{
	int			status;
	CString		sql(args[0]);
	SubTranBlock	subtran;

	subtran.enter();
	PG_TRY();
	{
		status = SPI_exec(sql, 0);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);
	return SPIResultToValue(status);
}

static Handle<v8::Value>
ExecuteSqlWithArgs(const Arguments& args)
{
	int				status;
	CString			sql(args[0]);
	Handle<Array>	array = Handle<Array>::Cast(args[1]);

	int		nargs = array->Length();
	Datum  *values = (Datum *) palloc(sizeof(Datum) * nargs);
	char   *nulls = (char *) palloc(sizeof(char) * nargs);
	Oid	   *argtypes = (Oid *) palloc(sizeof(Oid) * nargs);

	memset(nulls, ' ', sizeof(char) * nargs);
	for (int i = 0; i < nargs; i++)
	{
		Handle<v8::Value>	value = array->Get(i);

		if (value->IsNull() || value->IsUndefined())
		{
			nulls[i] = 'n';
			argtypes[i] = TEXTOID;
		}
		else if (value->IsBoolean())
		{
			values[i] = BoolGetDatum(value->ToBoolean()->Value());
			argtypes[i] = BOOLOID;
		}
		else if (value->IsInt32())
		{
			values[i] = Int32GetDatum(value->ToInt32()->Value());
			argtypes[i] = INT4OID;
		}
		else if (value->IsUint32())
		{
			values[i] = Int64GetDatum(value->ToUint32()->Value());
			argtypes[i] = INT8OID;
		}
		else if (value->IsNumber())
		{
			values[i] = Float8GetDatum(value->ToNumber()->Value());
			argtypes[i] = FLOAT8OID;
		}
		else if (value->IsDate())
		{
			// TODO: Date
			return ThrowError("Date is not supported as arguments for executeSql()");
		}
		else if (value->IsObject())
		{
			// TODO: Object
			return ThrowError("Object is not supported as arguments for executeSql()");
		}
		else if (value->IsArray())
		{
			// TODO: Array
			return ThrowError("Array is not supported as arguments for executeSql()");
		}
		else
		{
			CString str(value);
			values[i] = CStringGetTextDatum(str);
			argtypes[i] = TEXTOID;
		}
	}

	SubTranBlock	subtran;

	subtran.enter();
	PG_TRY();
	{
		status = SPI_execute_with_args(sql, nargs,
					argtypes, values, nulls, false, 0);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);
	return SPIResultToValue(status);
}

Handle<v8::Value> 
CreatePlan(const Arguments& args) throw()
{
	return SafeCall(CreatePlanInternal, args);
}

static Handle<v8::Value> 
CreatePlanInternal(const Arguments& args)
{
	SPIPlanPtr initial, saved;
	CString    sql(args[0]);
	Oid *types = NULL;
	Oid typeoid;
	int32 typemod;
	
	if (args.Length() > 1)
		types = (Oid *) palloc(sizeof(Oid) * args.Length() - 1);
	
	for (int i = 1; i < args.Length(); i++)
	{
		CString  typestr(args[i]);
		parseTypeString(typestr, &typeoid, &typemod);
		types[i-1] = typeoid;
	}
    
	initial = SPI_prepare(sql,args.Length() - 1,types);
	
	saved = SPI_saveplan(initial);
	SPI_freeplan(initial);
	return External::Wrap(saved);
}

Handle<v8::Value> 
ExecutePlan(const Arguments& args) throw()
{
	return SafeCall(ExecutePlanInternal, args);
}

static Handle<v8::Value> 
ExecutePlanInternal(const Arguments& args)
{
	return ThrowError("executePlan is not yet implemented");
}

Handle<v8::Value> 
FreePlan(const Arguments& args) throw()
{
	return SafeCall(FreePlanInternal, args);
}


static Handle<v8::Value> 
FreePlanInternal(const Arguments& args)
{
	int                     status;
	SPIPlanPtr      plan;
	
	plan = (SPIPlanPtr) External::Unwrap(args[0]);
	status = SPI_freeplan(plan);
    return Int32::New(status);
}

Handle<v8::Value>
CreateCursor(const Arguments& args) throw()
{
	return SafeCall(CreateCursorInternal, args);
}

static Handle<v8::Value>
CreateCursorInternal(const Arguments& args)
{
	SPIPlanPtr      plan;
	Portal cursor;
	int nargs;
	Datum *cargs = NULL;
	char *nulls = NULL;
    
	plan = (SPIPlanPtr) External::Unwrap(args[0]);
	nargs = SPI_getargcount(plan);
    
	if (args.Length() - 1 != nargs)
	{
		// XXX Fill in error here
	}
	
	if (nargs > 0)
	{
		cargs = (Datum *) palloc(sizeof(Datum) * nargs);
		nulls = (char *) palloc(sizeof(char) * nargs);
	}
	
	for (int i = 0; i < nargs; i++)
	{
		Handle<v8::Value> value = args[i+1];
		
		if (value->IsUndefined() || value->IsNull())
		{
			cargs[i] = (Datum) 0;
			nulls[i] = 'n';
		}
		else
		{
			
			// code stolen from plv8_fill_type()
			plv8_type typinfo;
			plv8_type *type = &typinfo;
			Oid typid =  SPI_getargtypeid(plan, i);
			MemoryContext mcxt = CurrentMemoryContext;
			bool isnull, ispreferred;
			nulls[i] = ' ';
			
			type->typid = typid;
			type->fn_input.fn_mcxt = type->fn_output.fn_mcxt = mcxt;
			get_type_category_preferred(typid, &type->category, &ispreferred);
			get_typlenbyvalalign(typid, &type->len, &type->byval, &type->align);
			
			if (type->category == TYPCATEGORY_ARRAY)
			{
				Oid      elemid = get_element_type(typid);
                
				if (elemid == InvalidOid)
					ereport(ERROR,
							(errmsg("cannot determine element type of array: %u", typid)));
				
				type->typid = elemid;
				get_typlenbyvalalign(type->typid, &type->len, &type->byval, &type->align);
			}
			
			cargs[i] = ToDatum(value,&isnull,type);
		}
	}

	cursor = SPI_cursor_open(NULL, plan, cargs, nulls, false);
	Handle<String> cname = ToString(cursor->name, strlen(cursor->name));
	return cname;
}

Handle<v8::Value> 
FetchCursor(const Arguments& args) throw()
{
	return SafeCall(FetchCursorInternal, args);
}

static Handle<v8::Value> 
FetchCursorInternal(const Arguments& args)
{
	CString cname(args[0]);
	Portal cursor = SPI_cursor_find(cname);
	SPI_cursor_fetch(cursor, true, 1);
	
	if (SPI_processed == 1)
	{
		// XXX: we need to cache this converter object instead of
		// making a new one per row, but we can't cache it until
		// we have the first row so we have a tupdesc
		Converter               conv(SPI_tuptable->tupdesc);
		Handle<v8::Object> result = conv.ToValue(SPI_tuptable->vals[0]);
		CString rstr(result);
		char * rcstr = rstr;
		return result;
	}
	else
	{
		return Undefined();
	}
}

Handle<v8::Value> 
CloseCursor(const Arguments& args) throw()
{
	return SafeCall(CloseCursorInternal, args);
}

static Handle<v8::Value> 
CloseCursorInternal(const Arguments& args)
{
	CString cname(args[0]);
	Portal cursor = SPI_cursor_find(cname);
	SPI_cursor_close(cursor);
    return Int32::New(1);
}

Handle<v8::Value>
Yield(const Arguments& args) throw()
{
	return SafeCall(YieldInternal, args);
}

static Handle<v8::Value>
YieldInternal(const Arguments& args)
{
	Handle<Array> data = Handle<Array>::Cast(args.Data());

	Converter *conv = static_cast<Converter *>(
		External::Unwrap(data->Get(0)));
	Tuplestorestate *tupstore = static_cast<Tuplestorestate *>(
		External::Unwrap(data->Get(1)));

	conv->ToDatum(args[0], tupstore);
	return Undefined();
}

Handle<Function>
CreateYieldFunction(Converter *conv, Tuplestorestate *tupstore)
{
	Handle<Array> data = Array::New(2);
	data->Set(0, External::Wrap(conv));
	data->Set(1, External::Wrap(tupstore));
	return FunctionTemplate::New(Yield, data)->GetFunction();
}

Handle<v8::Value>
Subtransaction(const Arguments& args) throw()
{
	return SafeCall(SubtransactionInternal, args);
}

static Handle<v8::Value>
SubtransactionInternal(const Arguments& args)
{
	if (args.Length() < 1)
		return Local<v8::Value>(*v8::Undefined());
	if (!args[0]->IsFunction())
		return Local<v8::Value>(*v8::Undefined());
	Handle<v8::Function> func = Local<v8::Function>(Function::Cast(*args[0]));

	SubTranBlock	subtran;

	subtran.enter();

	Handle<v8::Value> emptyargs[] = {};
	TryCatch try_catch;
	Handle<v8::Value> result = func->Call(func, 0, emptyargs);

	subtran.exit(!result.IsEmpty());

	if (result.IsEmpty())
		throw js_error(try_catch);
	return result;
}

SubTranBlock::SubTranBlock()
	: m_resowner(NULL),
	  m_mcontext(NULL)
{}

void
SubTranBlock::enter()
{
	m_resowner = CurrentResourceOwner;
	m_mcontext = CurrentMemoryContext;
	BeginInternalSubTransaction(NULL);
	/* Do not want to leave the previous memory context */
	MemoryContextSwitchTo(m_mcontext);

}

void
SubTranBlock::exit(bool success)
{
	if (success)
		ReleaseCurrentSubTransaction();
	else
		RollbackAndReleaseCurrentSubTransaction();

	MemoryContextSwitchTo(m_mcontext);
	CurrentResourceOwner = m_resowner;

	/*
	 * AtEOSubXact_SPI() should not have popped any SPI context, but just
	 * in case it did, make sure we remain connected.
	 */
	SPI_restore_connection();
}

void
SetupPlv8Functions(Handle<ObjectTemplate> plv8)
{
	PropertyAttribute	attrFull =
		PropertyAttribute(ReadOnly | DontEnum | DontDelete);

	SetCallback(plv8, "elog", plv8_Elog, attrFull);
	SetCallback(plv8, "execute", plv8_Execute, attrFull);
	SetCallback(plv8, "prepare", plv8_Prepare, attrFull);
	SetCallback(plv8, "subtransaction", plv8_Subtransaction, attrFull);
	SetCallback(plv8, "find_function", plv8_FindFunction, attrFull);
}

/*
 * v8 is not exception-safe! We cannot throw C++ exceptions over v8 functions.
 * So, we catch C++ exceptions and convert them to JavaScript ones.
 */
static Handle<v8::Value>
plv8_FunctionInvoker(const Arguments &args) throw()
{
	HandleScope		handle_scope;
	MemoryContext	ctx = CurrentMemoryContext;
	InvocationCallback	fn = UnwrapCallback(args.Data());

	try
	{
		return fn(args);
	}
	catch (js_error& e)
	{
		return ThrowException(e.error_object());
	}
	catch (pg_error& e)
	{
		MemoryContextSwitchTo(ctx);
		ErrorData *edata = CopyErrorData();
		Handle<String> message = ToString(edata->message);
		// XXX: add other fields? (detail, hint, context, internalquery...)
		FlushErrorState();
		FreeErrorData(edata);

		return ThrowException(Exception::Error(message));
	}
}

/*
 * plv8.elog(elevel, str)
 */
static Handle<v8::Value>
plv8_Elog(const Arguments& args)
{
	MemoryContext	ctx = CurrentMemoryContext;

	if (args.Length() < 2)
		return ThrowError("usage: print(elevel, ...)");

	int	elevel = args[0]->Int32Value();
	switch (elevel)
	{
	case DEBUG5:
	case DEBUG4:
	case DEBUG3:
	case DEBUG2:
	case DEBUG1:
	case LOG:
	case INFO:
	case NOTICE:
	case WARNING:
	case ERROR:
		break;
	default:
		return ThrowError("invalid error level");
	}

	std::ostringstream	stream;

	for (int i = 1; i < args.Length(); i++)
	{
		if (i > 1)
			stream << ' ';
		stream << CString(args[i]);
	}

	const char	   *message = stream.str().c_str();

	if (elevel != ERROR)
	{
		elog(elevel, "%s", message);
		return Undefined();
	}

	/* ERROR case */
	PG_TRY();
	{
		elog(elevel, "%s", message);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(ctx);
		ErrorData *edata = CopyErrorData();
		Local<String> message = ToString(edata->message);
		FlushErrorState();
		FreeErrorData(edata);

		return ThrowException(Exception::Error(message));
	}
	PG_END_TRY();

	return Undefined();
}

static Datum
ValueGetDatum(Handle<v8::Value> value, Oid typid, char *isnull)
{
	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = 'n';
		return (Datum) 0;
	}
	else
	{
		plv8_type	typinfo = { 0 };
		bool		IsNull;
		Datum		datum;

		plv8_fill_type(&typinfo, typid);
		datum = ToDatum(value, &IsNull, &typinfo);
		*isnull = (IsNull ?  'n' : ' ');
		return datum;
	}
}

/*
 * plv8.execute(statement, [param, ...])
 */
static Handle<v8::Value>
plv8_Execute(const Arguments &args)
{
	int				status;

	if (args.Length() < 1)
		return Undefined();

	CString			sql(args[0]);
	Handle<Array>	params;

	if (args.Length() >= 2)
		params = Handle<Array>::Cast(args[1]);

	int				nparam = params.IsEmpty() ? 0 : params->Length();
	Datum		   *values = (Datum *) palloc(sizeof(Datum) * nparam);
	char		   *nulls = (char *) palloc(sizeof(char) * nparam);
	Oid			   *types = (Oid *) palloc(sizeof(Oid) * nparam);

	for (int i = 0; i < nparam; i++)
	{
		Handle<v8::Value>	param = params->Get(i);

		types[i] = InferredDatumType(param);
		values[i] = ValueGetDatum(param, types[i], &nulls[i]);
	}

	SubTranBlock	subtran;
	PG_TRY();
	{
		subtran.enter();
		if (nparam > 0)
			status = SPI_execute_with_args(sql, nparam,
						types, values, nulls, false, 0);
		else
			status = SPI_exec(sql, 0);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	return SPIResultToValue(status);
}

/*
 * plv8.prepare(statement, args...)
 */
static Handle<v8::Value>
plv8_Prepare(const Arguments &args)
{
	SPIPlanPtr		initial = NULL, saved;
	CString			sql(args[0]);
	Handle<Array>	array;
	int				arraylen = 0;
	Oid			   *types = NULL;

	if (args.Length() > 1)
	{
		array = Handle<Array>::Cast(args[1]);
		arraylen = array->Length();
		types = (Oid *) palloc(sizeof(Oid) * arraylen);
	}

	for (int i = 0; i < arraylen; i++)
	{
		CString			typestr(array->Get(i));
		int32			typemod;

		parseTypeString(typestr, &types[i], &typemod);
	}

	PG_TRY();
	{
		initial = SPI_prepare(sql, arraylen, types);
		saved = SPI_saveplan(initial);
		SPI_freeplan(initial);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (PlanTemplate.IsEmpty())
	{
		Local<FunctionTemplate> base = FunctionTemplate::New();
		base->SetClassName(String::NewSymbol("PreparedPlan"));
		Local<ObjectTemplate> templ = base->InstanceTemplate();
		templ->SetInternalFieldCount(1);
		SetCallback(templ, "cursor", plv8_PlanCursor);
		SetCallback(templ, "execute", plv8_PlanExecute);
		SetCallback(templ, "free", plv8_PlanFree);
		PlanTemplate = Persistent<ObjectTemplate>::New(templ);
	}

	Local<v8::Object> result = PlanTemplate->NewInstance();
	result->SetInternalField(0, External::Wrap(saved));

	return result;
}

/*
 * plan.cursor(args, ...)
 */
static Handle<v8::Value>
plv8_PlanCursor(const Arguments &args)
{
	Handle<v8::Object>	self = args.This();
	SPIPlanPtr			plan;
	Datum			   *values = NULL;
	char			   *nulls = NULL;
	int					nparam = 0, argcount;
	Handle<Array>		params;
	Portal				cursor;

	plan = (SPIPlanPtr) External::Unwrap(self->GetInternalField(0));
	/* XXX: Add plan validation */

	if (args.Length() > 0 && args[0]->IsArray())
	{
		params = Handle<Array>::Cast(args[0]);
		nparam = params->Length();
	}

	argcount = SPI_getargcount(plan);

	if (argcount != nparam)
	{
		StringInfoData	buf;

		initStringInfo(&buf);
		appendStringInfo(&buf,
				"plan expected %d argument(s), given is %d", argcount, nparam);
		throw js_error(pstrdup(buf.data));
	}

	if (nparam > 0)
	{
		values = (Datum *) palloc(sizeof(Datum) * nparam);
		nulls = (char *) palloc(sizeof(char) * nparam);
	}

	for (int i = 0; i < nparam; i++)
	{
		Handle<v8::Value>	param = params->Get(i);
		Oid					typid = SPI_getargtypeid(plan, i);

		values[i] = ValueGetDatum(param, typid, &nulls[i]);
	}
	cursor = SPI_cursor_open(NULL, plan, values, nulls, false);
	Handle<String> cname = ToString(cursor->name, strlen(cursor->name));

	/*
	 * Instantiate if the template is empty.
	 */
	if (CursorTemplate.IsEmpty())
	{
		Local<FunctionTemplate> base = FunctionTemplate::New();
		base->SetClassName(String::NewSymbol("Cursor"));
		Local<ObjectTemplate> templ = base->InstanceTemplate();
		templ->SetInternalFieldCount(1);
		SetCallback(templ, "fetch", plv8_CursorFetch);
		SetCallback(templ, "close", plv8_CursorClose);
		CursorTemplate = Persistent<ObjectTemplate>::New(templ);
	}

	Local<v8::Object> result = CursorTemplate->NewInstance();
	result->SetInternalField(0, cname);

	return result;
}

/*
 * plan.execute(args, ...)
 */
static Handle<v8::Value>
plv8_PlanExecute(const Arguments &args)
{
	Handle<v8::Object>	self = args.This();
	SPIPlanPtr			plan;
	Datum			   *values = NULL;
	char			   *nulls = NULL;
	int					nparam = 0, argcount;
	Handle<Array>		params;
	SubTranBlock		subtran;
	int					status;

	plan = static_cast<SPIPlanPtr>(External::Unwrap(self->GetInternalField(0)));
	/* XXX: Add plan validation */

	if (args.Length() > 0 && args[0]->IsArray())
	{
		params = Handle<Array>::Cast(args[0]);
		nparam = params->Length();
	}

	argcount = SPI_getargcount(plan);

	if (argcount != nparam)
	{
		StringInfoData	buf;

		initStringInfo(&buf);
		appendStringInfo(&buf,
				"plan expected %d argument(s), given is %d", argcount, nparam);
		throw js_error(pstrdup(buf.data));
	}

	if (nparam > 0)
	{
		values = (Datum *) palloc(sizeof(Datum) * nparam);
		nulls = (char *) palloc(sizeof(char) * nparam);
	}

	for (int i = 0; i < nparam; i++)
	{
		Handle<v8::Value>	param = params->Get(i);
		Oid					typid = SPI_getargtypeid(plan, i);

		values[i] = ValueGetDatum(param, typid, &nulls[i]);
	}

	PG_TRY();
	{
		subtran.enter();
		status = SPI_execute_plan(plan, values, nulls, false, 0);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	return SPIResultToValue(status);
}

/*
 * plan.free()
 */
static Handle<v8::Value>
plv8_PlanFree(const Arguments &args)
{
	Handle<v8::Object>	self = args.This();
	SPIPlanPtr			plan;
	int					status = 0;

	plan = static_cast<SPIPlanPtr>(External::Unwrap(self->GetInternalField(0)));

	if (plan)
		status = SPI_freeplan(plan);

	self->SetInternalField(0, External::Wrap(0));

	return Int32::New(status);
}

/*
 * cursor.fetch([n])
 */
static Handle<v8::Value>
plv8_CursorFetch(const Arguments &args)
{
	Handle<v8::Object>	self = args.This();
	CString				cname(self->GetInternalField(0));
	Portal				cursor = SPI_cursor_find(cname);

	if (!cursor)
		throw js_error("cannot find cursor");

	/* XXX: get the argument */
	SPI_cursor_fetch(cursor, true, 1);

	if (SPI_processed == 1)
	{
		Converter			conv(SPI_tuptable->tupdesc);
		Handle<v8::Object>	result = conv.ToValue(SPI_tuptable->vals[0]);
		return result;
	}
	return Undefined();
}

/*
 * cursor.close()
 */
static Handle<v8::Value>
plv8_CursorClose(const Arguments &args)
{
	Handle<v8::Object>	self = args.This();
	CString				cname(self->GetInternalField(0));
	Portal				cursor = SPI_cursor_find(cname);

	if (!cursor)
		throw js_error("cannot find cursor");

	SPI_cursor_close(cursor);

	return Int32::New(cursor ? 1 : 0);
}

/*
 * plv8.subtransaction(func(){ ... })
 */
static Handle<v8::Value>
plv8_Subtransaction(const Arguments& args)
{
	if (args.Length() < 1)
		return Undefined();
	if (!args[0]->IsFunction())
		return Undefined();
	Handle<Function>	func = Handle<Function>::Cast(args[0]);
	SubTranBlock		subtran;

	subtran.enter();

	Handle<v8::Value> emptyargs[] = {};
	TryCatch try_catch;
	Handle<v8::Value> result = func->Call(func, 0, emptyargs);

	subtran.exit(!result.IsEmpty());

	if (result.IsEmpty())
		throw js_error(try_catch);
	return result;
}

/*
 * plv8.find_function("signature")
 */
static Handle<v8::Value>
plv8_FindFunction(const Arguments& args)
{
	if (args.Length() < 1)
		return Undefined();
	CString				signature(args[0]);
	Local<Function>		func;

	PG_TRY();
	{
		func = find_js_function_by_name(signature.str());
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return func;
}
