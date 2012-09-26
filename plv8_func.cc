/*-------------------------------------------------------------------------
 *
 * plv8_func.cc : PL/v8 built-in functions.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"
#include "plv8_param.h"
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

static Handle<v8::Value> plv8_FunctionInvoker(const Arguments& args) throw();
static Handle<v8::Value> plv8_Elog(const Arguments& args);
static Handle<v8::Value> plv8_Execute(const Arguments& args);
static Handle<v8::Value> plv8_Prepare(const Arguments& args);
static Handle<v8::Value> plv8_PlanCursor(const Arguments& args);
static Handle<v8::Value> plv8_PlanExecute(const Arguments& args);
static Handle<v8::Value> plv8_PlanFree(const Arguments& args);
static Handle<v8::Value> plv8_CursorFetch(const Arguments& args);
static Handle<v8::Value> plv8_CursorClose(const Arguments& args);
static Handle<v8::Value> plv8_ReturnNext(const Arguments& args);
static Handle<v8::Value> plv8_Subtransaction(const Arguments& args);
static Handle<v8::Value> plv8_FindFunction(const Arguments& args);
static Handle<v8::Value> plv8_QuoteLiteral(const Arguments& args);
static Handle<v8::Value> plv8_QuoteNullable(const Arguments& args);
static Handle<v8::Value> plv8_QuoteIdent(const Arguments& args);

#if PG_VERSION_NUM < 90100
/*
 * quote_literal_cstr -
 *	  returns a properly quoted literal
 */
static char *
quote_literal_cstr(const char *rawstr)
{
	return TextDatumGetCString(
			DirectFunctionCall1(quote_literal, CStringGetTextDatum(rawstr)));
}
#endif

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

		for (int r = 0; r < nrows; r++)
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
	SetCallback(plv8, "return_next", plv8_ReturnNext, attrFull);
	SetCallback(plv8, "subtransaction", plv8_Subtransaction, attrFull);
	SetCallback(plv8, "find_function", plv8_FindFunction, attrFull);
	SetCallback(plv8, "quote_literal", plv8_QuoteLiteral, attrFull);
	SetCallback(plv8, "quote_nullable", plv8_QuoteNullable, attrFull);
	SetCallback(plv8, "quote_ident", plv8_QuoteIdent, attrFull);

	plv8->SetInternalFieldCount(PLV8_INTNL_MAX);
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
		return ThrowError("usage: plv8.elog(elevel, ...)");

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
value_get_datum(Handle<v8::Value> value, Oid typid, char *isnull)
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
		try
		{
			datum = ToDatum(value, &IsNull, &typinfo);
		}
		catch (js_error& e){ e.rethrow(); }
		*isnull = (IsNull ?  'n' : ' ');
		return datum;
	}
}

static int
plv8_execute_params(const char *sql, Handle<Array> params)
{
	Assert(!params.IsEmpty());

	int				status;
	int				nparam = params->Length();
	Datum		   *values = (Datum *) palloc(sizeof(Datum) * nparam);
	char		   *nulls = (char *) palloc(sizeof(char) * nparam);
/*
 * Since 9.0, SPI may have the parser deduce the parameter types.  In prior
 * versions, we infer the types from the input JS values.
 */
#if PG_VERSION_NUM >= 90000
	SPIPlanPtr		plan;
	plv8_param_state parstate = {0};
	ParamListInfo	paramLI;

	parstate.memcontext = CurrentMemoryContext;
	plan = SPI_prepare_params(sql, plv8_variable_param_setup,
							  &parstate, 0);
	if (parstate.numParams != nparam)
		elog(ERROR, "parameter numbers mismatch: %d != %d",
				parstate.numParams, nparam);
	for (int i = 0; i < nparam; i++)
	{
		Handle<v8::Value>	param = params->Get(i);
		values[i] = value_get_datum(param,
								  parstate.paramTypes[i], &nulls[i]);
	}
	paramLI = plv8_setup_variable_paramlist(&parstate, values, nulls);
	status = SPI_execute_plan_with_paramlist(plan, paramLI, false, 0);
#else
	Oid			   *types = (Oid *) palloc(sizeof(Oid) * nparam);

	for (int i = 0; i < nparam; i++)
	{
		Handle<v8::Value>	param = params->Get(i);

		types[i] = inferred_datum_type(param);
		if (types[i] == InvalidOid)
			elog(ERROR, "parameter[%d] cannot translate to a database type", i);

		values[i] = value_get_datum(param, types[i], &nulls[i]);
	}
	status = SPI_execute_with_args(sql, nparam, types, values, nulls, false, 0);

	pfree(types);
#endif

	pfree(values);
	pfree(nulls);
	return status;
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


	SubTranBlock	subtran;
	PG_TRY();
	{
		subtran.enter();
		if (nparam == 0)
			status = SPI_exec(sql, 0);
		else
			status = plv8_execute_params(sql, params);
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
	plv8_param_state *parstate = NULL;

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
#if PG_VERSION_NUM >= 90000
		if (args.Length() == 1)
		{
			parstate =
				(plv8_param_state *) palloc0(sizeof(plv8_param_state));
			parstate->memcontext = CurrentMemoryContext;
			initial = SPI_prepare_params(sql, plv8_variable_param_setup,
										 parstate, 0);
		}
		else
#endif
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
		templ->SetInternalFieldCount(2);
		SetCallback(templ, "cursor", plv8_PlanCursor);
		SetCallback(templ, "execute", plv8_PlanExecute);
		SetCallback(templ, "free", plv8_PlanFree);
		PlanTemplate = Persistent<ObjectTemplate>::New(templ);
	}

	Local<v8::Object> result = PlanTemplate->NewInstance();
	result->SetInternalField(0, External::Wrap(saved));
#if PG_VERSION_NUM >= 90000
	if (parstate)
		result->SetInternalField(1, External::Wrap(parstate));
#endif

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
	plv8_param_state   *parstate = NULL;

	plan = static_cast<SPIPlanPtr>(External::Unwrap(self->GetInternalField(0)));
	/* XXX: Add plan validation */

	if (args.Length() > 0 && args[0]->IsArray())
	{
		params = Handle<Array>::Cast(args[0]);
		nparam = params->Length();
	}

	/*
	 * If the plan has the variable param info, use it.
	 */
	parstate = static_cast<plv8_param_state *>(
			External::Unwrap(self->GetInternalField(1)));

	if (parstate)
		argcount = parstate->numParams;
	else
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
		Oid					typid;

		if (parstate)
			typid = parstate->paramTypes[i];
		else
			typid = SPI_getargtypeid(plan, i);

		values[i] = value_get_datum(param, typid, &nulls[i]);
	}

	PG_TRY();
	{
#if PG_VERSION_NUM >= 90000
		if (parstate)
		{
			ParamListInfo	paramLI;

			paramLI = plv8_setup_variable_paramlist(parstate, values, nulls);
			cursor = SPI_cursor_open_with_paramlist(NULL, plan, paramLI, false);
		}
		else
#endif
			cursor = SPI_cursor_open(NULL, plan, values, nulls, false);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

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
	plv8_param_state   *parstate = NULL;

	plan = static_cast<SPIPlanPtr>(External::Unwrap(self->GetInternalField(0)));
	/* XXX: Add plan validation */

	if (args.Length() > 0 && args[0]->IsArray())
	{
		params = Handle<Array>::Cast(args[0]);
		nparam = params->Length();
	}

	/*
	 * If the plan has the variable param info, use it.
	 */
	parstate = static_cast<plv8_param_state *> (
			External::Unwrap(self->GetInternalField(1)));

	if (parstate)
		argcount = parstate->numParams;
	else
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
		Oid					typid;

		if (parstate)
			typid = parstate->paramTypes[i];
		else
			typid = SPI_getargtypeid(plan, i);

		values[i] = value_get_datum(param, typid, &nulls[i]);
	}

	PG_TRY();
	{
		subtran.enter();
#if PG_VERSION_NUM >= 90000
		if (parstate)
		{
			ParamListInfo	paramLI;

			paramLI = plv8_setup_variable_paramlist(parstate, values, nulls);
			status = SPI_execute_plan_with_paramlist(plan, paramLI, false, 0);
		}
		else
#endif
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
	plv8_param_state   *parstate;
	int					status = 0;

	plan = static_cast<SPIPlanPtr>(External::Unwrap(self->GetInternalField(0)));

	if (plan)
		status = SPI_freeplan(plan);

	self->SetInternalField(0, External::Wrap(0));

	parstate = static_cast<plv8_param_state *> (
			External::Unwrap(self->GetInternalField(1)));

	if (parstate)
		pfree(parstate);
	self->SetInternalField(1, External::Wrap(0));

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
 * plv8.return_next(retval)
 */
static Handle<v8::Value>
plv8_ReturnNext(const Arguments& args)
{
	Handle<v8::Object>	self = args.This();

	if (self->GetInternalField(PLV8_INTNL_CONV).IsEmpty())
		throw js_error("return_next called in context that cannot accept a set");
	Converter *conv = static_cast<Converter *>(
			External::Unwrap(self->GetInternalField(PLV8_INTNL_CONV)));
	Tuplestorestate *tupstore = static_cast<Tuplestorestate *>(
			External::Unwrap(self->GetInternalField(PLV8_INTNL_TUPSTORE)));

	conv->ToDatum(args[0], tupstore);

	return Undefined();
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

static Handle<v8::Value>
plv8_QuoteLiteral(const Arguments& args)
{
	if (args.Length() < 1)
		return Undefined();
	CString			instr(args[0]);
	char		   *result;

	PG_TRY();
	{
		result = quote_literal_cstr(instr);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return ToString(result);
}

static Handle<v8::Value>
plv8_QuoteNullable(const Arguments& args)
{
	if (args.Length() < 1)
		return Undefined();
	CString			instr(args[0]);
	char		   *result;

	if (args[0]->IsNull() || args[0]->IsUndefined())
		return ToString("NULL");

	PG_TRY();
	{
		result = quote_literal_cstr(instr);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return ToString(result);
}

static Handle<v8::Value>
plv8_QuoteIdent(const Arguments& args)
{
	if (args.Length() < 1)
		return Undefined();
	CString			instr(args[0]);
	const char	   *result;

	PG_TRY();
	{
		result = quote_identifier(instr);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return ToString(result);
}
