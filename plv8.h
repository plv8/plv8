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
#include <vector>

extern "C" {
#include "postgres.h"

#include "access/htup.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "utils/tuplestore.h"
}

#ifdef _MSC_VER
#define __attribute__(what)		__declspec what
#elif !defined(__GNUC__)
#define __attribute__(what)
#endif

/* numbers for plv8 object internal field */
#define PLV8_INTNL_CONV			1
#define PLV8_INTNL_TUPSTORE		2
#define PLV8_INTNL_MAX			3

enum Dialect{ PLV8_DIALECT_NONE, PLV8_DIALECT_COFFEE, PLV8_DIALECT_LIVESCRIPT };

/* js_error represents exceptions in JavaScript. */
class js_error
{
private:
	char	   *m_msg;
	char	   *m_detail;

public:
	js_error() throw();
	js_error(const char *msg) throw();
	js_error(v8::TryCatch &try_catch) throw();
	v8::Local<v8::Value> error_object();
	__attribute__((noreturn)) void rethrow() throw();
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
	FmgrInfo	fn_input;
	FmgrInfo	fn_output;
} plv8_type;

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

public:
	Converter(TupleDesc tupdesc);
	Converter(TupleDesc tupdesc, bool is_scalar);
	v8::Local<v8::Object> ToValue(HeapTuple tuple);
	Datum	ToDatum(v8::Handle<v8::Value> value, Tuplestorestate *tupstore = NULL);

private:
	Converter(const Converter&);
	Converter& operator = (const Converter&);
	void	Init();
};

extern v8::Local<v8::Function> find_js_function(Oid fn_oid);
extern v8::Local<v8::Function> find_js_function_by_name(const char *signature);
extern const char *FormatSPIStatus(int status) throw();
extern v8::Handle<v8::Value> ThrowError(const char *message) throw();

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
extern v8::Handle<v8::Value> Subtransaction(const v8::Arguments& args) throw();

extern void SetupPlv8Functions(v8::Handle<v8::ObjectTemplate> plv8);

#endif	// _PLV8_
