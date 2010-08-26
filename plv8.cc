/*
 * plv8.cc : PL/v8 handler routines.
 */
#include "plv8.h"
#include <new>
#include <sstream>

extern "C" {
#define delete		delete_
#define namespace	namespace_
#define	typeid		typeid_
#define	typename	typename_
#define	using		using_

#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#undef delete
#undef namespace
#undef typeid
#undef typename
#undef using

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);

Datum	plv8_call_handler(PG_FUNCTION_ARGS) throw();
Datum	plv8_call_validator(PG_FUNCTION_ARGS) throw();

#if PG_VERSION_NUM >= 90000
PG_FUNCTION_INFO_V1(plv8_inline_handler);
Datum	plv8_inline_handler(PG_FUNCTION_ARGS) throw();
#endif
} // extern "C"

using namespace v8;

typedef struct plv8_proc
{
	Oid						fn_oid;

	Persistent<Function>	function;
	char					proname[NAMEDATALEN];
	char				   *prosrc;
	
	TransactionId			fn_xmin;
	ItemPointerData			fn_tid;
	
	int						nargs;
	plv8_type				rettype;
	plv8_type				argtypes[FUNC_MAX_ARGS];
} plv8_proc;

static HTAB *plv8_proc_hash = NULL;

/*
 * loser_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc_cache(Oid fn_oid, bool validate, char ***argnames) throw();
static void plv8_fill_type(plv8_type *type, Oid typid) throw();

/*
 * CamelCaseFunctions are C++ functions.
 * They could raise errors with C++ throw statements, or never throw exceptions.
 */
static plv8_proc *Compile(Oid fn_oid, bool validate, bool is_trigger);
static Handle<Function> CreateFunction(const char *proname, int proarglen,
					const char *proargs[], const char *prosrc, bool is_trigger);
static Datum CallFunction(PG_FUNCTION_ARGS, Handle<Function> fn,
				int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallTrigger(PG_FUNCTION_ARGS, Handle<Function> fn);
static Handle<v8::Value> Print(const Arguments& args) throw();
static Handle<v8::Value> PrintInternal(const Arguments& args);
static Handle<v8::Value> ExecuteSql(const Arguments& args) throw();
static Handle<v8::Value> ExecuteSqlInternal(const Arguments& args);
static Handle<Context> GetGlobalContext() throw();


Datum
plv8_call_handler(PG_FUNCTION_ARGS) throw()
{
	Oid		fn_oid = fcinfo->flinfo->fn_oid;
	bool	is_trigger = CALLED_AS_TRIGGER(fcinfo);

	try
	{
		HandleScope	handle_scope;

		if (!fcinfo->flinfo->fn_extra)
			fcinfo->flinfo->fn_extra = Compile(fn_oid, false, is_trigger);

		plv8_proc   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;

		if (is_trigger)
			return CallTrigger(fcinfo, proc->function);
		else
			return CallFunction(fcinfo, proc->function,
						proc->nargs, proc->argtypes, &proc->rettype);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

#if PG_VERSION_NUM >= 90000
Datum
plv8_inline_handler(PG_FUNCTION_ARGS) throw()
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));

	Assert(IsA(codeblock, InlineCodeBlock));

	try
	{
		HandleScope			handle_scope;

		Handle<Function>	fn = CreateFunction(NULL, 0, NULL,
										codeblock->source_text, false);
		return CallFunction(fcinfo, fn, 0, NULL, NULL);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}
#endif

static Datum
CallFunction(PG_FUNCTION_ARGS, Handle<Function> fn,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	Handle<Context>		global_context = GetGlobalContext();
	Context::Scope		context_scope(global_context);
	Handle<v8::Value>	args[FUNC_MAX_ARGS];

	TryCatch			try_catch;

	for (int i = 0; i < nargs; i++)
		args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);
	Handle<v8::Value> result =
		fn->Call(global_context->Global(), nargs, args);
	if (result.IsEmpty())
		throw js_error(try_catch);
	if (rettype)
		return ToDatum(result, &fcinfo->isnull, rettype);
	else
		PG_RETURN_VOID();
}

static Datum
CallTrigger(PG_FUNCTION_ARGS, Handle<Function> fn)
{
	// trigger arguments are:
	//	0: NEW
	//	1: OLD
	//	2: TG_NAME
	//	3: TG_WHEN
	//	4: TG_LEVEL
	//	5: TG_OP
	//	6: TG_RELID
	//	7: TG_TABLE_NAME
	//	8: TG_TABLE_SCHEMA
	//	9: TG_ARGV
	TriggerData		   *trig = (TriggerData *) fcinfo->context;
	Relation			rel = trig->tg_relation;
	TriggerEvent		event = trig->tg_event;
	Handle<v8::Value>	args[10];
	Datum				result = (Datum) 0;

	Handle<Context>		global_context = GetGlobalContext();
	Context::Scope		context_scope(global_context);
	TryCatch			try_catch;

	if (TRIGGER_FIRED_FOR_ROW(event))
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);

		// TODO: cache Converter into fn_extra.

		if (TRIGGER_FIRED_BY_INSERT(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_trigtuple);
			// OLD
			args[1] = Undefined();
		}
		else if (TRIGGER_FIRED_BY_DELETE(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = Undefined();
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
		else if (TRIGGER_FIRED_BY_UPDATE(event))
		{
			result = PointerGetDatum(trig->tg_newtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_newtuple);
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
	}
	else
	{
		args[0] = args[1] = Undefined();
	}

	// 2: TG_NAME
	args[2] = ToString(trig->tg_trigger->tgname);

	// 3: TG_WHEN
	if (TRIGGER_FIRED_BEFORE(event))
		args[3] = String::New("BEFORE");
	else
		args[3] = String::New("AFTER");

	// 4: TG_LEVEL
	if (TRIGGER_FIRED_FOR_ROW(event))
		args[4] = String::New("ROW");
	else
		args[4] = String::New("STATEMENT");

	// 5: TG_OP
	if (TRIGGER_FIRED_BY_INSERT(event))
		args[5] = String::New("INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(event))
		args[5] = String::New("DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(event))
		args[5] = String::New("UPDATE");
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(event))
		args[5] = String::New("TRUNCATE");
#endif
	else
		args[5] = String::New("?");

	// 6: TG_RELID
	args[6] = Uint32::New(RelationGetRelid(rel));

	// 7: TG_TABLE_NAME
	args[7] = ToString(RelationGetRelationName(rel));

	// 8: TG_TABLE_SCHEMA
	args[8] = ToString(get_namespace_name(RelationGetNamespace(rel)));

	// 9: TG_ARGV
	Handle<Array> tgargs = Array::New(trig->tg_trigger->tgnargs);
	for (int i = 0; i < trig->tg_trigger->tgnargs; i++)
		tgargs->Set(i, ToString(trig->tg_trigger->tgargs[i]));
	args[9] = tgargs;

	Handle<v8::Value> newtup =
		fn->Call(global_context->Global(), lengthof(args), args);
	if (newtup.IsEmpty())
		throw js_error(try_catch);

	// TODO: replace NEW tuple if modified.

	return result;
}

Datum
plv8_call_validator(PG_FUNCTION_ARGS) throw()
{
	Oid				fn_oid = PG_GETARG_OID(0);
	HeapTuple		tuple;
	Form_pg_proc	proc;
	char			functyptype;
	bool			is_trigger = false;
	js_error		error;

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, or VOID */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			is_trigger = true;
		else if (proc->prorettype != RECORDOID &&
			proc->prorettype != VOIDOID)
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/v8 functions cannot return type %s",
						format_type_be(proc->prorettype))));
	}

	ReleaseSysCache(tuple);

	try
	{
		(void) Compile(fn_oid, true, is_trigger);
		/* the result of a validator is ignored */
		PG_RETURN_VOID();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

static plv8_proc *
plv8_get_proc_cache(Oid fn_oid, bool validate, char ***argnames) throw()
{
	HeapTuple		procTup;
	Form_pg_proc	procStruct;
	plv8_proc	   *proc;
	bool			found;
	bool			isnull;
	Datum			prosrc;
	Oid			   *argtypes;
	char		   *argmodes;
	Oid				rettype;
	MemoryContext	oldcontext;

	if (plv8_proc_hash == NULL)
	{
		HASHCTL    hash_ctl = { 0 };

		hash_ctl.keysize = sizeof(Oid);
		hash_ctl.entrysize = sizeof(plv8_proc);
		hash_ctl.hash = oid_hash;
		plv8_proc_hash = hash_create("PLv8 Procedures", 32,
									 &hash_ctl, HASH_ELEM | HASH_FUNCTION);
	}

	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	proc = (plv8_proc *) hash_search(plv8_proc_hash, &fn_oid, HASH_ENTER, &found);

	if (found)
	{
		bool    uptodate;

		uptodate = (!proc->function.IsEmpty() &&
			proc->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			ItemPointerEquals(&proc->fn_tid, &procTup->t_self));

		if (!uptodate)
		{
			if (proc->prosrc)
			{
				pfree(proc->prosrc);
				proc->prosrc = NULL;
			}
			proc->function.Dispose();
		}
		else
		{
			ReleaseSysCache(procTup);
			return proc;
		}
	}
	else
	{
		new(&proc->function) Persistent<Function>();
		proc->prosrc = NULL;
	}

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	prosrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	//  functyptype = get_typtype(procStruct->prorettype);
	rettype = procStruct->prorettype;

	strlcpy(proc->proname, NameStr(procStruct->proname), NAMEDATALEN);
	proc->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	proc->fn_tid = procTup->t_self;
	proc->nargs = get_func_arg_info(procTup, &argtypes, argnames, &argmodes);

	if (validate)
	{
		/* Disallow pseudotypes in arguments (either IN or OUT) */
		for (int i = 0; i < proc->nargs; i++)
		{
			if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO)
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/v8 functions cannot accept type %s",
							format_type_be(argtypes[i]))));
		}
	}

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	proc->prosrc = TextDatumGetCString(prosrc);

	ReleaseSysCache(procTup);

	for (int i = 0; i < proc->nargs; i++)
	{
		Oid		argtype = argtypes[i];
		char	argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

		switch (argmode)
		{
		case PROARGMODE_IN:
		case PROARGMODE_VARIADIC:
			break;
		default:
			elog(ERROR, "OUT parameters are not supported");
		}

		plv8_fill_type(&proc->argtypes[i], argtype);
	}

	plv8_fill_type(&proc->rettype, rettype);

	/* restore */
	MemoryContextSwitchTo(oldcontext);

	return proc;
}

static plv8_proc *
Compile(Oid fn_oid, bool validate, bool is_trigger)
{
	plv8_proc  *proc;
	char	  **argnames;

	PG_TRY();
	{
		proc = plv8_get_proc_cache(fn_oid, validate, &argnames);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (proc->function.IsEmpty())
		proc->function = Persistent<Function>::New(CreateFunction(
						proc->proname,
						proc->nargs,
						(const char **) argnames,
						proc->prosrc,
						is_trigger));

	return proc;
}

static Handle<Function>
CreateFunction(
	const char *proname,
	int proarglen,
	const char *proargs[],
	const char *prosrc,
	bool is_trigger)
{
	HandleScope		handle_scope;
	StringInfoData  src;
	Handle<Context>	global_context = GetGlobalContext();

	initStringInfo(&src);

	/*
	 *  function _(<arg1, ...>){
	 *    <prosrc>
	 *  }
	 *  _
	 */
	appendStringInfo(&src, "function _(");
	if (is_trigger)
	{
		if (proarglen != 0)
			throw js_error("trigger function cannot have arguments");
		// trigger function has special arguments.
		appendStringInfo(&src,
			"NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
			"TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
	}
	else
	{
		for (int i = 0; i < proarglen; i++)
		{
			if (i > 0)
				appendStringInfoChar(&src, ',');
			if (proargs && proargs[i])
				appendStringInfoString(&src, proargs[i]);
			else
				appendStringInfo(&src, "$%d", i + 1);	// unnamed argument to $N
		}
	}
	appendStringInfo(&src, "){\n%s\n};\n_", prosrc);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined();
	Handle<String> source = ToString(src.data, src.len);
	pfree(src.data);

	Context::Scope	context_scope(global_context);
	TryCatch		try_catch;
	Handle<Script>	script = Script::Compile(source, name);

	if (script.IsEmpty())
		throw js_error(try_catch);

	Handle<v8::Value> result = script->Run();
	if (result.IsEmpty())
		throw js_error(try_catch);

	Handle<Function> fn = Handle<Function>::Cast(result);
	if (fn.IsEmpty())
		throw js_error(try_catch);

	return fn;
}

static void
plv8_fill_type(plv8_type *type, Oid typid) throw()
{
	bool    ispreferred;

	type->typid = typid;
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
}

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

static Handle<v8::Value>
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

static Handle<v8::Value>
ExecuteSql(const Arguments& args) throw()
{
	return SafeCall(ExecuteSqlInternal, args);
}

/*
 * executeSql(string sql) returns array<json> for query, or integer for command.
 *
 * TODO: support query arguments with SPI_execute_with_args().
 */
static Handle<v8::Value>
ExecuteSqlInternal(const Arguments& args)
{
	if (args.Length() != 1)
		return ThrowException(String::New("usage: executeSql(sql)"));

	SPI_connect();

	CString				sql(args[0]);
	Handle<v8::Value>	result;
	int					status;

	PG_TRY();
	{
		status = SPI_exec(sql, 0);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();  

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

	SPI_finish();

	return result;
}

static Handle<Context>
GetGlobalContext() throw()
{
	static Persistent<Context>	global_context;

	if (global_context.IsEmpty())
	{
		HandleScope				handle_scope;
		Handle<ObjectTemplate>	global = ObjectTemplate::New();

		// built-in function print(elevel, ...)
		global->Set(String::NewSymbol("print"),
					FunctionTemplate::New(Print));
		global->Set(String::NewSymbol("DEBUG5"), Int32::New(DEBUG5));
		global->Set(String::NewSymbol("DEBUG4"), Int32::New(DEBUG4));
		global->Set(String::NewSymbol("DEBUG3"), Int32::New(DEBUG3));
		global->Set(String::NewSymbol("DEBUG2"), Int32::New(DEBUG2));
		global->Set(String::NewSymbol("DEBUG1"), Int32::New(DEBUG1));
		global->Set(String::NewSymbol("DEBUG"), Int32::New(DEBUG5));
		global->Set(String::NewSymbol("LOG"), Int32::New(LOG));
		global->Set(String::NewSymbol("INFO"), Int32::New(INFO));
		global->Set(String::NewSymbol("NOTICE"), Int32::New(NOTICE));
		global->Set(String::NewSymbol("WARNING"), Int32::New(WARNING));
		// ERROR or higher severity levels are not allowed. Use "throw" instead.

		// built-in function executeSql(sql)
		global->Set(String::NewSymbol("executeSql"),
					FunctionTemplate::New(ExecuteSql));

		global_context = Context::New(NULL, global);
	}

	return global_context;
}

Converter::Converter(TupleDesc tupdesc) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts)
{
	for (int c = 0; c < tupdesc->natts; c++)
	{
		m_colnames[c] = ToString(SPI_fname(m_tupdesc, c + 1));
		PG_TRY();
		{
			plv8_fill_type(&m_coltypes[c], m_tupdesc->attrs[c]->atttypid);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();
	}
}

// TODO: use prototype instead of per tuple fields to reduce
// memory consumption.
Handle<Object>
Converter::ToValue(HeapTuple tuple)
{
	Handle<Object>	obj = Object::New();

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Datum		datum;
		bool		isnull;

		datum = SPI_getbinval(tuple, m_tupdesc, c + 1, &isnull);
		obj->Set(m_colnames[c], ::ToValue(datum, isnull, &m_coltypes[c]));
	}

	return obj;
}

Datum
Converter::ToDatum(Handle<v8::Value> value)
{
	TryCatch		try_catch;

	Handle<Object>	obj = Handle<Object>::Cast(value);
	if (obj.IsEmpty())
		throw js_error(try_catch);

	/*
	 * Use vector<char> instead of vector<bool> because <bool> version is
	 * s specialized and different from bool[].
	 */
	std::vector<Datum>	values(m_tupdesc->natts);
	std::vector<char>	nulls(m_tupdesc->natts);

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Handle<v8::Value> attr = obj->Get(m_colnames[c]);
		if (attr.IsEmpty() || attr->IsUndefined() || attr->IsNull())
			nulls[c] = true;
		else
			values[c] = ::ToDatum(attr, (bool *) &nulls[c], &m_coltypes[c]);
	}

	HeapTuple tuple = heap_form_tuple(m_tupdesc, &values[0], (bool *) &nulls[0]);

	return HeapTupleGetDatum(tuple);
}

js_error::js_error() throw()
	: m_msg(NULL), m_detail(NULL)
{
}

js_error::js_error(const char *msg) throw()
{
	m_msg = pstrdup(msg);
	m_detail = NULL;
}

js_error::js_error(TryCatch &try_catch) throw()
{
	HandleScope			handle_scope;
	String::Utf8Value	exception(try_catch.Exception());
	Handle<Message>		message = try_catch.Message();

	m_msg = NULL;
	m_detail = NULL;

	try
	{
		m_msg = ToCStringCopy(exception);

		if (!message.IsEmpty())
		{
			StringInfoData	str;
			CString			script(message->GetScriptResourceName());
			int				lineno = message->GetLineNumber();
			CString			source(message->GetSourceLine());

			/*
			 * Report lineno - 1 because "function _(...){" was added
			 * at the first line to the javascript code.
			 */
			initStringInfo(&str);
			appendStringInfo(&str, "%s() LINE %d: %s",
				script.str("?"), lineno - 1, source.str("?"));
			m_detail = str.data;
		}
	}
	catch (...)
	{
		// nested error, keep quiet.
	}
}

__attribute__((noreturn))
void
js_error::rethrow() throw()
{
	ereport(ERROR,
		(m_msg ? errmsg("%s", m_msg) : 0,
			m_detail ? errdetail("%s", m_detail) : 0));
	exit(0);	// keep compiler quiet
}

__attribute__((noreturn))
void
pg_error::rethrow() throw()
{
	PG_RE_THROW();
	exit(0);	// keep compiler quiet
}
