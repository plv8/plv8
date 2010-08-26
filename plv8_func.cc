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

#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "utils/builtins.h"

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
static Handle<v8::Value> YieldInternal(const Arguments& args);

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
		return ThrowException(String::New("internal exception"));
	}
	catch (pg_error& e)
	{
		MemoryContextSwitchTo(ctx);
		ErrorData *edata = CopyErrorData();
		Handle<String> message = ToString(edata->message);
		// XXX: add other fields? (detail, hint, context, internalquery...)
		FlushErrorState();
		FreeErrorData(edata);

		return ThrowException(message);
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
		return ThrowException(String::New("usage: print(elevel, ...)"));

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
		return ThrowException(String::New("invalid error level for print()"));
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
		return ThrowException(String::New("usage: executeSql(sql [, args])"));

	Handle<v8::Value>	result;

	SPI_connect();
	if (args.Length() == 1)
		result = SafeCall(ExecuteSqlInternal, args);
	else
		result = SafeCall(ExecuteSqlWithArgs, args);
	SPI_finish();
	return result;
}

static Handle<v8::Value>
SPIResultToValue(int status)
{
	Handle<v8::Value>	result;

	if (status < 0)
		return ThrowException(String::New("SPI failed"));

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
	SPI_connect();

	int			status;
	CString		sql(args[0]);

	PG_TRY();
	{
		status = SPI_exec(sql, 0);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

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
			return ThrowException(String::New("Date is not supported as arguments for executeSql()"));
		}
		else if (value->IsObject())
		{
			// TODO: Object
			return ThrowException(String::New("Object is not supported as arguments for executeSql()"));
		}
		else if (value->IsArray())
		{
			// TODO: Array
			return ThrowException(String::New("Array is not supported as arguments for executeSql()"));
		}
		else
		{
			CString str(value);
			values[i] = CStringGetTextDatum(str);
			argtypes[i] = TEXTOID;
		}
	}

	PG_TRY();
	{
		status = SPI_execute_with_args(sql, nargs,
					argtypes, values, nulls, false, 0);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return SPIResultToValue(status);
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
