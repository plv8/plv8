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
	Handle<v8::Value>	result;

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
		Handle<Array>	rows = Array::New(nrows);

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
