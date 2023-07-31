/*-------------------------------------------------------------------------
 *
 * plv8.h
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#ifndef _PLV8_
#define _PLV8_

#include "plv8_config.h"
#include <v8.h>
#ifdef ENABLE_DEBUGGER_SUPPORT
#include <v8-debug.h>
#endif  // ENABLE_DEBUGGER_SUPPORT
#include <v8-version-string.h>
#include <vector>

extern "C" {
#include "postgres.h"

#include "access/htup.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "utils/tuplestore.h"
#include "windowapi.h"
}
#include <string>

#ifdef _MSC_VER
#define __attribute__(what)		__declspec what
#elif !defined(__GNUC__)
#define __attribute__(what)
#endif

/* numbers for plv8 object internal field */
/* Converter for SRF */
#define PLV8_INTNL_CONV			1
/* Tuplestore for SRF */
#define PLV8_INTNL_TUPSTORE		2
/* FunctionCallInfo for window functions */
#define PLV8_INTNL_FCINFO		3
#define PLV8_INTNL_MAX			4

enum Dialect{ PLV8_DIALECT_NONE, PLV8_DIALECT_COFFEE, PLV8_DIALECT_LIVESCRIPT };

/* js_error represents exceptions in JavaScript. */
class js_error
{
private:
	char	   *m_msg;
	int	   m_code;
	char	   *m_detail;
	char	   *m_hint;
	char	   *m_context;
	void init(v8::Isolate *isolate, v8::Local<v8::Value> exception, v8::Local<v8::Message> message) noexcept;

public:
	js_error() noexcept;
	explicit js_error(const char *msg) noexcept;
	explicit js_error(v8::Isolate *isolate, v8::Local<v8::Value> exception, v8::Local<v8::Message> message) noexcept;
	explicit js_error(v8::TryCatch &try_catch) noexcept;
	v8::Local<v8::Value> error_object();
	__attribute__((noreturn)) void rethrow(const char *msg_format = nullptr) noexcept;
	void log(int elevel, const char *msg_format = nullptr) noexcept;
};

/*
 * pg_error represents ERROR in postgres.
 * Instances of the class should be thrown only in PG_CATCH block.
 */
class pg_error
{
public:
	__attribute__((noreturn)) void rethrow() throw();
};

typedef enum plv8_external_array_type
{
	kExternalByteArray = 1,
	kExternalUnsignedByteArray,
	kExternalShortArray,
	kExternalUnsignedShortArray,
	kExternalIntArray,
	kExternalUnsignedIntArray,
	kExternalFloatArray,
	kExternalDoubleArray,
	kExternalUnsignedInt64Array,
	kExternalInt64Array
} plv8_external_array_type;

/*
 * When TYPCATEGORY_ARRAY, other fields are for element types.
 *
 * Note that postgres doesn't support type modifiers for arguments and result types.
 */
typedef struct plv8_type
{
	Oid			typid;
	Oid			ioparam;
	int16		len;
	bool		byval;
	char		align;
	char		category;
	bool		is_composite;
	FmgrInfo	fn_input;
	FmgrInfo	fn_output;
	plv8_external_array_type ext_array;
} plv8_type;

/*
 * For the security reasons, the global context is separated
 * between users and it's associated with user id.
 */
typedef struct plv8_context
{
	v8::Isolate				   	   	   *isolate;
	v8::ArrayBuffer::Allocator	   	   *array_buffer_allocator;
	v8::Persistent<v8::Context>			context;
	v8::Persistent<v8::ObjectTemplate>	recv_templ;
	v8::Persistent<v8::Context>			compile_context;
	v8::Persistent<v8::ObjectTemplate>  plan_template;
	v8::Persistent<v8::ObjectTemplate>  cursor_template;
	v8::Persistent<v8::ObjectTemplate>  window_template;
	v8::Local<v8::Context> localContext() { return v8::Local<v8::Context>::New(isolate, context) ; }
	bool 						is_dead;
	bool						interrupted;
	Oid							user_id;
	std::vector<std::tuple<v8::Global<v8::Promise>, v8::Global<v8::Message>, v8::Global<v8::Value>>> unhandled_promises;
	bool 						ignore_unhandled_promises;
} plv8_context;

/*
 * A multibyte string in the database encoding. It works more effective
 * when the encoding is UTF8.
 */
class CString
{
private:
	v8::String::Utf8Value	m_utf8;
	char				   *m_str;

public:
	explicit CString(v8::Handle<v8::Value> value);
	~CString();
	operator char* ()				{ return m_str; }
	operator const char* () const	{ return m_str; }
	const char* str(const char *ifnull = NULL) const
	{ return m_str ? m_str : ifnull; }

    static bool toStdString(v8::Handle<v8::Value> value, std::string &out);

private:
	CString(const CString&);
	CString& operator = (const CString&);
};

/*
 * Records in postgres to JSON in v8 converter.
 */
class Converter
{
private:
	TupleDesc								m_tupdesc;
	std::vector< v8::Handle<v8::String> >	m_colnames;
	std::vector< plv8_type >				m_coltypes;
	bool									m_is_scalar;
	MemoryContext							m_memcontext;

public:
	Converter(TupleDesc tupdesc);
	Converter(TupleDesc tupdesc, bool is_scalar);
	~Converter();
	v8::Local<v8::Object> ToValue(HeapTuple tuple);
	Datum	ToDatum(v8::Handle<v8::Value> value, Tuplestorestate *tupstore = NULL);

private:
	Converter(const Converter&);
	Converter& operator = (const Converter&);
	void	Init();
};

/*
 * Provide JavaScript JSON object functionality.
 */
class JSONObject
{
private:
	v8::Handle<v8::Object> m_json;

public:
	JSONObject();
	v8::Handle<v8::Value> Parse(v8::Handle<v8::Value> str);
	v8::Handle<v8::Value> Stringify(v8::Handle<v8::Value> val);
};

/*
 * Check if this is a window function call.  If so, we store fcinfo
 * in plv8 object for API to use it.  It would be possible to do it
 * in the normal function cases, but currently nobody is using it
 * and probably adds overhead.  We need a class because the destructor
 * makes sure the restore happens.
 */
class WindowFunctionSupport
{
private:
	WindowObject			m_winobj;
	v8::Handle<v8::Object>	m_plv8obj;
	v8::Handle<v8::Value>	m_prev_fcinfo;

public:
	WindowFunctionSupport(v8::Handle<v8::Context> context,
						FunctionCallInfo fcinfo)
	{
		m_winobj = PG_WINDOW_OBJECT();
		if (WindowObjectIsValid(m_winobj))
		{
			m_plv8obj = v8::Handle<v8::Object>::Cast(
					context->Global()->Get(context, v8::String::NewFromUtf8Literal(
						context->GetIsolate(),
						"plv8",
						v8::NewStringType::kInternalized)).ToLocalChecked());
			if (m_plv8obj.IsEmpty())
				throw js_error("plv8 object not found");
			/* Stash the current item, just in case of nested call */
			m_prev_fcinfo = m_plv8obj->GetInternalField(PLV8_INTNL_FCINFO);
			m_plv8obj->SetInternalField(PLV8_INTNL_FCINFO,
					v8::External::New(context->GetIsolate(), fcinfo));
		}
	}
	bool IsWindowCall() { return WindowObjectIsValid(m_winobj); }
	WindowObject GetWindowObject() { return m_winobj; }
	~WindowFunctionSupport()
	{
		/* Restore the previous fcinfo if we saved. */
		if (WindowObjectIsValid(m_winobj))
		{
			m_plv8obj->SetInternalField(PLV8_INTNL_FCINFO, m_prev_fcinfo);
		}
	}
};

/*
 * In case this is nested via SPI, stash pre-registered converters
 * for the previous SRF.  We need a class because the destructor makes
 * sure the restore happens.
 */
class SRFSupport
{
private:
	v8::Handle<v8::Object> m_plv8obj;
	v8::Handle<v8::Value> m_prev_conv, m_prev_tupstore;

public:
	SRFSupport(v8::Handle<v8::Context> context,
			   Converter *conv, Tuplestorestate *tupstore)
	{
	    v8::Local<v8::Value> m_val;
	    if (!context->Global()->Get(context, v8::String::NewFromUtf8Literal(
                context->GetIsolate(),
                "plv8",
                v8::NewStringType::kInternalized)).ToLocal(&m_val))
            throw js_error("plv8 object not found");
	    m_plv8obj = v8::Handle<v8::Object>::Cast(m_val);
		m_prev_conv = m_plv8obj->GetInternalField(PLV8_INTNL_CONV);
		m_prev_tupstore = m_plv8obj->GetInternalField(PLV8_INTNL_TUPSTORE);
		m_plv8obj->SetInternalField(PLV8_INTNL_CONV,
									v8::External::New(context->GetIsolate(), conv));
		m_plv8obj->SetInternalField(PLV8_INTNL_TUPSTORE,
									v8::External::New(context->GetIsolate(), tupstore));
	}
	~SRFSupport()
	{
		/* Restore the previous items. */
		m_plv8obj->SetInternalField(PLV8_INTNL_CONV, m_prev_conv);
		m_plv8obj->SetInternalField(PLV8_INTNL_TUPSTORE, m_prev_tupstore);
	}
};

extern plv8_context* current_context;
extern v8::Local<v8::Function> find_js_function(Oid fn_oid);
extern v8::Local<v8::Function> find_js_function_by_name(const char *signature);
extern const char *FormatSPIStatus(int status) throw();
extern plv8_type *get_plv8_type(PG_FUNCTION_ARGS, int argno);

// plv8_type.cc
extern void plv8_fill_type(plv8_type *type, Oid typid, MemoryContext mcxt = NULL);
extern Oid inferred_datum_type(v8::Handle<v8::Value> value);
extern Datum ToDatum(v8::Handle<v8::Value> value, bool *isnull, plv8_type *type);
extern v8::Local<v8::Value> ToValue(Datum datum, bool isnull, plv8_type *type);
extern v8::Local<v8::String> ToString(Datum value, plv8_type *type);
extern v8::Local<v8::String> ToString(const char *str, int len = -1, int encoding = GetDatabaseEncoding());
extern char *ToCString(const v8::String::Utf8Value &value);
extern char *ToCStringCopy(const v8::String::Utf8Value &value);

// plv8_func.cc
extern v8::Handle<v8::Function> CreateYieldFunction(Converter *conv, Tuplestorestate *tupstore);
extern void Subtransaction(const v8::FunctionCallbackInfo<v8::Value>& info) throw();

extern void SetupPlv8Functions(v8::Handle<v8::ObjectTemplate> plv8);
extern void SetupPrepFunctions(v8::Handle<v8::ObjectTemplate> templ);
extern void SetupCursorFunctions(v8::Handle<v8::ObjectTemplate> templ);
extern void SetupWindowFunctions(v8::Handle<v8::ObjectTemplate> templ);

extern void HandleUnhandledPromiseRejections();

extern void GetMemoryInfo(v8::Local<v8::Object> obj);

extern struct config_generic *plv8_find_option(const char *name);
char *plv8_string_option(struct config_generic * record);
int plv8_int_option(struct config_generic * record);

#endif	// _PLV8_
