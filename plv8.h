#ifndef _PLV8_
#define _PLV8_

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

public:
	Converter(TupleDesc tupdesc);
	v8::Handle<v8::Object> ToValue(HeapTuple tuple);
	Datum	ToDatum(v8::Handle<v8::Value> value, Tuplestorestate *tupstore = NULL);

private:
	Converter(const Converter&);
	Converter& operator = (const Converter&);
};

// plv8_type.cc
extern Datum ToDatum(v8::Handle<v8::Value> value, bool *isnull, plv8_type *type);
extern v8::Handle<v8::Value> ToValue(Datum datum, bool isnull, plv8_type *type);
extern v8::Handle<v8::String> ToString(Datum value, plv8_type *type);
extern v8::Handle<v8::String> ToString(const char *str, int len = -1, int encoding = GetDatabaseEncoding());
extern char *ToCString(const v8::String::Utf8Value &value);
extern char *ToCStringCopy(const v8::String::Utf8Value &value);

// plv8_func.cc
extern v8::Handle<v8::Value> Print(const v8::Arguments& args) throw();
extern v8::Handle<v8::Value> ExecuteSql(const v8::Arguments& args) throw();
extern v8::Handle<v8::Value> Yield(const v8::Arguments& args) throw();
extern v8::Handle<v8::Function> CreateYieldFunction(Converter *conv, Tuplestorestate *tupstore);

#endif	// _PLV8_
