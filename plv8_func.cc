/*-------------------------------------------------------------------------
 *
 * plv8_func.cc : PL/v8 built-in functions.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"
#include "plv8_param.h"
#include <span>
#include <string>
#include <variant>

extern "C" {
#define String PG_Node_String
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "miscadmin.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "nodes/memnodes.h"
#undef String
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
static void plv8_Commit(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_Rollback(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_CompileWasmCached(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_ExpectedColumns(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_ReturnQuery(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_RegisterColumnInference(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_RegisterSupport(const FunctionCallbackInfo<v8::Value>& args);
static void plv8_SetFunctionColumns(const FunctionCallbackInfo<v8::Value>& args);

const intptr_t plv8_external_references[] = {
	reinterpret_cast<intptr_t>(plv8_FunctionInvoker),
	reinterpret_cast<intptr_t>(plv8_Elog),
	reinterpret_cast<intptr_t>(plv8_Execute),
	reinterpret_cast<intptr_t>(plv8_Prepare),
	reinterpret_cast<intptr_t>(plv8_PlanCursor),
	reinterpret_cast<intptr_t>(plv8_PlanExecute),
	reinterpret_cast<intptr_t>(plv8_PlanFree),
	reinterpret_cast<intptr_t>(plv8_CursorFetch),
	reinterpret_cast<intptr_t>(plv8_CursorMove),
	reinterpret_cast<intptr_t>(plv8_CursorClose),
	reinterpret_cast<intptr_t>(plv8_ReturnNext),
	reinterpret_cast<intptr_t>(plv8_Subtransaction),
	reinterpret_cast<intptr_t>(plv8_FindFunction),
	reinterpret_cast<intptr_t>(plv8_GetWindowObject),
	reinterpret_cast<intptr_t>(plv8_WinGetPartitionLocal),
	reinterpret_cast<intptr_t>(plv8_WinSetPartitionLocal),
	reinterpret_cast<intptr_t>(plv8_WinGetCurrentPosition),
	reinterpret_cast<intptr_t>(plv8_WinGetPartitionRowCount),
	reinterpret_cast<intptr_t>(plv8_WinSetMarkPosition),
	reinterpret_cast<intptr_t>(plv8_WinRowsArePeers),
	reinterpret_cast<intptr_t>(plv8_WinGetFuncArgInPartition),
	reinterpret_cast<intptr_t>(plv8_WinGetFuncArgInFrame),
	reinterpret_cast<intptr_t>(plv8_WinGetFuncArgCurrent),
	reinterpret_cast<intptr_t>(plv8_QuoteLiteral),
	reinterpret_cast<intptr_t>(plv8_QuoteNullable),
	reinterpret_cast<intptr_t>(plv8_QuoteIdent),
	reinterpret_cast<intptr_t>(plv8_MemoryUsage),
	reinterpret_cast<intptr_t>(plv8_Commit),
	reinterpret_cast<intptr_t>(plv8_Rollback),
	reinterpret_cast<intptr_t>(plv8_CompileWasmCached),
	reinterpret_cast<intptr_t>(plv8_Require),
	reinterpret_cast<intptr_t>(plv8_Import),
	reinterpret_cast<intptr_t>(plv8_CompileBytecode),
	reinterpret_cast<intptr_t>(plv8_ExpectedColumns),
	reinterpret_cast<intptr_t>(plv8_ReturnQuery),
	reinterpret_cast<intptr_t>(plv8_RegisterColumnInference),
	reinterpret_cast<intptr_t>(plv8_RegisterSupport),
	reinterpret_cast<intptr_t>(plv8_SetFunctionColumns),
	0
};

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

static inline Local<v8::Value>
WrapCallback(FunctionCallback func)
{
	Isolate* isolate = Isolate::GetCurrent();
	return External::New(isolate,
			reinterpret_cast<void *>(
				reinterpret_cast<uintptr_t>(func)),
			v8::kExternalPointerTypeTagDefault);
}

static inline FunctionCallback
UnwrapCallback(Handle<v8::Value> value)
{
	return reinterpret_cast<FunctionCallback>(
			reinterpret_cast<uintptr_t>(External::Cast(*value)->Value(v8::kExternalPointerTypeTagDefault)));
}

static inline void
SetCallback(Handle<ObjectTemplate> obj, const char *name,
			FunctionCallback func, PropertyAttribute attr = None)
{
	Isolate* isolate = Isolate::GetCurrent();
	obj->Set(isolate, name,
				FunctionTemplate::New(isolate, plv8_FunctionInvoker,
					WrapCallback(func)), attr);
}

class SubTranBlock
{
private:
	ResourceOwner		m_resowner;
	MemoryContext		m_mcontext;
	bool				m_entered;
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
		isolate->ThrowException(v8::String::NewFromUtf8(isolate, FormatSPIStatus(status)).ToLocalChecked());
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
			rows->CreateDataProperty(context, r, conv.ToValue(SPI_tuptable->vals[r])).Check();

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
	: m_resowner(NULL),
	  m_mcontext(NULL),
	  m_entered(false)
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
	m_entered = true;
}

void
SubTranBlock::exit(bool success)
{
	if (!m_entered)
		return;

	m_entered = false;
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
	TryCatch try_catch(isolate);
	Local<v8::Value> json_val;
	if (!global->Get(context, v8::String::NewFromUtf8Literal(isolate, "JSON")).ToLocal(&json_val) ||
		json_val.IsEmpty() || !json_val->IsObject())
	{
		if (try_catch.HasCaught())
			throw js_error(try_catch);
		throw js_error("JSON not found");
	}
	m_json = Local<v8::Object>::Cast(json_val);
}

/*
 * Call JSON.parse().  Currently this supports only one argument.
 */
Handle<v8::Value>
JSONObject::Parse(Handle<v8::Value> str)
{
	Isolate* isolate = v8::Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();
	TryCatch try_catch(isolate);
	Local<v8::Value> parse_val;
	if (!m_json->Get(context, v8::String::NewFromUtf8Literal(isolate, "parse")).ToLocal(&parse_val) ||
		parse_val.IsEmpty() || !parse_val->IsFunction())
	{
		if (try_catch.HasCaught())
			throw js_error(try_catch);
		throw js_error("JSON.parse() not found");
	}
	Handle<Function> parse_func = Handle<Function>::Cast(parse_val);

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
	TryCatch try_catch(isolate);
	Local<v8::Value> stringify_val;
	if (!m_json->Get(context, v8::String::NewFromUtf8Literal(isolate, "stringify")).ToLocal(&stringify_val) ||
		stringify_val.IsEmpty() || !stringify_val->IsFunction())
	{
		if (try_catch.HasCaught())
			throw js_error(try_catch);
		throw js_error("JSON.stringify() not found");
	}
	Handle<Function> stringify_func = Handle<Function>::Cast(stringify_val);

	MaybeLocal<v8::Value> value = stringify_func->Call(context, m_json, 1, &val);
	if (value.IsEmpty())
		throw js_error(try_catch);
	return value.ToLocalChecked();
}

static void
plv8_CompileWasmCached(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();
	Local<Context> context = isolate->GetCurrentContext();

	if (args.Length() < 1)
		throw js_error("plv8.compile_wasm_cached([cache_key, ]wasm_bytes) requires a byte buffer");

	Local<v8::Value> bytes_arg;
	if (args.Length() >= 2 && args[0]->IsString())
	{
		v8::String::Utf8Value key_utf8(isolate, args[0]);
		if (*key_utf8 == nullptr || key_utf8.length() == 0)
			throw js_error("plv8.compile_wasm_cached cache_key must be non-empty");
		bytes_arg = args[1];
	}
	else
	{
		bytes_arg = args[0];
	}

	const uint8_t *wire_ptr = nullptr;
	size_t wire_len = 0;

	if (bytes_arg->IsArrayBufferView())
	{
		Local<ArrayBufferView> view = Local<ArrayBufferView>::Cast(bytes_arg);
		wire_ptr = static_cast<const uint8_t *>(view->Buffer()->GetBackingStore()->Data()) + view->ByteOffset();
		wire_len = view->ByteLength();
	}
	else if (bytes_arg->IsArrayBuffer())
	{
		Local<ArrayBuffer> ab = Local<ArrayBuffer>::Cast(bytes_arg);
		wire_ptr = static_cast<const uint8_t *>(ab->GetBackingStore()->Data());
		wire_len = ab->ByteLength();
	}
	else
	{
		throw js_error("plv8.compile_wasm_cached requires an ArrayBuffer or ArrayBufferView");
	}

	if (wire_ptr == nullptr || wire_len == 0)
		throw js_error("plv8.compile_wasm_cached received empty WebAssembly wire bytes");

	std::vector<uint8_t> cached_compiled_bytes;
	if (plv8_wasm_cache_lookup(wire_ptr, wire_len, &cached_compiled_bytes) &&
		!cached_compiled_bytes.empty())
	{
		v8::WasmModuleCompilation compilation;
		compilation.SetHasCompiledModuleBytes();
		compilation.OnBytesReceived(wire_ptr, wire_len);

		v8::Local<v8::WasmModuleObject> mod_obj;
		bool deserialized_ok = false;

		compilation.Finish(
			isolate,
			[&](v8::WasmStreaming::ModuleCachingInterface &caching) {
				deserialized_ok = caching.SetCachedCompiledModuleBytes(
					std::span<const uint8_t>(cached_compiled_bytes.data(),
											 cached_compiled_bytes.size()));
			},
			[&](std::variant<v8::Local<v8::WasmModuleObject>, v8::Local<v8::Value>> res) {
				if (std::holds_alternative<v8::Local<v8::WasmModuleObject>>(res))
					mod_obj = std::get<v8::Local<v8::WasmModuleObject>>(res);
			});

		if (deserialized_ok && !mod_obj.IsEmpty())
		{
			args.GetReturnValue().Set(mod_obj);
			return;
		}
	}

	v8::MaybeLocal<v8::WasmModuleObject> maybe_mod =
		v8::WasmModuleObject::Compile(
			isolate, std::span<const uint8_t>(wire_ptr, wire_len));
	if (maybe_mod.IsEmpty())
	{
		return;
	}

	v8::Local<v8::WasmModuleObject> mod_obj = maybe_mod.ToLocalChecked();
	v8::CompiledWasmModule compiled = mod_obj->GetCompiledModule();
	v8::OwnedBuffer serialized = compiled.Serialize();
	if (serialized.buffer && serialized.size > 0)
	{
		plv8_wasm_cache_store(wire_ptr, wire_len, serialized.buffer.get(), serialized.size);
	}

	args.GetReturnValue().Set(mod_obj);
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
	SetCallback(plv8, "rollback", plv8_Rollback, attrFull);
	SetCallback(plv8, "commit", plv8_Commit, attrFull);
	SetCallback(plv8, "compile_wasm_cached", plv8_CompileWasmCached, attrFull);
	SetCallback(plv8, "require", plv8_Require, attrFull);
	SetCallback(plv8, "import", plv8_Import, attrFull);
	SetCallback(plv8, "compile_bytecode", plv8_CompileBytecode, attrFull);
	SetCallback(plv8, "expected_columns", plv8_ExpectedColumns, attrFull);
	SetCallback(plv8, "return_query", plv8_ReturnQuery, attrFull);
	SetCallback(plv8, "register_column_inference", plv8_RegisterColumnInference, attrFull);
	SetCallback(plv8, "register_support", plv8_RegisterSupport, attrFull);
	SetCallback(plv8, "set_function_columns", plv8_SetFunctionColumns, attrFull);

	plv8->SetInternalFieldCount(PLV8_INTNL_MAX);
}

void
SetupGlobalFunctions(Handle<ObjectTemplate> global)
{
	PropertyAttribute	attrFull =
		PropertyAttribute(ReadOnly | DontEnum | DontDelete);

	SetCallback(global, "require", plv8_Require, attrFull);
}

void
SetupPrepFunctions(Handle<ObjectTemplate> templ)
{
	PropertyAttribute attrFull =
		PropertyAttribute(ReadOnly | DontEnum | DontDelete);

	templ->SetInternalFieldCount(3);
	SetCallback(templ, "cursor", plv8_PlanCursor, attrFull);
	SetCallback(templ, "execute", plv8_PlanExecute, attrFull);
	SetCallback(templ, "free", plv8_PlanFree, attrFull);
}

void
SetupCursorFunctions(Handle<ObjectTemplate> templ)
{
	PropertyAttribute attrFull =
		PropertyAttribute(ReadOnly | DontEnum | DontDelete);

	templ->SetInternalFieldCount(1);
	SetCallback(templ, "fetch", plv8_CursorFetch, attrFull);
	SetCallback(templ, "move", plv8_CursorMove, attrFull);
	SetCallback(templ, "close", plv8_CursorClose, attrFull);
}

void
SetupWindowFunctions(Handle<ObjectTemplate> templ)
{
	Isolate *isolate = Isolate::GetCurrent();
	PropertyAttribute attrFull =
		PropertyAttribute(ReadOnly | DontEnum | DontDelete);

	/* We store fcinfo here. */
	templ->SetInternalFieldCount(1);

	/* Functions. */
	SetCallback(templ, "get_partition_local", plv8_WinGetPartitionLocal, attrFull);
	SetCallback(templ, "set_partition_local", plv8_WinSetPartitionLocal, attrFull);
	SetCallback(templ, "get_current_position", plv8_WinGetCurrentPosition, attrFull);
	SetCallback(templ, "get_partition_row_count", plv8_WinGetPartitionRowCount, attrFull);
	SetCallback(templ, "set_mark_position", plv8_WinSetMarkPosition, attrFull);
	SetCallback(templ, "rows_are_peers", plv8_WinRowsArePeers, attrFull);
	SetCallback(templ, "get_func_arg_in_partition", plv8_WinGetFuncArgInPartition, attrFull);
	SetCallback(templ, "get_func_arg_in_frame", plv8_WinGetFuncArgInFrame, attrFull);
	SetCallback(templ, "get_func_arg_current", plv8_WinGetFuncArgCurrent, attrFull);

	/* Constants for get_func_in_XXX() */
	templ->Set(v8::String::NewFromUtf8(isolate, "SEEK_CURRENT").ToLocalChecked(), Int32::New(isolate, WINDOW_SEEK_CURRENT), attrFull);
	templ->Set(v8::String::NewFromUtf8(isolate, "SEEK_HEAD").ToLocalChecked(), Int32::New(isolate, WINDOW_SEEK_HEAD), attrFull);
	templ->Set(v8::String::NewFromUtf8(isolate, "SEEK_TAIL").ToLocalChecked(), Int32::New(isolate, WINDOW_SEEK_TAIL), attrFull);
}

/*
 * v8 is not exception-safe! We cannot throw C++ exceptions over v8 functions.
 * So, we catch C++ exceptions and convert them to JavaScript ones.
 */
static void
plv8_FunctionInvoker(const FunctionCallbackInfo<v8::Value> &args) throw()
{
	PLV8_ASSERT_MAIN_PG_THREAD();
	Isolate *		isolate = args.GetIsolate();
	Handle<Context> context = isolate->GetCurrentContext();
	HandleScope		handle_scope(isolate);
	MemoryContext	ctx = CurrentMemoryContext;
	FunctionCallback	fn = UnwrapCallback(args.Data());
	/*
	 * A callback that runs SQL may call into another plv8 function under a
	 * different user (SECURITY DEFINER), which switches current_context and,
	 * if it errors out, never gets to switch it back.  Restore it here so
	 * the JavaScript that continues after this callback still sees its own
	 * context.
	 */
	CurrentContextScope	context_scope(current_context);

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

		Handle<v8::String> message = ToString(edata->message);
		Handle<v8::String> sqlerrcode = ToString(unpack_sql_state(edata->sqlerrcode));

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
		Handle<v8::Integer> code = Uint32::New(isolate, edata->sqlerrcode);

		FlushErrorState();
		FreeErrorData(edata);

		Handle<v8::Object> err = Exception::Error(message)->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
		err->Set(context, v8::String::NewFromUtf8(isolate, "sqlerrcode").ToLocalChecked(), sqlerrcode).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "schema_name").ToLocalChecked(), schema_name).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "table_name").ToLocalChecked(), table_name).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "column_name").ToLocalChecked(), column_name).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "datatype_name").ToLocalChecked(), datatype_name).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "constraint_name").ToLocalChecked(), constraint_name).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "detail").ToLocalChecked(), detail).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "hint").ToLocalChecked(), hint).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "context").ToLocalChecked(), sql_context).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "internalquery").ToLocalChecked(), internalquery).Check();
		err->Set(context, v8::String::NewFromUtf8(isolate, "code").ToLocalChecked(), code).Check();

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
	Local<Context>	context = isolate->GetCurrentContext();

	if (args.Length() < 2) {
		args.GetReturnValue().Set(isolate->ThrowException(v8::String::NewFromUtf8(args.GetIsolate(), "usage: plv8.elog(elevel, ...)").ToLocalChecked()));
		return;
	}

	int	elevel = Plv8ToInt32(isolate, context, args[0]);
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
		args.GetReturnValue().Set(isolate->ThrowException(v8::String::NewFromUtf8(args.GetIsolate(), "invalid error level").ToLocalChecked()));
		return;
	}

	std::string msg;
	std::string buf;
	for (int i = 1; i < args.Length(); i++)
	{
		if (i > 1){
			msg += " ";
		}

		if (!CString::toStdString(args[i],buf)){
			args.GetReturnValue().Set(Undefined(isolate));
			return;
		}

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
		Local<v8::String> message = ToString(edata->message);
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
	if (value.IsEmpty() || value->IsUndefined() || value->IsNull())
	{
		*isnull = 'n';
		return (Datum) 0;
	}
	else
	{
		plv8_type	typinfo = { 0 };
		bool		IsNull = false;
		Datum		datum = (Datum) 0;
		bool		has_js_err = false;
		bool		has_pg_err = false;
		js_error	js_err;
		pg_error	pg_err;

		plv8_fill_type(&typinfo, typid);
		try
		{
			datum = ToDatum(value, &IsNull, &typinfo);
		}
		catch (js_error& e)
		{
			js_err = e;
			has_js_err = true;
		}
		catch (pg_error& e)
		{
			pg_err = e;
			has_pg_err = true;
		}
		if (has_js_err)
			js_err.rethrow();
		if (has_pg_err)
			pg_err.rethrow();
		*isnull = (IsNull ?  'n' : ' ');
		return datum;
	}
}

static Handle<v8::Value>
param_get_element(Isolate *isolate, Local<Context> context, Handle<Array> params, int index)
{
	Handle<v8::Value>	param;
	bool				has_js_err = false;
	js_error			js_err;

	try
	{
		param = Plv8GetElement(isolate, context, params, index);
	}
	catch (js_error& e)
	{
		js_err = e;
		has_js_err = true;
	}
	if (has_js_err)
		js_err.rethrow();
	return param;
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
		Handle<v8::Value>	param = param_get_element(isolate, context, params, i);
		values[i] = value_get_datum(param,
								  parstate.paramTypes[i], &nulls[i]);
	}
	paramLI = plv8_setup_variable_paramlist(&parstate, values, nulls);
	status = SPI_execute_plan_with_paramlist(plan, paramLI, false, 0);

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
		result->CreateDataProperty(context, i - downshift, args[i]).Check();
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
	char		  **typestrs = NULL;
	plv8_param_state *parstate = NULL;

	if (args.Length() > 1)
	{
		if (args[1]->IsArray())
			array = Handle<Array>::Cast(args[1]);
		else /* Consume trailing elements as an array. */
			array = convertArgsToArray(args, 1, 0);
		arraylen = array->Length();
		types = (Oid *) palloc(sizeof(Oid) * arraylen);
		typestrs = (char **) palloc(sizeof(char *) * arraylen);
	}

	for (int i = 0; i < arraylen; i++)
	{
		CString			typestr(Plv8GetElement(isolate, context, array, i));
		typestrs[i] = pstrdup(typestr);
	}

	SubTranBlock	subtran;
	PG_TRY();
	{
		subtran.enter();
		for (int i = 0; i < arraylen; i++)
		{
			int32			typemod;

#if PG_VERSION_NUM >= 160000
			parseTypeString(typestrs[i], &types[i], &typemod, NULL);
#else
			parseTypeString(typestrs[i], &types[i], &typemod, false);
#endif
		}
		if (args.Length() == 1)
		{
			parstate =
				(plv8_param_state *) palloc0(sizeof(plv8_param_state));
			parstate->memcontext = CurrentMemoryContext;
			initial = SPI_prepare_params(sql, plv8_variable_param_setup,
										 parstate, 0);
		}
		else
			initial = SPI_prepare(sql, arraylen, types);
		saved = SPI_saveplan(initial);
		SPI_freeplan(initial);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(isolate, current_context->plan_template);

	Local<v8::Object> result = templ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
	result->SetInternalField(0, External::New(isolate, saved, v8::kExternalPointerTypeTagDefault));
	result->SetInternalField(1, External::New(isolate, parstate, v8::kExternalPointerTypeTagDefault));
	result->SetInternalField(2, Int32::New(isolate, 0));

	args.GetReturnValue().Set(result);
}

static void
check_plan_this(const FunctionCallbackInfo<v8::Value> &args)
{
	Isolate *isolate = args.GetIsolate();
	Handle<v8::Object> self = args.This();

	if (current_context == nullptr || current_context->plan_ftmpl.IsEmpty() ||
		self.IsEmpty() || !current_context->plan_ftmpl.Get(isolate)->HasInstance(self) ||
		self->InternalFieldCount() < 3 ||
		!self->GetInternalField(0).As<v8::Value>()->IsExternal() ||
		!self->GetInternalField(1).As<v8::Value>()->IsExternal())
	{
		throw js_error("unexpected type");
	}
}

class PlanInUseGuard
{
private:
	Isolate *m_isolate;
	Local<v8::Object> m_self;

public:
	PlanInUseGuard(Isolate *isolate, Local<v8::Object> self)
		: m_isolate(isolate), m_self(self)
	{
		int32_t in_use = m_self->GetInternalField(2).As<v8::Int32>()->Value();
		m_self->SetInternalField(2, Int32::New(m_isolate, in_use + 1));
	}
	~PlanInUseGuard()
	{
		int32_t in_use = m_self->GetInternalField(2).As<v8::Int32>()->Value();
		m_self->SetInternalField(2, Int32::New(m_isolate, in_use - 1));
	}
};

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

	check_plan_this(args);
	PlanInUseGuard		in_use_guard(isolate, self);

	plan = static_cast<SPIPlanPtr>(
			self->GetInternalField(0).As<External>()->Value(v8::kExternalPointerTypeTagDefault));

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
			self->GetInternalField(1).As<External>()->Value(v8::kExternalPointerTypeTagDefault));

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

	SubTranBlock		subtran;
	PG_TRY();
	{
		subtran.enter();
		for (int i = 0; i < nparam; i++)
		{
			Handle<v8::Value>	param = param_get_element(isolate, context, params, i);
			Oid					typid;

			if (parstate)
				typid = parstate->paramTypes[i];
			else
				typid = SPI_getargtypeid(plan, i);

			values[i] = value_get_datum(param, typid, &nulls[i]);
		}
		if (parstate)
		{
			ParamListInfo	paramLI;

			paramLI = plv8_setup_variable_paramlist(parstate, values, nulls);
			cursor = SPI_cursor_open_with_paramlist(NULL, plan, paramLI, false);
		}
		else
			cursor = SPI_cursor_open(NULL, plan, values, nulls, false);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	Handle<v8::String> cname = ToString(cursor->name, strlen(cursor->name));
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

	check_plan_this(args);
	PlanInUseGuard		in_use_guard(isolate, self);

	plan = static_cast<SPIPlanPtr>(
			self->GetInternalField(0).As<External>()->Value(v8::kExternalPointerTypeTagDefault));
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
			self->GetInternalField(1).As<External>()->Value(v8::kExternalPointerTypeTagDefault));

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

	if (plan == NULL)
	{
		throw js_error("plan unexpectedly null");
	}

	if (nparam > 0)
	{
		values = (Datum *) palloc(sizeof(Datum) * nparam);
		nulls = (char *) palloc(sizeof(char) * nparam);
	}

	PG_TRY();
	{
		subtran.enter();
		for (int i = 0; i < nparam; i++)
		{
			Handle<v8::Value>	param = param_get_element(isolate, context, params, i);
			Oid					typid;

			if (parstate)
				typid = parstate->paramTypes[i];
			else
				typid = SPI_getargtypeid(plan, i);

			values[i] = value_get_datum(param, typid, &nulls[i]);
		}
		if (parstate)
		{
			ParamListInfo	paramLI;

			paramLI = plv8_setup_variable_paramlist(parstate, values, nulls);
			status = SPI_execute_plan_with_paramlist(plan, paramLI, false, 0);
		}
		else
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

	check_plan_this(args);
	if (self->GetInternalField(2).As<v8::Int32>()->Value() != 0)
		throw js_error("cannot free a plan in use");

	plan = static_cast<SPIPlanPtr>(
			self->GetInternalField(0).As<External>()->Value(v8::kExternalPointerTypeTagDefault));

	if (plan)
		status = SPI_freeplan(plan);

	self->SetInternalField(0, External::New(isolate, 0, v8::kExternalPointerTypeTagDefault));

	parstate = static_cast<plv8_param_state *>(
			self->GetInternalField(1).As<External>()->Value(v8::kExternalPointerTypeTagDefault));

	if (parstate)
		pfree(parstate);
	self->SetInternalField(1, External::New(isolate, 0, v8::kExternalPointerTypeTagDefault));

	args.GetReturnValue().Set(Int32::New(isolate, status));
}

static void
check_cursor_this(const FunctionCallbackInfo<v8::Value> &args)
{
	Isolate *isolate = args.GetIsolate();
	Handle<v8::Object> self = args.This();

	if (current_context == nullptr || current_context->cursor_ftmpl.IsEmpty() ||
		self.IsEmpty() || !current_context->cursor_ftmpl.Get(isolate)->HasInstance(self) ||
		self->InternalFieldCount() < 1)
	{
		throw js_error("cannot find cursor");
	}
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

	check_cursor_this(args);

	CString				cname(self->GetInternalField(0).As<v8::Value>());
	Portal				cursor = SPI_cursor_find(cname);
	int					nfetch = 1;
	bool				forward = true, wantarray = false;
	SubTranBlock		subtran;

	if (!cursor)
		throw js_error("cannot find cursor");

	if (args.Length() >= 1)
	{
		wantarray = true;
		nfetch = Plv8ToInt32(isolate, context, args[0]);

		if (nfetch < 0)
		{
			nfetch = -nfetch;
			forward = false;
		}
	}
	PG_TRY();
	{
		subtran.enter();
		SPI_cursor_fetch(cursor, forward, nfetch);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

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
				array->CreateDataProperty(context, i, conv.ToValue(SPI_tuptable->vals[i])).Check();
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
	Local<Context>		context = isolate->GetCurrentContext();
	Handle<v8::Object>	self = args.This();

	check_cursor_this(args);

	CString				cname(self->GetInternalField(0).As<v8::Value>());
	Portal				cursor = SPI_cursor_find(cname);
	int					nmove = 1;
	bool				forward = true;
	SubTranBlock		subtran;

	if (!cursor)
		throw js_error("cannot find cursor");

	if (args.Length() < 1) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	nmove = Plv8ToInt32(isolate, context, args[0]);
	if (nmove < 0)
	{
		nmove = -nmove;
		forward = false;
	}

	PG_TRY();
	{
		subtran.enter();
		SPI_cursor_move(cursor, forward, nmove);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	args.GetReturnValue().Set(Undefined(isolate));
}

/*
 * cursor.close()
 */
static void
plv8_CursorClose(const FunctionCallbackInfo<v8::Value> &args)
{
	Handle<v8::Object>	self = args.This();

	check_cursor_this(args);

	CString				cname(self->GetInternalField(0).As<v8::Value>());
	Portal				cursor = SPI_cursor_find(cname);
	SubTranBlock		subtran;

	if (!cursor)
		throw js_error("cannot find cursor");

	PG_TRY();
	{
		subtran.enter();
		SPI_cursor_close(cursor);
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);

	args.GetReturnValue().Set(Int32::New(args.GetIsolate(), cursor ? 1 : 0));
}

static bool
is_valid_plv8_this(Isolate *isolate, Handle<v8::Object> self)
{
	if (self.IsEmpty() || self->InternalFieldCount() < PLV8_INTNL_MAX)
		return false;
	if (current_context != nullptr && !current_context->plv8_obj.IsEmpty())
		return self->StrictEquals(current_context->plv8_obj.Get(isolate));
	return true;
}

/*
 * plv8.return_next(retval)
 */
static void
plv8_ReturnNext(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();
	Handle<v8::Object>	self = args.This();

	if (!is_valid_plv8_this(isolate, self))
		throw js_error("return_next called in context that cannot accept a set");

	Handle<v8::Value>	conv_value = self->GetInternalField(PLV8_INTNL_CONV).As<v8::Value>();
	Handle<v8::Value>	tupstore_value = self->GetInternalField(PLV8_INTNL_TUPSTORE).As<v8::Value>();

	if (!conv_value->IsExternal() || !tupstore_value->IsExternal())
		throw js_error("return_next called in context that cannot accept a set");

	Converter *conv = static_cast<Converter *>(
			Handle<External>::Cast(conv_value)->Value(v8::kExternalPointerTypeTagDefault));

	Tuplestorestate *tupstore = static_cast<Tuplestorestate *>(
			Handle<External>::Cast(tupstore_value)->Value(v8::kExternalPointerTypeTagDefault));

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
	// Stack-allocate FunctionCallInfoBaseData with
	// space for 2 arguments:
	LOCAL_FCINFO(fake_fcinfo, 2);

	FmgrInfo	flinfo;
	text *arg;

	char perm[16];
	strcpy(perm, "EXECUTE");
	arg = charToText(perm);
	Oid funcoid = InvalidOid;
	bool has_priv = false;

	PG_TRY();
	{
		if (strchr(signature, '(') == NULL)
			funcoid = DatumGetObjectId(
					DirectFunctionCall1(regprocin, CStringGetDatum(signature.str())));
		else
			funcoid = DatumGetObjectId(
					DirectFunctionCall1(regprocedurein, CStringGetDatum(signature.str())));

		MemSet(&flinfo, 0, sizeof(flinfo));
		fake_fcinfo->flinfo = &flinfo;
		flinfo.fn_oid = InvalidOid;
		flinfo.fn_mcxt = CurrentMemoryContext;
		fake_fcinfo->nargs = 2;
		fake_fcinfo->args[0].value = ObjectIdGetDatum(funcoid);
		fake_fcinfo->args[1].value = PointerGetDatum(arg);
		Datum ret = has_function_privilege_id(fake_fcinfo);

		if (ret == 0) {
			elog(WARNING, "failed to find or no permission for js function %s", signature.str());
		} else {
			if (DatumGetBool(ret)) {
				has_priv = true;
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

	if (has_priv)
	{
		func = find_js_function(funcoid);
		if (func.IsEmpty())
		{
			PG_TRY();
			{
				elog(ERROR, "javascript function is not found for \"%s\"", signature.str());
			}
			PG_CATCH();
			{
				throw pg_error();
			}
			PG_END_TRY();
		}
	}

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

	if (!is_valid_plv8_this(isolate, self))
		throw js_error("get_window_object called in wrong context");

	Handle<v8::Value>	fcinfo_value =
			self->GetInternalField(PLV8_INTNL_FCINFO).As<v8::Value>();

	if (!fcinfo_value->IsExternal())
		throw js_error("get_window_object called in wrong context");
	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(isolate, current_context->window_template);

	Local<v8::Object> js_winobj = templ->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
	js_winobj->SetInternalField(0, fcinfo_value);

	args.GetReturnValue().Set(js_winobj);
}

static FunctionCallInfo
check_window_this(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();
	Handle<v8::Object> self = args.This();

	if (current_context == nullptr || current_context->window_ftmpl.IsEmpty() ||
		self.IsEmpty() || !current_context->window_ftmpl.Get(isolate)->HasInstance(self) ||
		self->InternalFieldCount() < 1)
	{
		throw js_error("window function api called with wrong object");
	}

	Handle<v8::Value> field0 = self->GetInternalField(0).As<v8::Value>();
	if (field0.IsEmpty() || !field0->IsExternal())
		throw js_error("window function api called with wrong object");

	if (!current_context->plv8_obj.IsEmpty())
	{
		Local<v8::Object> plv8_obj = current_context->plv8_obj.Get(isolate);
		if (plv8_obj->InternalFieldCount() < PLV8_INTNL_MAX ||
			!field0->StrictEquals(plv8_obj->GetInternalField(PLV8_INTNL_FCINFO).As<v8::Value>()))
		{
			throw js_error("window function api called with wrong object");
		}
	}

	FunctionCallInfo fcinfo = static_cast<FunctionCallInfo>(
		field0.As<External>()->Value(v8::kExternalPointerTypeTagDefault));
	if (fcinfo == NULL)
		throw js_error("window function api called with wrong object");

	return fcinfo;
}

/*
 * Short-cut routine for window function API
 */
static inline WindowObject
plv8_MyWindowObject(const FunctionCallbackInfo<v8::Value>& args)
{
	/* fcinfo is embedded in the internal field.  See plv8_GetWindowObject() */
	FunctionCallInfo fcinfo = check_window_this(args);

	WindowObject winobj = PG_WINDOW_OBJECT();

	if (!WindowObjectIsValid(winobj))
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
	FunctionCallInfo fcinfo = check_window_this(args);

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
	Local<Context>	context = isolate->GetCurrentContext();
	WindowObject	winobj = plv8_MyWindowObject(args);
	size_t			size;
	window_storage *storage;

	if (args.Length() < 1)
		size = 1000; /* default 1K */
	else {
		const int input_size = Plv8ToInt32(isolate, context, args[0]);
		if (input_size < 0) {
			args.GetReturnValue().Set(isolate->ThrowException(v8::String::NewFromUtf8(args.GetIsolate(), "allocation size cannot be negative").ToLocalChecked()));
			return;
	    	}
		size = input_size;
	}
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

	args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), pos));
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

	args.GetReturnValue().Set(v8::Integer::New(args.GetIsolate(), pos));
}

/*
 * winobj.set_mark_pos(pos)
 */
static void
plv8_WinSetMarkPosition(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	Local<Context>	context = isolate->GetCurrentContext();
	WindowObject	winobj = plv8_MyWindowObject(args);
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
        }
	int64		markpos = Plv8ToInteger(isolate, context, args[0]);

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
	Local<Context>	context = isolate->GetCurrentContext();
	WindowObject	winobj = plv8_MyWindowObject(args);
	if (args.Length() < 2) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
	}
	int64		pos1 = Plv8ToInteger(isolate, context, args[0]);
	int64		pos2 = Plv8ToInteger(isolate, context, args[1]);
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

	args.GetReturnValue().Set(v8::Boolean::New(isolate, res));
}

/*
 * winobj.get_func_arg_in_partition(argno, relpos, seektype, set_mark)
 */
static void
plv8_WinGetFuncArgInPartition(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate*		isolate = args.GetIsolate();
	Local<Context>	context = isolate->GetCurrentContext();
	WindowObject	winobj = plv8_MyWindowObject(args);
	/* Since we return undefined in "isout" case, throw if arg isn't enough. */
	if (args.Length() < 4)
		throw js_error("argument not enough");
	int			argno = Plv8ToInt32(isolate, context, args[0]);
	int			relpos = Plv8ToInt32(isolate, context, args[1]);
	int			seektype = Plv8ToInt32(isolate, context, args[2]);
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
	Local<Context>	context = isolate->GetCurrentContext();
	WindowObject	winobj = plv8_MyWindowObject(args);
	/* Since we return undefined in "isout" case, throw if arg isn't enough. */
	if (args.Length() < 4)
		throw js_error("argument not enough");
	int			argno = Plv8ToInt32(isolate, context, args[0]);
	int			relpos = Plv8ToInt32(isolate, context, args[1]);
	int			seektype = Plv8ToInt32(isolate, context, args[2]);
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
	Local<Context>	context = isolate->GetCurrentContext();
	WindowObject	winobj = plv8_MyWindowObject(args);
	if (args.Length() < 1) {
                args.GetReturnValue().Set(Undefined(isolate));
		return;
        }
	int			argno = Plv8ToInt32(isolate, context, args[0]);
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
	Isolate 		   *isolate = Isolate::GetCurrent();
	Handle<Context> context = isolate->GetCurrentContext();

	isolate->GetHeapStatistics(&v8_heap_stats);

	Local<v8::Value> total = Local<v8::Value>::New(isolate, Number::New(isolate, v8_heap_stats.total_heap_size()));
	Local<v8::Value> used = Local<v8::Value>::New(isolate, Number::New(isolate, v8_heap_stats.used_heap_size()));
	Local<v8::Value> external = Local<v8::Value>::New(isolate, Number::New(isolate, v8_heap_stats.external_memory()));

	obj->Set(context, v8::String::NewFromUtf8(isolate, "total_heap_size").ToLocalChecked(), total).Check();
	obj->Set(context, v8::String::NewFromUtf8(isolate, "used_heap_size").ToLocalChecked(), used).Check();
	obj->Set(context, v8::String::NewFromUtf8(isolate, "external_memory").ToLocalChecked(), external).Check();
}

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

/*
 * plv8.expected_columns()
 *
 * Returns an Array of { name, type_oid, typmod, collation_oid, type_sql }
 * describing the expected result TupleDesc of the currently executing
 * set-returning function (SRF), or null if not called inside an SRF.
 */
static void
plv8_ExpectedColumns(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();
	Local<Context> context = isolate->GetCurrentContext();
	Handle<v8::Object> self = args.This();

	if (!is_valid_plv8_this(isolate, self))
	{
		args.GetReturnValue().Set(Null(isolate));
		return;
	}

	Handle<v8::Value> conv_value = self->GetInternalField(PLV8_INTNL_CONV).As<v8::Value>();

	if (!conv_value->IsExternal())
	{
		args.GetReturnValue().Set(Null(isolate));
		return;
	}

	Converter *conv = static_cast<Converter *>(
		Handle<External>::Cast(conv_value)->Value(v8::kExternalPointerTypeTagDefault));
	if (conv == nullptr || conv->GetTupleDesc() == NULL)
	{
		args.GetReturnValue().Set(Null(isolate));
		return;
	}

	TupleDesc td = conv->GetTupleDesc();
	Local<Array> arr = Array::New(isolate);
	uint32_t out_idx = 0;

	for (int att = 0; att < td->natts; att++)
	{
		Form_pg_attribute attr = TupleDescAttr(td, att);

		CHECK_FOR_INTERRUPTS();
		if (attr->attisdropped)
			continue;

		char *type_sql = NULL;
		PG_TRY();
		{
			type_sql = format_type_with_typemod(attr->atttypid, attr->atttypmod);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();

		Local<Object> col_obj = Object::New(isolate);
		col_obj->CreateDataProperty(context, ToString("name"), ToString(NameStr(attr->attname))).Check();
		col_obj->CreateDataProperty(context, ToString("type_oid"), Uint32::NewFromUnsigned(isolate, attr->atttypid)).Check();
		col_obj->CreateDataProperty(context, ToString("typmod"), Int32::New(isolate, attr->atttypmod)).Check();
		col_obj->CreateDataProperty(context, ToString("collation_oid"), Uint32::NewFromUnsigned(isolate, attr->attcollation)).Check();
		col_obj->CreateDataProperty(context, ToString("type_sql"), ToString(type_sql ? type_sql : "text")).Check();

		arr->CreateDataProperty(context, out_idx++, col_obj).Check();
	}

	args.GetReturnValue().Set(arr);
}

/*
 * plv8.return_query(sql [, args])
 *
 * Executes a SQL query via SPI and streams all result rows directly into the
 * active SRF tuplestore (matching columns positionally, coercing via I/O only
 * when column type OIDs differ). Returns the number of rows appended.
 */
static void
plv8_ReturnQuery(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();
	Handle<v8::Object> self = args.This();

	if (!is_valid_plv8_this(isolate, self))
		throw js_error("return_query called in context that cannot accept a set");

	Handle<v8::Value> conv_value = self->GetInternalField(PLV8_INTNL_CONV).As<v8::Value>();
	Handle<v8::Value> tupstore_value = self->GetInternalField(PLV8_INTNL_TUPSTORE).As<v8::Value>();

	if (!conv_value->IsExternal() || !tupstore_value->IsExternal())
		throw js_error("return_query called in context that cannot accept a set");

	if (args.Length() < 1 || args[0]->IsUndefined() || args[0]->IsNull())
		throw js_error("return_query requires a SQL query string");

	Converter *conv = static_cast<Converter *>(
		Handle<External>::Cast(conv_value)->Value(v8::kExternalPointerTypeTagDefault));
	Tuplestorestate *tupstore = static_cast<Tuplestorestate *>(
		Handle<External>::Cast(tupstore_value)->Value(v8::kExternalPointerTypeTagDefault));

	CString sql(args[0]);
	Handle<Array> params;

	if (args.Length() >= 2 && !args[1]->IsUndefined() && !args[1]->IsNull())
	{
		if (args[1]->IsArray())
			params = Handle<Array>::Cast(args[1]);
		else
			params = convertArgsToArray(args, 1, 1);
	}

	int nparam = params.IsEmpty() ? 0 : params->Length();
	int status = 0;
	uint64 nrows = 0;

	SubTranBlock subtran;
	PG_TRY();
	{
		subtran.enter();
		if (nparam == 0)
			status = SPI_execute(sql, false, 0);
		else
			status = plv8_execute_params(sql, params);

		if (status < 0)
			elog(ERROR, "SPI_execute failed in return_query: %s", FormatSPIStatus(status));

		if (SPI_tuptable != NULL && SPI_processed > 0)
		{
			TupleDesc spi_td = SPI_tuptable->tupdesc;
			TupleDesc exp_td = conv->GetTupleDesc();
			nrows = SPI_processed;

			/* Build positional map of non-dropped columns in spi_td and exp_td */
			int spi_cols[FUNC_MAX_ARGS];
			int n_spi_cols = 0;
			for (int a = 0; a < spi_td->natts && n_spi_cols < FUNC_MAX_ARGS; a++)
			{
				if (!TupleDescAttr(spi_td, a)->attisdropped)
					spi_cols[n_spi_cols++] = a;
			}

			Datum *values = (Datum *) palloc0(sizeof(Datum) * exp_td->natts);
			bool *nulls = (bool *) palloc0(sizeof(bool) * exp_td->natts);

			for (uint64 r = 0; r < nrows; r++)
			{
				HeapTuple spi_tup = SPI_tuptable->vals[r];
				int logical_idx = 0;

				CHECK_FOR_INTERRUPTS();

				for (int exp_att = 0; exp_att < exp_td->natts; exp_att++)
				{
					Form_pg_attribute exp_attr = TupleDescAttr(exp_td, exp_att);
					if (exp_attr->attisdropped)
					{
						nulls[exp_att] = true;
						values[exp_att] = (Datum) 0;
						continue;
					}

					if (logical_idx >= n_spi_cols)
					{
						nulls[exp_att] = true;
						values[exp_att] = (Datum) 0;
						logical_idx++;
						continue;
					}

					int spi_att = spi_cols[logical_idx++];
					Form_pg_attribute spi_attr = TupleDescAttr(spi_td, spi_att);
					bool isnull = false;
					Datum d = heap_getattr(spi_tup, spi_att + 1, spi_td, &isnull);

					if (isnull)
					{
						nulls[exp_att] = true;
						values[exp_att] = (Datum) 0;
					}
					else if (spi_attr->atttypid == exp_attr->atttypid)
					{
						nulls[exp_att] = false;
						values[exp_att] = d;
					}
					else
					{
						Oid typoutput;
						bool typIsVarlena;
						Oid typinput;
						Oid typioparam;
						char *str_val;

						getTypeOutputInfo(spi_attr->atttypid, &typoutput, &typIsVarlena);
						str_val = OidOutputFunctionCall(typoutput, d);
						getTypeInputInfo(exp_attr->atttypid, &typinput, &typioparam);
						values[exp_att] = OidInputFunctionCall(typinput, str_val, typioparam, exp_attr->atttypmod);
						nulls[exp_att] = false;
						pfree(str_val);
					}
				}

				tuplestore_putvalues(tupstore, exp_td, values, nulls);
			}

			pfree(values);
			pfree(nulls);
			SPI_freetuptable(SPI_tuptable);
		}
	}
	PG_CATCH();
	{
		subtran.exit(false);
		throw pg_error();
	}
	PG_END_TRY();

	subtran.exit(true);
	args.GetReturnValue().Set(Int32::New(isolate, (int32_t) nrows));
}

/*
 * plv8.register_column_inference(func_name, handler_fn_or_sql_func_name)
 */
static void
plv8_RegisterColumnInference(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();

	if (args.Length() < 2 || !args[0]->IsString())
		throw js_error("plv8.register_column_inference(func_name, handler) requires a function name and a callback or SQL function name");

	CString func_name(args[0]);
	if (args[1]->IsFunction())
	{
		plv8_register_column_inference_hook(func_name.str(), NULL, Local<Function>::Cast(args[1]));
	}
	else if (args[1]->IsString())
	{
		CString sql_fn(args[1]);
		plv8_register_column_inference_hook(func_name.str(), sql_fn.str(), Local<Function>());
	}
	else if (args[1]->IsNull() || args[1]->IsUndefined())
	{
		plv8_register_column_inference_hook(func_name.str(), NULL, Local<Function>());
	}
	else
	{
		throw js_error("plv8.register_column_inference second argument must be a Function, string, or null");
	}

	args.GetReturnValue().Set(v8::Boolean::New(isolate, true));
}

/*
 * plv8.register_support(func_name, handler_fn_or_sql_func_name)
 */
static void
plv8_RegisterSupport(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();

	if (args.Length() < 2 || !args[0]->IsString())
		throw js_error("plv8.register_support(func_name, handler) requires a function name and a callback or SQL function name");

	CString func_name(args[0]);
	if (args[1]->IsFunction())
	{
		plv8_register_planner_support_hook(func_name.str(), NULL, Local<Function>::Cast(args[1]));
	}
	else if (args[1]->IsString())
	{
		CString sql_fn(args[1]);
		plv8_register_planner_support_hook(func_name.str(), sql_fn.str(), Local<Function>());
	}
	else if (args[1]->IsNull() || args[1]->IsUndefined())
	{
		plv8_register_planner_support_hook(func_name.str(), NULL, Local<Function>());
	}
	else
	{
		throw js_error("plv8.register_support second argument must be a Function, string, or null");
	}

	args.GetReturnValue().Set(v8::Boolean::New(isolate, true));
}

/*
 * plv8.set_function_columns(func_name, columns_array_or_null)
 */
static void
plv8_SetFunctionColumns(const FunctionCallbackInfo<v8::Value>& args)
{
	Isolate *isolate = args.GetIsolate();
	Local<Context> context = isolate->GetCurrentContext();

	if (args.Length() < 2 || !args[0]->IsString())
		throw js_error("plv8.set_function_columns(func_name, columns) requires a function name and columns array or null");

	CString func_name(args[0]);
	if (args[1]->IsNull() || args[1]->IsUndefined())
	{
		plv8_set_function_columns_js(func_name.str(), 0, NULL);
		args.GetReturnValue().Set(v8::Boolean::New(isolate, true));
		return;
	}

	if (!args[1]->IsArray())
		throw js_error("plv8.set_function_columns second argument must be an Array or null");

	Local<Array> arr = Local<Array>::Cast(args[1]);
	uint32_t len = arr->Length();
	if (len == 0)
	{
		plv8_set_function_columns_js(func_name.str(), 0, NULL);
		args.GetReturnValue().Set(v8::Boolean::New(isolate, true));
		return;
	}

	if (len > FUNC_MAX_ARGS)
		len = FUNC_MAX_ARGS;

	Plv8ColMeta cols[FUNC_MAX_ARGS];
	int ncols = 0;

	for (uint32_t i = 0; i < len; i++)
	{
		Local<v8::Value> elem;
		if (!arr->Get(context, i).ToLocal(&elem) || !elem->IsObject())
			continue;
		Local<Object> obj = Local<Object>::Cast(elem);

		Local<v8::Value> name_val;
		const char *cname = "?column?";
		if (obj->Get(context, ToString("name")).ToLocal(&name_val) && name_val->IsString())
		{
			CString cs(name_val);
			cname = pstrdup(cs.str("?column?"));
		}
		else
		{
			cname = pstrdup("?column?");
		}

		Oid type_oid = InvalidOid;
		int32 typmod = -1;
		Oid coll_oid = InvalidOid;

		Local<v8::Value> oid_val;
		if (obj->Get(context, ToString("type_oid")).ToLocal(&oid_val) && oid_val->IsNumber())
			type_oid = (Oid) oid_val->Uint32Value(context).FromMaybe(0);

		Local<v8::Value> tm_val;
		if (obj->Get(context, ToString("typmod")).ToLocal(&tm_val) && tm_val->IsNumber())
			typmod = tm_val->Int32Value(context).FromMaybe(-1);

		Local<v8::Value> coll_val;
		if (obj->Get(context, ToString("collation_oid")).ToLocal(&coll_val) && coll_val->IsNumber())
			coll_oid = (Oid) coll_val->Uint32Value(context).FromMaybe(0);

		if (!OidIsValid(type_oid))
		{
			Local<v8::Value> type_str_val;
			if ((obj->Get(context, ToString("type")).ToLocal(&type_str_val) && type_str_val->IsString()) ||
				(obj->Get(context, ToString("type_sql")).ToLocal(&type_str_val) && type_str_val->IsString()))
			{
				CString ts(type_str_val);
				PG_TRY();
				{
#if PG_VERSION_NUM >= 160000
					(void) parseTypeString(ts.str(), &type_oid, &typmod, NULL);
#else
					(void) parseTypeString(ts.str(), &type_oid, &typmod, false);
#endif
				}
				PG_CATCH();
				{
					FlushErrorState();
					type_oid = TEXTOID;
					typmod = -1;
				}
				PG_END_TRY();
			}
		}

		if (!OidIsValid(type_oid))
			type_oid = TEXTOID;

		cols[ncols].name = (char *) cname;
		cols[ncols].type_oid = type_oid;
		cols[ncols].typmod = typmod;
		cols[ncols].collation_oid = coll_oid;
		ncols++;
	}

	plv8_set_function_columns_js(func_name.str(), ncols, cols);
	args.GetReturnValue().Set(v8::Boolean::New(isolate, true));
}
