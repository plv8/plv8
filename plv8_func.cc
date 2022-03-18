/*-------------------------------------------------------------------------
 *
 * plv8_func.cc : PL/v8 built-in functions.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"
#include "plv8_param.h"
#include <string>

extern "C" {
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "nodes/memnodes.h"
} // extern "C"

using namespace v8;

static void plv8_FunctionInvoker(const FunctionCallbackInfo<v8::Value>& args) throw();
static void plv8_Elog(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_Execute(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_Prepare(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_PlanCursor(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_PlanExecute(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_PlanFree(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_CursorFetch(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_CursorMove(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_CursorClose(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_ReturnNext(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_Subtransaction(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_FindFunction(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_GetWindowObject(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinGetPartitionLocal(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinSetPartitionLocal(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinGetCurrentPosition(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinGetPartitionRowCount(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinSetMarkPosition(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinRowsArePeers(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinGetFuncArgInPartition(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinGetFuncArgInFrame(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_WinGetFuncArgCurrent(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_QuoteLiteral(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_QuoteNullable(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_QuoteIdent(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_MemoryUsage(const FunctionCallbackInfo<v8::Value>& args);

#if PG_VERSION_NUM >= 110000
static void plv8_Commit(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_Rollback(const FunctionCallbackInfo<v8::Value>& args);
#endif

/*
 * Window function API allows to store partition-local memory, but
 * the allocation is only once per partition.  maxlen represents
 * the allocated size for this partition (if it's zero, the allocation
 * has just happened).  Also v8 doesn't provide vaule serialization,
 * so currently the object is JSON-ized and stored as a string.
 */
typedef struct window_storage
{
	size_t		maxlen;			/* allocated memory */
	size_t		len;			/* the byte size of data */
	char		data[1];		/* actual string (without null-termination */
} window_storage;

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
WrapCallback(FunctionCallback func)
{
	Isolate* isolate = Isolate::GetCurrent();
	return External::New(isolate,
			reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(func)));
}

static inline FunctionCallback
UnwrapCallback(Handle<v8::Value> value)
{
	return reinterpret_cast<FunctionCallback>(
			reinterpret_cast<uintptr_t>(External::Cast(*value)->Value()));
}

static inline void
SetCallback(Handle<ObjectTemplate> obj, const char *name,
			FunctionCallback func, PropertyAttribute attr = None)
{
	Isolate* isolate = Isolate::GetCurrent();
	obj->Set(isolate, name,
				FunctionTemplate::New(isolate, plv8_FunctionInvoker,
					WrapCallback(func)));
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

static text *
charToText(char *string)
{
	int len = strlen(string);
	text *result = (text *) palloc(len + 1 + VARHDRSZ);

	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), string, len + 1);

	return result;
}


static Handle<v8::Value>
SPIResultToValue(int status)
{
	Isolate* isolate = Isolate::GetCurrent();
	Local<Context>		context = isolate->GetCurrentContext();
	Local<v8::Value>	result;

	if (status < 0) {
		isolate->ThrowException(String::NewFromUtf8(isolate, FormatSPIStatus(status)).ToLocalChecked());
		return result;
	}

	switch (status)
	{
	case SPI_OK_UTILITY:
	case SPI_OK_REWRITTEN:
	{
		if (SPI_tuptable == NULL) {
			result = Int32::New(isolate, SPI_processed);
			break;
		}
		// will fallthrough here to the "SELECT" logic below
	}
	case SPI_OK_SELECT:
	case SPI_OK_INSERT_RETURNING:
	case SPI_OK_DELETE_RETURNING:
	case SPI_OK_UPDATE_RETURNING:
	{
		int				nrows = SPI_processed;
		Converter		conv(SPI_tuptable->tupdesc);
		Local<Array>	rows = Array::New(isolate, nrows);

		for (int r = 0; r < nrows; r++)
			rows->Set(context, r, conv.ToValue(SPI_tuptable->vals[r])).Check();

		result = rows;
		break;
	}
	default:
		result = Int32::New(isolate, SPI_processed);
		break;
	}

	return result;
}

SubTranBlock::SubTranBlock()
{}

void
SubTranBlock::enter()
{

	if (!IsTransactionOrTransactionBlock())
		throw js_error("out of transaction");

	m_resowner = CurrentResourceOwner;
	m_mcontext = CurrentMemoryContext;
	BeginInternalSubTransaction(NULL);
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
}

JSONObject::JSONObject()
{
	Isolate* isolate = v8::Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();
	Handle<Object> global = context->Global();
	MaybeLocal<v8::Object> maybeJson = global->Get(context, String::NewFromUtf8(isolate, "JSON").ToLocalChecked()).ToLocalChecked()->ToObject(isolate->GetCurrentContext());
	if (maybeJson.IsEmpty())
		throw js_error("JSON not found");
	m_json = maybeJson.ToLocalChecked();
}

/*
 * Call JSON.parse().  Currently this supports only one argument.
 */
Handle<v8::Value>
JSONObject::Parse(Handle<v8::Value> str)
{
	Isolate* isolate = v8::Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();
	Handle<Function> parse_func =
		Handle<Function>::Cast(m_json->Get(context, String::NewFromUtf8(isolate, "parse").ToLocalChecked()).ToLocalChecked());

	if (parse_func.IsEmpty())
		throw js_error("JSON.parse() not found");

	TryCatch try_catch(isolate);
	MaybeLocal<v8::Value> value = parse_func->Call(context, m_json, 1, &str);
	if (value.IsEmpty())
		throw js_error(try_catch);
	return value.ToLocalChecked();
}

/*
 * Call JSON.stringify().  Currently this supports only one argument.
 */
Handle<v8::Value>
JSONObject::Stringify(Handle<v8::Value> val)
{
	Isolate* isolate = v8::Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();
	Handle<Function> stringify_func =
		Handle<Function>::Cast(m_json->Get(context, String::NewFromUtf8(isolate, "stringify").ToLocalChecked()).ToLocalChecked());

	if (stringify_func.IsEmpty())
		throw js_error("JSON.stringify() not found");

	TryCatch try_catch(isolate);
	MaybeLocal<v8::Value> value = stringify_func->Call(isolate->GetCurrentContext(), m_json, 1, &val);
	if (value.IsEmpty())
		throw js_error(try_catch);
	return value.ToLocalChecked();
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
	SetCallback(plv8, "get_window_object", plv8_GetWindowObject, attrFull);
	SetCallback(plv8, "quote_literal", plv8_QuoteLiteral, attrFull);
	SetCallback(plv8, "quote_nullable", plv8_QuoteNullable, attrFull);
	SetCallback(plv8, "quote_ident", plv8_QuoteIdent, attrFull);
	SetCallback(plv8, "memory_usage", plv8_MemoryUsage, attrFull);

#if PG_VERSION_NUM >= 110000
	SetCallback(plv8, "rollback", plv8_Rollback, attrFull);
	SetCallback(plv8, "commit", plv8_Commit, attrFull);
#endif
	plv8->SetInternalFieldCount(PLV8_INTNL_MAX);
}

void
SetupPrepFunctions(Handle<ObjectTemplate> templ)
{
	templ->SetInternalFieldCount(2);
	SetCallback(templ, "cursor", plv8_PlanCursor);
	SetCallback(templ, "execute", plv8_PlanExecute);
	SetCallback(templ, "free", plv8_PlanFree);
}

void
SetupCursorFunctions(Handle<ObjectTemplate> templ)
{
	templ->SetInternalFieldCount(1);
	SetCallback(templ, "fetch", plv8_CursorFetch);
	SetCallback(templ, "move", plv8_CursorMove);
	SetCallback(templ, "close", plv8_CursorClose);
}

void
SetupWindowFunctions(Handle<ObjectTemplate> templ)
{
	Isolate *isolate = Isolate::GetCurrent();

	/* We store fcinfo here. */
	templ->SetInternalFieldCount(1);

	/* Functions. */
	SetCallback(templ, "get_partition_local", plv8_WinGetPartitionLocal);
	SetCallback(templ, "set_partition_local", plv8_WinSetPartitionLocal);
	SetCallback(templ, "get_current_position", plv8_WinGetCurrentPosition);
	SetCallback(templ, "get_partition_row_count", plv8_WinGetPartitionRowCount);
	SetCallback(templ, "set_mark_position", plv8_WinSetMarkPosition);
	SetCallback(templ, "rows_are_peers", plv8_WinRowsArePeers);
	SetCallback(templ, "get_func_arg_in_partition", plv8_WinGetFuncArgInPartition);
	SetCallback(templ, "get_func_arg_in_frame", plv8_WinGetFuncArgInFrame);
	SetCallback(templ, "get_func_arg_current", plv8_WinGetFuncArgCurrent);

	/* Constants for get_func_in_XXX() */
	templ->Set(String::NewFromUtf8(isolate, "SEEK_CURRENT").ToLocalChecked(), Int32::New(isolate, WINDOW_SEEK_CURRENT));
	templ->Set(String::NewFromUtf8(isolate, "SEEK_HEAD").ToLocalChecked(), Int32::New(isolate, WINDOW_SEEK_HEAD));
	templ->Set(String::NewFromUtf8(isolate, "SEEK_TAIL").ToLocalChecked(), Int32::New(isolate, WINDOW_SEEK_TAIL));
}

/*
 * v8 is not exception-safe! We cannot throw C++ exceptions over v8 functions.
 * So, we catch C++ exceptions and convert them to JavaScript ones.
 */
static void
plv8_FunctionInvoker(const FunctionCallbackInfo<v8::Value> &args) throw()
{
	Isolate *		isolate = args.GetIsolate();
	Handle<Context> context = isolate->GetCurrentContext();
	HandleScope		handle_scope(isolate);
	MemoryContext	ctx = CurrentMemoryContext;
	FunctionCallback	fn = UnwrapCallback(args.Data());

	try
	{
		return fn(args);
	}
	catch (js_error& e)
	{
		args.GetReturnValue().Set(isolate->ThrowException(e.error_object()));
	}
	catch (pg_error& e)
	{
		MemoryContextSwitchTo(ctx);
		ErrorData *edata = CopyErrorData();

		Handle<String> message = ToString(edata->message);
		Handle<String> sqlerrcode = ToString(unpack_sql_state(edata->sqlerrcode));
#if PG_VERSION_NUM >= 90300
		Handle<v8::Value> schema_name = edata->schema_name ?
			Handle<Primitive>(ToString(edata->schema_name)) : Null(isolate);
		Handle<Primitive> table_name = edata->table_name ?
			Handle<Primitive>(ToString(edata->table_name)) : Null(isolate);
		Handle<Primitive> column_name = edata->column_name ?
			Handle<Primitive>(ToString(edata->column_name)) : Null(isolate);
		Handle<Primitive> datatype_name = edata->datatype_name ?
			Handle<Primitive>(ToString(edata->datatype_name)) : Null(isolate);
		Handle<Primitive> constraint_name = edata->constraint_name ?
			Handle<Primitive>(ToString(edata->constraint_name)) : Null(isolate);
		Handle<Primitive> detail = edata->detail ?
			Handle<Primitive>(ToString(edata->detail)) : Null(isolate);
		Handle<Primitive> hint = edata->hint ?
			Handle<Primitive>(ToString(edata->hint)) : Null(isolate);
		Handle<Primitive> sql_context = edata->context ?
			Handle<Primitive>(ToString(edata->context)) : Null(isolate);
		Handle<Primitive> internalquery = edata->internalquery ?
			Handle<Primitive>(ToString(edata->internalquery)) : Null(isolate);
		Handle<Integer> code = Uint32::New(isolate, edata->sqlerrcode);

#endif

		FlushErrorState();
		FreeErrorData(edata);

		Handle<v8::Object> err = Exception::Error(message)->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
		err->Set(context, String::NewFromUtf8(isolate, "sqlerrcode").ToLocalChecked(), sqlerrcode).Check();
#if PG_VERSION_NUM >= 90300
		err->Set(context, String::NewFromUtf8(isolate, "schema_name").ToLocalChecked(), schema_name).Check();
		err->Set(context, String::NewFromUtf8(isolate, "table_name").ToLocalChecked(), table_name).Check();
		err->Set(context, String::NewFromUtf8(isolate, "column_name").ToLocalChecked(), column_name).Check();
		err->Set(context, String::NewFromUtf8(isolate, "datatype_name").ToLocalChecked(), datatype_name).Check();
		err->Set(context, String::NewFromUtf8(isolate, "constraint_name").ToLocalChecked(), constraint_name).Check();
		err->Set(context, String::NewFromUtf8(isolate, "detail").ToLocalChecked(), detail).Check();
		err->Set(context, String::NewFromUtf8(isolate, "hint").ToLocalChecked(), hint).Check();
		err->Set(context, String::NewFromUtf8(isolate, "context").ToLocalChecked(), sql_context).Check();
		err->Set(context, String::NewFromUtf8(isolate, "internalquery").ToLocalChecked(), internalquery).Check();
		err->Set(context, String::NewFromUtf8(isolate, "code").ToLocalChecked(), code).Check();
#endif

		args.GetReturnValue().Set(isolate->ThrowException(err));
	}
}

/*
 * plv8.elog(elevel, str)
 */
static void
plv8_Elog(const FunctionCallbackInfo<v8::Value>& args)
{
	MemoryContext	ctx = CurrentMemoryContext;
	Isolate *		isolate = args.GetIsolate();

	if (args.Length() < 2) {
		args.GetReturnValue().Set(isolate->ThrowException(String::NewFromUtf8(args.GetIsolate(), "usage: plv8.elog(elevel, ...)").ToLocalChecked()));
		return;
	}

	int	elevel = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();
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
		args.GetReturnValue().Set(isolate->ThrowException(String::NewFromUtf8(args.GetIsolate(), "invalid error level").ToLocalChecked()));
		return;
	}

	std::string msg;
	std::string buf;
	for (int i = 1; i < args.Length(); i++)
	{
		if (i > 1){
			msg += " ";
		}
		//elog(NOTICE, "msg -> %s", msg.c_str());
		//elog(NOTICE, "buf -> %s", buf.c_str());

		if (!CString::toStdString(args[i],buf)){
			args.GetReturnValue().Set(Undefined(isolate));
			return;
		}
		
		CString::toStdString(args[i],buf);
		msg += buf;
	}

	const char	*message = msg.c_str();

	if (elevel != ERROR)
	{
		elog(elevel, "%s", message);
		args.GetReturnValue().Set(Undefined(isolate));
		return;
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

		args.GetReturnValue().Set(isolate->ThrowException(Exception::Error(message)));
		return;
	}
	PG_END_TRY();

	args.GetReturnValue().Set(Undefined(isolate));
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
		catch (pg_error& e){ e.rethrow(); }
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
	Isolate *isolate = Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();

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
		Handle<v8::Value>	param = params->Get(context, i).ToLocalChecked();
		values[i] = value_get_datum(param,
								  parstate.paramTypes[i], &nulls[i]);
	}
	paramLI = plv8_setup_variable_paramlist(&parstate, values, nulls);
	status = SPI_execute_plan_with_paramlist(plan, paramLI, false, 0);
#else
	Oid			   *types = (Oid *) palloc(sizeof(Oid) * nparam);

	for (int i = 0; i < nparam; i++)
	{
		Handle<v8::Value>	param = params->Get(context, i);

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

static Handle<Array>
convertArgsToArray(const FunctionCallbackInfo<v8::Value> &args, int start, int downshift)
{
	Isolate *isolate = Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();
	Local<Array> result = Array::New(args.GetIsolate(), args.Length() - start);
	for (int i = start; i < args.Length(); i++)
	{
		result->Set(context, i - downshift, args[i]).Check();
	}
	return result;
}

/*
 * plv8.execute(statement, [param, ...])
 */
static void
plv8_Execute(const FunctionCallbackInfo<v8::Value> &args)
{
	int				status;

	if (args.Length() < 1) {
		args.GetReturnValue().Set(Undefined(args.GetIsolate()));
		return;
	}

	CString			sql(args[0]);
	Handle<Array>	params;

	if (args.Length() >= 2)
	{
		if (args[1]->IsArray())
			params = Handle<Array>::Cast(args[1]);
		else /* Consume trailing elements as an array. */
			params = convertArgsToArray(args, 1, 1);
	}

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
		SPI_pop_conditional(true);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	args.GetReturnValue().Set(SPIResultToValue(status));
}

/*
 * plv8.prepare(statement, args...)
 */
static void
plv8_Prepare(const FunctionCallbackInfo<v8::Value> &args)
{
	Isolate *		isolate = args.GetIsolate();
	Handle<Context> context = isolate->GetCurrentContext();
	SPIPlanPtr		initial = NULL, saved;
	CString			sql(args[0]);
	Handle<Array>	array;
	int				arraylen = 0;
	Oid			   *types = NULL;
	plv8_param_state *parstate = NULL;

	if (args.Length() > 1)
	{
		if (args[1]->IsArray())
			array = Handle<Array>::Cast(args[1]);
		else /* Consume trailing elements as an array. */
			array = convertArgsToArray(args, 1, 0);
		arraylen = array->Length();
		types = (Oid *) palloc(sizeof(Oid) * arraylen);
	}

	for (int i = 0; i < arraylen; i++)
	{
		CString			typestr(array->Get(context, i).ToLocalChecked());
		int32			typemod;

#if PG_VERSION_NUM >= 90400
		parseTypeString(typestr, &types[i], &typemod, false);
#else
		parseTypeString(typestr, &types[i], &typemod);
#endif
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

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(isolate, current_context->plan_template);

	Local<v8::Object> result = templ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
	result->SetInternalField(0, External::New(isolate, saved));
	result->SetInternalField(1, External::New(isolate, parstate));

	args.GetReturnValue().Set(result);
}

/*
 * plan.cursor(args, ...)
 */
static void
plv8_PlanCursor(const FunctionCallbackInfo<v8::Value> &args)
{
	Isolate *			isolate = args.GetIsolate();
	Handle<Context> context = isolate->GetCurrentContext();
	Handle<v8::Object>	self = args.This();
	SPIPlanPtr			plan;
	Datum			   *values = NULL;
	char			   *nulls = NULL;
	int					nparam = 0, argcount;
	Handle<Array>		params;
	Portal				cursor;
	plv8_param_state   *parstate = NULL;

	plan = static_cast<SPIPlanPtr>(
			Handle<External>::Cast(self->GetInternalField(0))->Value());

	if (plan == NULL) {
		StringInfoData	buf;

		initStringInfo(&buf);
		appendStringInfo(&buf,
				"plan unexpectedly null");
		throw js_error(pstrdup(buf.data));
	}
	/* XXX: Add plan validation */

	if (args.Length() > 0)
	{
		if (args[0]->IsArray())
			params = Handle<Array>::Cast(args[0]);
		else
			params = convertArgsToArray(args, 0, 0);
		nparam = params->Length();
	}

	/*
	 * If the plan has the variable param info, use it.
	 */
	parstate = static_cast<plv8_param_state *>(
			Handle<External>::Cast(self->GetInternalField(1))->Value());

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
		Handle<v8::Value>	param = params->Get(context, i).ToLocalChecked();
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
	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(isolate, current_context->cursor_template);

	Local<v8::Object> result = templ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
	result->SetInternalField(0, cname);

	args.GetReturnValue().Set(result);
}

/*
 * plan.execute(args, ...)
 */
static void
plv8_PlanExecute(const FunctionCallbackInfo<v8::Value> &args)
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
	Isolate *		isolate = args.GetIsolate();
	Local<Context>		context = isolate->GetCurrentContext();


	plan = static_cast<SPIPlanPtr>(
			Handle<External>::Cast(self->GetInternalField(0))->Value());
	/* XXX: Add plan validation */

	if (args.Length() > 0)
	{
		if (args[0]->IsArray())
			params = Handle<Array>::Cast(args[0]);
		else
			params = convertArgsToArray(args, 0, 0);
		nparam = params->Length();
	}

	/*
	 * If the plan has the variable param info, use it.
	 */
	parstate = static_cast<plv8_param_state *>(
			Handle<External>::Cast(self->GetInternalField(1))->Value());

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
		Handle<v8::Value>	param = params->Get(context, i).ToLocalChecked();
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

	args.GetReturnValue().Set(SPIResultToValue(status));
	SPI_freetuptable(SPI_tuptable);
}

/*
 * plan.free()
 */
static void
plv8_PlanFree(const FunctionCallbackInfo<v8::Value> &args)
{
	Isolate *			isolate = args.GetIsolate();
	Handle<v8::Object>	self = args.This();
	SPIPlanPtr			plan;
	plv8_param_state   *parstate;
	int					status = 0;

	plan = static_cast<SPIPlanPtr>(
			Handle<External>::Cast(self->GetInternalField(0))->Value());

	if (plan)
		status = SPI_freeplan(plan);

	self->SetInternalField(0, External::New(isolate, 0));

	parstate = static_cast<plv8_param_state *>(
			Handle<External>::Cast(self->GetInternalField(1))->Value());

	if (parstate)
		pfree(parstate);
	self->SetInternalField(1, External::New(isolate, 0));

	args.GetReturnValue().Set(Int32::New(isolate, status));
}

/*
 * cursor.fetch([n])
 */
static void
plv8_CursorFetch(const FunctionCallbackInfo<v8::Value> &args)
{
	Isolate*			isolate = args.GetIsolate();
	Handle<Context> context = isolate->GetCurrentContext();
	Handle<v8::Object>	self = args.This();
	CString				cname(self->GetInternalField(0));
	Portal				cursor = SPI_cursor_find(cname);
	int					nfetch = 1;
	bool				forward = true, wantarray = false;

	if (!cursor)
		throw js_error("cannot find cursor");

	if (args.Length() >= 1)
	{
		wantarray = true;
		nfetch = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();

		if (nfetch < 0)
		{
			nfetch = -nfetch;
			forward = false;
		}
	}
	PG_TRY();
	{
		SPI_cursor_fetch(cursor, forward, nfetch);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (SPI_processed > 0)
	{
		Converter			conv(SPI_tuptable->tupdesc);

		if (!wantarray)
		{
			Handle<v8::Object>	result = conv.ToValue(SPI_tuptable->vals[0]);
			args.GetReturnValue().Set(result);
			SPI_freetuptable(SPI_tuptable);

			return;
		}
		else
		{
			Handle<Array> array = Array::New(isolate);
			for (unsigned int i = 0; i < SPI_processed; i++)
				array->Set(context, i, conv.ToValue(SPI_tuptable->vals[i])).Check();
			args.GetReturnValue().Set(array);
			SPI_freetuptable(SPI_tuptable);
			return;
		}
	}

	SPI_freetuptable(SPI_tuptable);
	args.GetReturnValue().Set(Undefined(isolate));
}

/*
 * cursor.move(n)
 */
static void
plv8_CursorMove(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*			isolate = args.GetIsolate();
	Handle<v8::Object>	self = args.This();
	CString				cname(self->GetInternalField(0));
	Portal				cursor = SPI_cursor_find(cname);
	int					nmove = 1;
	bool				forward = true;

	if (!cursor)
		throw js_error("cannot find cursor");

	if (args.Length() < 1) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	nmove = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	if (nmove < 0)
	{
		nmove = -nmove;
		forward = false;
	}

	PG_TRY();
	{
		SPI_cursor_move(cursor, forward, nmove);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(Undefined(isolate));
}

/*
 * cursor.close()
 */
static void
plv8_CursorClose(const FunctionCallbackInfo<v8::Value> &args)
{
	Handle<v8::Object>	self = args.This();
	CString				cname(self->GetInternalField(0));
	Portal				cursor = SPI_cursor_find(cname);

	if (!cursor)
		throw js_error("cannot find cursor");

	PG_TRY();
	{
		SPI_cursor_close(cursor);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(Int32::New(args.GetIsolate(), cursor ? 1 : 0));
}

/*
 * plv8.return_next(retval)
 */
static void
plv8_ReturnNext(const FunctionCallbackInfo<v8::Value>& args)
{
	Handle<v8::Object>	self = args.This();
	Handle<v8::Value>	conv_value = self->GetInternalField(PLV8_INTNL_CONV);

	if (!conv_value->IsExternal())
		throw js_error("return_next called in context that cannot accept a set");

	Converter *conv = static_cast<Converter *>(
			Handle<External>::Cast(conv_value)->Value());

	Tuplestorestate *tupstore = static_cast<Tuplestorestate *>(
			Handle<External>::Cast(
				self->GetInternalField(PLV8_INTNL_TUPSTORE))->Value());

	conv->ToDatum(args[0], tupstore);

	args.GetReturnValue().Set(Undefined(args.GetIsolate()));
}

/*
 * plv8.subtransaction(func(){ ... })
 */
static void
plv8_Subtransaction(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *		isolate = args.GetIsolate();

	if (args.Length() < 1) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}
	if (!args[0]->IsFunction()) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}
	Handle<Function>	func = Handle<Function>::Cast(args[0]);
	SubTranBlock		subtran;

	subtran.enter();

	Handle<v8::Value> emptyargs[1] = {};
	TryCatch try_catch(isolate);
	MaybeLocal<v8::Value> result = func->Call(isolate->GetCurrentContext(), func, 0, emptyargs);

	subtran.exit(!result.IsEmpty());

	if (result.IsEmpty())
		throw js_error(try_catch);
	args.GetReturnValue().Set(result.ToLocalChecked());
}

/*
 * plv8.find_function("signature")
 */
static void
plv8_FindFunction(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate			    *isolate = Isolate::GetCurrent();
	if (args.Length() < 1) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}
	CString				signature(args[0]);
	Local<Function>		func;
#if PG_VERSION_NUM < 120000
	FunctionCallInfoData fake_fcinfo;
#else
	// Stack-allocate FunctionCallInfoBaseData with
	// space for 2 arguments:
	LOCAL_FCINFO(fake_fcinfo, 2);
#endif
	FmgrInfo	flinfo;
	text *arg;

	char perm[16];
	strcpy(perm, "EXECUTE");
	arg = charToText(perm);
	Oid funcoid;

	PG_TRY();
	{
		if (strchr(signature, '(') == NULL)
			funcoid = DatumGetObjectId(
					DirectFunctionCall1(regprocin, CStringGetDatum(signature.str())));
		else
			funcoid = DatumGetObjectId(
					DirectFunctionCall1(regprocedurein, CStringGetDatum(signature.str())));

#if PG_VERSION_NUM < 120000
				MemSet(&fake_fcinfo, 0, sizeof(fake_fcinfo));
				MemSet(&flinfo, 0, sizeof(flinfo));
				fake_fcinfo.flinfo = &flinfo;
				flinfo.fn_oid = InvalidOid;
				flinfo.fn_mcxt = CurrentMemoryContext;
				fake_fcinfo.nargs = 2;
				fake_fcinfo.arg[0] = ObjectIdGetDatum(funcoid);
				fake_fcinfo.arg[1] = CStringGetDatum(arg);
				Datum ret = has_function_privilege_id(&fake_fcinfo);
#else
				MemSet(&flinfo, 0, sizeof(flinfo));
				fake_fcinfo->flinfo = &flinfo;
				flinfo.fn_oid = InvalidOid;
				flinfo.fn_mcxt = CurrentMemoryContext;
				fake_fcinfo->nargs = 2;
				fake_fcinfo->args[0].value = ObjectIdGetDatum(funcoid);
				fake_fcinfo->args[1].value = CStringGetDatum(arg);
				Datum ret = has_function_privilege_id(fake_fcinfo);
#endif

		if (ret == 0) {
			elog(WARNING, "failed to find or no permission for js function %s", signature.str());
		} else {
			if (DatumGetBool(ret)) {
				func = find_js_function(funcoid);
				if (func.IsEmpty())
					elog(ERROR, "javascript function is not found for \"%s\"", signature.str());
			} else {
				elog(WARNING, "no permission to execute js function %s", signature.str());
			}
		}
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(func);
}

/*
 * plv8.get_window_object()
 * Returns window object in window functions, which provides window function API.
 */
static void
plv8_GetWindowObject(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*			isolate = args.GetIsolate();
	Handle<v8::Object>	self = args.This();
	Handle<v8::Value>	fcinfo_value =
			self->GetInternalField(PLV8_INTNL_FCINFO);

	if (!fcinfo_value->IsExternal())
		throw js_error("get_window_object called in wrong context");
	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(isolate, current_context->window_template);

	Local<v8::Object> js_winobj = templ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
	js_winobj->SetInternalField(0, fcinfo_value);

	args.GetReturnValue().Set(js_winobj);
}

/*
 * Short-cut routine for window function API
 */
static inline WindowObject
plv8_MyWindowObject(const FunctionCallbackInfo<v8::Value>& args)
{
	Handle<v8::Object>	self = args.This();
	/* fcinfo is embedded in the internal field.  See plv8_GetWindowObject() */
	FunctionCallInfo fcinfo = static_cast<FunctionCallInfo>(
			Handle<External>::Cast(self->GetInternalField(0))->Value());

	if (fcinfo == NULL)
		throw js_error("window function api called with wrong object");

	WindowObject winobj = PG_WINDOW_OBJECT();

	if (!winobj)
		throw js_error("window function api called with wrong object");
	return winobj;
}

/*
 * Short-cut routine for window function API
 * Unfortunately, in the JS functino level we don't know the plv8 function
 * argument information enough.  Thus, we obtain it from function expression.
 */
static inline plv8_type *
plv8_MyArgType(const FunctionCallbackInfo<v8::Value>& args, int argno)
{
	Handle<v8::Object>	self = args.This();
	FunctionCallInfo fcinfo = static_cast<FunctionCallInfo>(
			Handle<External>::Cast(self->GetInternalField(0))->Value());

	if (fcinfo == NULL)
		throw js_error("window function api called with wrong object");

	/* This is safe to call in C++ context (without PG_TRY). */
	return get_plv8_type(fcinfo, argno);
}

/*
 * winobj.get_partition_local([size])
 * The default allocation size is 1K, but the caller can override this value
 * by the argument at the first call.
 */
static void
plv8_WinGetPartitionLocal(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);
	size_t			size;
	window_storage *storage;

	if (args.Length() < 1)
		size = 1000; /* default 1K */
	else
		size = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	size += sizeof(size_t) * 2;

	PG_TRY();
	{
		storage = (window_storage *) WinGetPartitionLocalMemory(winobj, size);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	/* If it's new, store the maximum size. */
	if (storage->maxlen == 0)
		storage->maxlen = size;

	/* If nothing is stored, undefined is returned. */
	if (storage->len == 0) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	/*
	 * Currently we support only serializable JSON object to be stored.
	 */
	JSONObject JSON;
	Handle<v8::Value> value = ToString(storage->data, storage->len);

	args.GetReturnValue().Set(JSON.Parse(value));
}

/*
 * winobj.set_partition_local(obj)
 * If the storage has not been allocated, it's allocated based on the
 * size of JSON-ized input string.
 */
static void
plv8_WinSetPartitionLocal(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);

	if (args.Length() < 1) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	JSONObject JSON;
	Handle<v8::Value> value = JSON.Stringify(args[0]);
	CString str(value);
	size_t str_size = strlen(str);
	size_t size = str_size + sizeof(size_t) * 2;
	window_storage *storage;

	PG_TRY();
	{
		storage = (window_storage *) WinGetPartitionLocalMemory(winobj, size);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (storage->maxlen != 0 && storage->maxlen < size)
	{
		throw js_error("window local memory overflow");
	}
	else if (storage->maxlen == 0)
	{
		/* new allocation */
		storage->maxlen = size;
	}
	storage->len = str_size;
	memcpy(storage->data, str, str_size);

	args.GetReturnValue().Set(Undefined(isolate));
}

/*
 * winobj.get_current_position()
 */
static void
plv8_WinGetCurrentPosition(const FunctionCallbackInfo<v8::Value>& args)
{
	WindowObject	winobj = plv8_MyWindowObject(args);
	int64			pos = 0;

	PG_TRY();
	{
		pos = WinGetCurrentPosition(winobj);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(Integer::New(args.GetIsolate(), pos));
}

/*
 * winobj.get_partition_row_count()
 */
static void
plv8_WinGetPartitionRowCount(const FunctionCallbackInfo<v8::Value>& args)
{
	WindowObject	winobj = plv8_MyWindowObject(args);
	int64			pos = 0;

	PG_TRY();
	{
		pos = WinGetPartitionRowCount(winobj);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(Integer::New(args.GetIsolate(), pos));
}

/*
 * winobj.set_mark_pos(pos)
 */
static void
plv8_WinSetMarkPosition(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
        }
	int64		markpos = args[0]->IntegerValue(isolate->GetCurrentContext()).ToChecked();

	PG_TRY();
	{
		WinSetMarkPosition(winobj, markpos);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

        args.GetReturnValue().Set(Undefined(isolate));
}

/*
 * winobj.rows_are_peers(pos1, pos2)
 */
static void
plv8_WinRowsArePeers(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);
	if (args.Length() < 2) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
	}
	int64		pos1 = args[0]->IntegerValue(isolate->GetCurrentContext()).ToChecked();
	int64		pos2 = args[1]->IntegerValue(isolate->GetCurrentContext()).ToChecked();
	bool		res = false;

	PG_TRY();
	{
		res = WinRowsArePeers(winobj, pos1, pos2);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(Boolean::New(isolate, res));
}

/*
 * winobj.get_func_arg_in_partition(argno, relpos, seektype, set_mark)
 */
static void
plv8_WinGetFuncArgInPartition(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);
	/* Since we return undefined in "isout" case, throw if arg isn't enough. */
	if (args.Length() < 4)
		throw js_error("argument not enough");
	int			argno = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	int			relpos = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	int			seektype = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	bool		set_mark = args[3]->BooleanValue(isolate);
	bool		isnull, isout;
	Datum		res;

	PG_TRY();
	{
		res = WinGetFuncArgInPartition(winobj,
									   argno,
									   relpos,
									   seektype,
									   set_mark,
									   &isnull,
									   &isout);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	/* Return undefined to tell it's out of partition. */
	if (isout) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	plv8_type *type = plv8_MyArgType(args, argno);

	args.GetReturnValue().Set(ToValue(res, isnull, type));
}

/*
 * winobj.get_func_arg_in_frame(argno, relpos, seektype, set_mark)
 */
static void
plv8_WinGetFuncArgInFrame(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);
	/* Since we return undefined in "isout" case, throw if arg isn't enough. */
	if (args.Length() < 4)
		throw js_error("argument not enough");
	int			argno = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	int			relpos = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	int			seektype = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	bool		set_mark = args[3]->BooleanValue(isolate);
	bool		isnull, isout;
	Datum		res;

	PG_TRY();
	{
		res = WinGetFuncArgInFrame(winobj,
								   argno,
								   relpos,
								   seektype,
								   set_mark,
								   &isnull,
								   &isout);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	/* Return undefined to tell it's out of frame. */
	if (isout) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	plv8_type *type = plv8_MyArgType(args, argno);

	args.GetReturnValue().Set(ToValue(res, isnull, type));
}

/*
 * winobj.get_func_arg_current(argno)
 */
static void
plv8_WinGetFuncArgCurrent(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	WindowObject	winobj = plv8_MyWindowObject(args);
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
        }
	int			argno = args[0]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	bool		isnull;
	Datum		res;

	PG_TRY();
	{
		res = WinGetFuncArgCurrent(winobj,
								   argno,
								   &isnull);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	plv8_type *type = plv8_MyArgType(args, argno);

	args.GetReturnValue().Set(ToValue(res, isnull, type));
}

/*
 * plv8.quote_literal(str)
 */
static void
plv8_QuoteLiteral(const FunctionCallbackInfo<v8::Value>& args)
{
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(args.GetIsolate()));
		return;
	}
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

	args.GetReturnValue().Set(ToString(result));
}

/*
 * plv8.quote_nullable(str)
 */
static void
plv8_QuoteNullable(const FunctionCallbackInfo<v8::Value>& args)
{
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(args.GetIsolate()));
		return;
	}
	CString			instr(args[0]);
	char		   *result;

	if (args[0]->IsNull() || args[0]->IsUndefined()) {
		args.GetReturnValue().Set(ToString("NULL"));
		return;
	}

	PG_TRY();
	{
		result = quote_literal_cstr(instr);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	args.GetReturnValue().Set(ToString(result));
}

/*
 * plv8.quote_ident(str)
 */
static void
plv8_QuoteIdent(const FunctionCallbackInfo<v8::Value>& args)
{
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(args.GetIsolate()));
		return;
	}
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

	args.GetReturnValue().Set(ToString(result));
}

static void
plv8_MemoryUsage(const FunctionCallbackInfo<v8::Value>& args)
{
	// V8 memory usage
  	HeapStatistics v8_heap_stats;
	Isolate *		isolate = args.GetIsolate();
	isolate->GetHeapStatistics(&v8_heap_stats);

	Local<v8::Value>	result;
	Local<v8::Object>	obj = v8::Object::New(isolate);

	GetMemoryInfo(obj);
	result = obj;
	args.GetReturnValue().Set(result);
}

void GetMemoryInfo(v8::Local<v8::Object> obj) {
	HeapStatistics  	v8_heap_stats;
	Isolate 		   *isolate = obj->GetIsolate();
	Handle<Context> context = isolate->GetCurrentContext();

	isolate->GetHeapStatistics(&v8_heap_stats);

	Local<v8::Value> total = Local<v8::Value>::New(isolate, Number::New(isolate, v8_heap_stats.total_heap_size()));
	Local<v8::Value> used = Local<v8::Value>::New(isolate, Number::New(isolate, v8_heap_stats.used_heap_size()));
	Local<v8::Value> external = Local<v8::Value>::New(isolate, Number::New(isolate, v8_heap_stats.external_memory()));

	obj->Set(context, String::NewFromUtf8(isolate, "total_heap_size").ToLocalChecked(), total).Check();
	obj->Set(context, String::NewFromUtf8(isolate, "used_heap_size").ToLocalChecked(), used).Check();
	obj->Set(context, String::NewFromUtf8(isolate, "external_memory").ToLocalChecked(), external).Check();
}

#if PG_VERSION_NUM >= 110000

static void
plv8_Commit(const FunctionCallbackInfo<v8::Value> &args)
{
	PG_TRY();
	{
		HoldPinnedPortals();
		SPI_commit();
		SPI_start_transaction();
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();
}

static void
plv8_Rollback(const FunctionCallbackInfo<v8::Value> &args)
{
	PG_TRY();
	{
		HoldPinnedPortals();
		SPI_rollback();
		SPI_start_transaction();
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();
}

#endif
