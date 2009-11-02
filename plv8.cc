#include "plv8.h"

#include <v8.h>

#ifdef __cplusplus
extern "C"{
#endif

#include "funcapi.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/array.h"
#include "utils/memutils.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);

void		_PG_init(void);

#ifdef __cplusplus
} // extern "C"
#endif

typedef struct plv8_type_info
{
	Oid			typid;
	Oid			ioparam;
	int32		typmod;
	int16		len;
	bool		byval;
	char		align;
	char		category;
	FmgrInfo	fn_input;
	FmgrInfo	fn_output;

	/* optional, only when category = TYPECATEGORY_ARRAY */
	struct plv8_type_info *elem;
} plv8_type_info;

typedef struct
{
	v8::Persistent<v8::Function>	function;
	char	   *proname;
	char	   *prosrc;

	TransactionId fn_xmin;
	ItemPointerData fn_tid;

	int			nargs;
	plv8_type_info		rettype;
	plv8_type_info		argtypes[FUNC_MAX_ARGS];
/*
	Oid			rettype;
	Oid			retioparam;
	int32		rettypmod;
	char		retcategory;
	FmgrInfo   *fn_retinput;
	int			nargs;
	Oid			argtypes[FUNC_MAX_ARGS];
	char		argcategories[FUNC_MAX_ARGS];
	/ * for array type arguments * /
	Oid			elemtypes[FUNC_MAX_ARGS];
	int16		elemlens[FUNC_MAX_ARGS];
	bool		elembyvals[FUNC_MAX_ARGS];
	char		elemaligns[FUNC_MAX_ARGS];
	
	FmgrInfo   *fn_argoutput[FUNC_MAX_ARGS];
*/
} plv8_info;

typedef struct
{
	char		proc_name[NAMEDATALEN];
	plv8_info  *info_data;
} plv8_info_entry;


v8::Persistent<v8::Context> global_context = v8::Context::New();
v8::TryCatch global_try_catch;
static HTAB *plv8_info_hash = NULL;


//static Datum plv8_func_handler(PG_FUNCTION_ARGS);
static plv8_info *compile_plv8_function(Oid fn_oid, bool is_trigger);

static v8::Persistent<v8::Function> plv8_create_function(const char *proname, int proarglen,
														 const char *proargs[], const char *prosrc);
static Datum plv8_call_jsfunction(FunctionCallInfo fcinfo);
static void plv8_fill_type(Oid typid, plv8_type_info *type, bool forinput, bool output);
static Datum plv8_jsarray_to_datum(v8::Handle<v8::Value> value,
								   bool *isnull, Oid elemtype, Oid elemioparam, int16 elemlen,
								   bool elembyval, char elemalign, FmgrInfo *fn_eleminput);
static Datum plv8_jsval_to_datum(v8::Handle<v8::Value> value,
								 bool *isnull, Oid typid, Oid ioparam, FmgrInfo *fn_input);
static v8::Handle<v8::Value> plv8_datum_to_jsarray(Datum datum,
						 bool isnull, Oid elemtype, int16 elemlen,
						 bool elembyval, char elemalign, FmgrInfo *fn_elemoutput);
static v8::Handle<v8::Value> plv8_datum_to_jsval(Datum datum, bool isnull, Oid type, FmgrInfo *fn_output);
static char *plv8_toutf(char *cstr);
static char *plv8_tolocal(char *cstr);
static void plv8_elog_exception(int errcode, v8::TryCatch *try_catch);
static const char *ToCString(v8::String::Utf8Value &value);


void
_PG_init(void)
{
	static bool inited = false;
	HASHCTL		hash_ctl;

	if (inited)
		return;

	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(plv8_info);

	plv8_info_hash = hash_create("PLv8 Procedures",
								 32,
								 &hash_ctl,
								 HASH_ELEM);

	inited = true;
}

Datum
plv8_call_handler(PG_FUNCTION_ARGS)
{
	Oid			funcOid = fcinfo->flinfo->fn_oid;

	if (!fcinfo->flinfo->fn_extra)
	{
		fcinfo->flinfo->fn_extra = (void *) compile_plv8_function(funcOid, false);
	}

	PG_RETURN_DATUM(plv8_call_jsfunction(fcinfo));
}

Datum
plv8_call_validator(PG_FUNCTION_ARGS)
{
	Oid			funcOid = PG_GETARG_OID(0);
	HeapTuple	tuple;
	Form_pg_proc proc;
	char		functyptype;
	int			numargs;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	bool		istrigger = false;
	int			i;

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID,
						   ObjectIdGetDatum(funcOid),
						   0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", funcOid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, or VOID */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
			(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
			istrigger = true;
		else if (proc->prorettype != RECORDOID &&
				 proc->prorettype != VOIDOID)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/Perl functions cannot return type %s",
							format_type_be(proc->prorettype))));
	}

	/* Disallow pseudotypes in arguments (either IN or OUT) */
	numargs = get_func_arg_info(tuple,
								&argtypes, &argnames, &argmodes);
	for (i = 0; i < numargs; i++)
	{
		if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("PL/v8 functions cannot accept type %s",
							format_type_be(argtypes[i]))));
	}

	ReleaseSysCache(tuple);

	compile_plv8_function(funcOid, istrigger);
	
	/* the result of a validator is ignored */
	PG_RETURN_VOID();
}

//static Datum
//plv8_func_handler(PG_FUNCTION_ARGS)
//{
//	
//}

static plv8_info *
compile_plv8_function(Oid fn_oid, bool is_trigger)
{
	HeapTuple	procTup;
	Form_pg_proc	procStruct;
	char		internal_proname[NAMEDATALEN];
	plv8_info  *info = NULL;
	plv8_info_entry *hash_entry;
	bool		found;
	bool		isnull;
	Datum		procsrcdatum;
	char	   *proc_source;
	Oid		   *argtypes;
	char	  **argnames;
	char	   *argmodes;
	Oid			rettype;
//	Oid			functyptype;
//	Oid			retinput;
	MemoryContext oldcontext;
	int			i;

	procTup = SearchSysCache(PROCOID,
							 ObjectIdGetDatum(fn_oid),
							 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	sprintf(internal_proname, "__PLv8_%u", fn_oid);

	hash_entry = (plv8_info_entry *) hash_search(plv8_info_hash, internal_proname, HASH_FIND, NULL);

	if (hash_entry)
	{
		bool		uptodate;

		info = hash_entry->info_data;

		uptodate = (info->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
					ItemPointerEquals(&info->fn_tid, &procTup->t_self));

		if (!uptodate)
		{
			pfree(info->prosrc);
			pfree(info->proname);
			info->function.Dispose();
			pfree(info);
			info = NULL;
			hash_search(plv8_info_hash, internal_proname, HASH_REMOVE, NULL);
		}
		else
		{
			ReleaseSysCache(procTup);
			return info;
		}
	}

	procStruct = (Form_pg_proc) GETSTRUCT(procTup);

	procsrcdatum = SysCacheGetAttr(PROCOID, procTup,
								   Anum_pg_proc_prosrc, &isnull);
	if (isnull)
		elog(ERROR, "null prosrc");

	proc_source = TextDatumGetCString(procsrcdatum);

	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	info = (plv8_info *) palloc0(sizeof(plv8_info));
	info->proname = pstrdup(NameStr(procStruct->proname));
	info->prosrc = pstrdup(proc_source);
	info->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
	info->fn_tid = procTup->t_self;
	info->nargs = get_func_arg_info(procTup,
									&argtypes, &argnames, &argmodes);

//	functyptype = get_typtype(procStruct->prorettype);
	rettype = procStruct->prorettype;

	ReleaseSysCache(procTup);

	for(i = 0; i < info->nargs; i++)
	{
		Oid		argtype = argtypes[i];
		char	argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

		if (argmode != PROARGMODE_IN)
		{
			elog(ERROR, "argument %d must be IN parameter", i + 1);
		}

		plv8_fill_type(argtype, &info->argtypes[i], false, true);
	}

	plv8_fill_type(rettype, &info->rettype, true, false);

	info->function = plv8_create_function(internal_proname, info->nargs, (const char **) argnames, proc_source);
	hash_entry = (plv8_info_entry *) hash_search(plv8_info_hash, internal_proname, HASH_ENTER, &found);
	hash_entry->info_data = info;

	/* restore */
	MemoryContextSwitchTo(oldcontext);

	return info;
}

static v8::Persistent<v8::Function>
plv8_create_function(const char *proname, int proarglen, const char *proargs[], const char *prosrc)
{
	v8::HandleScope hscope;
	StringInfoData	args;
	StringInfoData	body;
	int				i;

	initStringInfo(&args);
	initStringInfo(&body);

	for(i = 0; i < proarglen; i++)
	{
		appendStringInfo(&args, "%s", proargs[i]);
		if (i != proarglen - 1)
		{
			appendStringInfoString(&args, ", ");
		}
	}

	/*
	 *	function <proname>(<arg1, ...>){
	 *		<prosrc>
	 *	}
	 *	<proname>
	 */
	appendStringInfo(&body, "function %s(%s){\n%s\n};\n%s", proname, args.data, prosrc, proname);
	
	v8::Handle<v8::Value> name = v8::String::New("compile");
	v8::Handle<v8::String> source = v8::String::New(body.data);
	v8::Context::Scope context_scope(global_context);
	v8::TryCatch try_catch;
	v8::Handle<v8::Script> script = v8::Script::Compile(source, name);

	if (script.IsEmpty())
	{
		plv8_elog_exception(ERROR, &try_catch);
	}

	v8::Handle<v8::Value> result = script->Run();
	if (result.IsEmpty())
	{
		plv8_elog_exception(ERROR, &try_catch);
	}

	if (!result->IsFunction())
	{
		elog(ERROR, "!result->IsFunction()");
	}

	return v8::Persistent<v8::Function>::New(v8::Handle<v8::Function>::Cast(result));
}

static Datum
plv8_call_jsfunction(FunctionCallInfo fcinfo)
{
	v8::HandleScope handle_scope;
	v8::Context::Scope context_scope(global_context);
	v8::Handle<v8::Value> args[FUNC_MAX_ARGS];
	int i;
	plv8_info	   *info = (plv8_info *) fcinfo->flinfo->fn_extra;
	plv8_type_info *rettype = &info->rettype;

	for(i = 0; i < info->nargs; i++)
	{
		plv8_type_info	   *type = &info->argtypes[i];

		if (type->category == TYPCATEGORY_ARRAY)
		{
			plv8_type_info	*elem = type->elem;

			args[i] = plv8_datum_to_jsarray(fcinfo->arg[i], fcinfo->argnull[i],
											elem->typid, elem->len,
											elem->byval, elem->align,
											&elem->fn_output);
		}
		else
		{
			args[i] = plv8_datum_to_jsval(fcinfo->arg[i], fcinfo->argnull[i],
										  type->typid, &type->fn_output);
		}
	}

	v8::Handle<v8::Value> result = info->function->Call(global_context->Global(), info->nargs, args);

	if (result.IsEmpty())
	{
		plv8_elog_exception(ERROR, &global_try_catch);
	}

	if (rettype->category == TYPCATEGORY_ARRAY)
	{
		plv8_type_info	   *elem = rettype->elem;

		return plv8_jsarray_to_datum(result,
									 &fcinfo->isnull,
									 elem->typid,
									 elem->ioparam,
									 elem->len,
									 elem->byval,
									 elem->align,
									 &elem->fn_input);
	}
	else
	{
		return plv8_jsval_to_datum(result, &fcinfo->isnull, rettype->typid, rettype->ioparam, &rettype->fn_input);
	}
/*
	else if (result->IsUndefined() || result->IsNull())
	{
		cstr_result = NULL;
		fcinfo->isnull = true;
		return (Datum) 0;
	}

	switch(rettype->typid)
	{
		case INT2OID:
			if (result->IsNumber())
				return Int16GetDatum((int16) (result->Int32Value()));
		case INT4OID:
			if (result->IsNumber())
				return Int32GetDatum((int32) (result->Int32Value()));
		case INT8OID:
			if (result->IsNumber())
				return Int64GetDatum((int64) (result->IntegerValue()));
		case FLOAT4OID:
			if (result->IsNumber())
				return Float4GetDatum((float4) (result->NumberValue()));
		case FLOAT8OID:
			if (result->IsNumber())
				return Float8GetDatum((float8) (result->NumberValue()));
		case BOOLOID:
			if (result->IsBoolean())
				return BoolGetDatum(result->BooleanValue());
		default:
			v8::String::Utf8Value str(result);
			cstr_result = const_cast<char *>(ToCString(str));
			cstr_result = plv8_tolocal(cstr_result);
			return InputFunctionCall(&rettype->fn_input, cstr_result, rettype->ioparam, -1);
	}
	elog(ERROR, "return type and returned value type unmatch");
	return (Datum) 0;
*/
}

static void
plv8_fill_type(Oid typid, plv8_type_info *type, bool forinput, bool foroutput)
{
	char		category;
	bool		ispreferred;

	type->typid = typid;
	get_type_category_preferred(typid, &category, &ispreferred);
	type->category = category;
	get_typlenbyvalalign(typid, &type->len, &type->byval, &type->align);

	if (forinput)
	{
		Oid		input_func;

		getTypeInputInfo(typid, &input_func, &type->ioparam);
		fmgr_info(input_func, &type->fn_input);
	}

	if (foroutput)
	{
		Oid		output_func;
		bool	isvarlen;

		getTypeOutputInfo(typid, &output_func, &isvarlen);
		fmgr_info(output_func, &type->fn_output);
	}

	if (category == TYPCATEGORY_ARRAY)
	{
		Oid			elemid = get_element_type(typid);
		plv8_type_info *elemtype = (plv8_type_info *) palloc0(sizeof(plv8_type_info));

		if (elemid == InvalidOid)
		{
			elog(ERROR, "type(%u):category == '%c' but elemid is invalid", typid, category);
		}
		
		plv8_fill_type(elemid, elemtype, forinput, foroutput);
		type->elem = elemtype;
	}
}

static Datum
plv8_jsarray_to_datum(v8::Handle<v8::Value> value,
					  bool *isnull,
					  Oid elemtype,
					  Oid elemioparam,
					  int16 elemlen,
					  bool elembyval,
					  char elemalign,
					  FmgrInfo *fn_eleminput)
{
	int			i, length;
	Datum	   *dvalues;
	bool	   *dnulls;
	int			ndims[0];
	int			lbs[] = {1};
	ArrayType  *array_type;

	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum ) 0;
	}

	if (!value->IsArray())
	{
		elog(ERROR, "value is not an Array");
	}

	v8::Handle<v8::Array> array(v8::Handle<v8::Array>::Cast(value));

	length = array->Length();
	dvalues = (Datum *) palloc(sizeof(Datum) * length);
	dnulls = (bool *) palloc0(sizeof(bool) * length);
	ndims[0] = length;
	for(i = 0; i < length; i++)
	{
		v8::Handle<v8::Value> el = array->Get(v8::Int32::New(i));
		dvalues[i] = plv8_jsval_to_datum(el, &dnulls[i], elemtype, elemioparam, fn_eleminput);
	}

	array_type = construct_md_array(dvalues, dnulls, 1, ndims, lbs, elemtype, elemlen, elembyval, elemalign);
	pfree(dvalues);
	pfree(dnulls);

	return (Datum) array_type;
}

static Datum
plv8_jsval_to_datum(v8::Handle<v8::Value> value,
					bool *isnull,
					Oid typid,
					Oid ioparam,
					FmgrInfo *fn_input)
{
	*isnull = false;
	if (value->IsUndefined() || value->IsNull())
	{
		*isnull = true;
		return (Datum) 0;
	}

	switch(typid)
	{
		case INT2OID:
			if (value->IsNumber())
				return Int16GetDatum((int16) (value->Int32Value()));
		case INT4OID:
			if (value->IsNumber())
				return Int32GetDatum((int32) (value->Int32Value()));
		case INT8OID:
			if (value->IsNumber())
				return Int64GetDatum((int64) (value->IntegerValue()));
		case FLOAT4OID:
			if (value->IsNumber())
				return Float4GetDatum((float4) (value->NumberValue()));
		case FLOAT8OID:
			if (value->IsNumber())
				return Float8GetDatum((float8) (value->NumberValue()));
		case BOOLOID:
			if (value->IsBoolean())
				return BoolGetDatum(value->BooleanValue());
		default:
			char *cstr;
			v8::String::Utf8Value str(value);
			cstr = const_cast<char *>(ToCString(str));
			cstr = plv8_tolocal(cstr);
			return InputFunctionCall(fn_input, cstr, ioparam, -1);
	}

	elog(ERROR, "datum type and js value type unmatch");
	return (Datum) 0;
}

static v8::Handle<v8::Value>
plv8_datum_to_jsarray(Datum datum,
					  bool isnull,
					  Oid elemtype,
					  int16 elemlen,
					  bool elembyval,
					  char elemalign,
					  FmgrInfo *fn_elemoutput)
{
	Datum	   *dvalues;
	bool	   *dnulls;
	int			nelems;
	int			i;

	if (isnull)
	{
		return v8::Null();
	}

	deconstruct_array((ArrayType *) datum, elemtype, elemlen, elembyval, elemalign,
					  &dvalues, &dnulls, &nelems);
	v8::Local<v8::Array>	result = v8::Array::New(nelems);
	for(i = 0; i < nelems; i++)
	{
		v8::Handle<v8::Value> val = plv8_datum_to_jsval(dvalues[i], dnulls[i], elemtype, fn_elemoutput);
		result->Set(v8::Int32::New(i), val);
	}

	return result;
}

static v8::Handle<v8::Value>
plv8_datum_to_jsval(Datum datum, bool isnull, Oid type, FmgrInfo *fn_output)
{
	char	   *value_text;

	if (isnull)
	{
		return v8::Null();
	}

	switch(type)
	{
		case INT2OID:
			return v8::Int32::New((int32_t) DatumGetInt16(datum));
		case INT4OID:
			return v8::Int32::New((int32_t) DatumGetInt32(datum));
		case INT8OID:
			return v8::Number::New((double) DatumGetInt64(datum));
		case FLOAT4OID:
			return v8::Number::New((double) DatumGetFloat4(datum));
		case FLOAT8OID:
			return v8::Number::New((double) DatumGetFloat8(datum));
		case BOOLOID:
			return v8::Boolean::New((bool) DatumGetBool(datum));
		default:
			value_text = OutputFunctionCall(fn_output, datum);
			value_text = plv8_toutf(value_text);
			return v8::String::New(value_text);
	}
}

static char *
plv8_toutf(char *cstr)
{
	int		db_encoding = GetDatabaseEncoding();

	if (db_encoding == PG_UTF8)
		return cstr;

	return (char *) pg_do_encoding_conversion(reinterpret_cast<unsigned char *>(cstr),
											  strlen(cstr), db_encoding, PG_UTF8);
}

static char *
plv8_tolocal(char *cstr)
{
	int		db_encoding = GetDatabaseEncoding();

	if (db_encoding == PG_UTF8)
		return cstr;

	return (char *) pg_do_encoding_conversion(reinterpret_cast<unsigned char *>(cstr),
											  strlen(cstr), PG_UTF8, db_encoding);
}

static void
plv8_elog_exception(int errcode, v8::TryCatch *try_catch)
{
	v8::String::Utf8Value exception(try_catch->Exception());
	const char *exception_string = ToCString(exception);

	elog(errcode, "%s", exception_string);
}

static const char *
ToCString(v8::String::Utf8Value &value)
{
	return *value ? *value : "<string conversion failed>";
}
