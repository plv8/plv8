/*-------------------------------------------------------------------------
 *
 * plv8.cc : PL/v8 handler routines.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"

#ifdef _MSC_VER
#undef open
#endif

#include "libplatform/libplatform.h"
#include "plv8_allocator.h"

#include <new>

extern "C" {
#if PG_VERSION_NUM >= 90300
#include "access/htup_details.h"
#endif
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#if PG_VERSION_NUM >= 120000
#include "catalog/pg_database.h"
#endif

#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#endif

#include <signal.h>

#ifdef EXECUTION_TIMEOUT
#ifdef _MSC_VER
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

PG_MODULE_MAGIC;

PGDLLEXPORT Datum	plv8_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_call_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plcoffee_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plcoffee_call_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plls_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plls_call_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_reset(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_info(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);
PG_FUNCTION_INFO_V1(plcoffee_call_handler);
PG_FUNCTION_INFO_V1(plcoffee_call_validator);
PG_FUNCTION_INFO_V1(plls_call_handler);
PG_FUNCTION_INFO_V1(plls_call_validator);
PG_FUNCTION_INFO_V1(plv8_reset);
PG_FUNCTION_INFO_V1(plv8_info);


PGDLLEXPORT void _PG_init(void);

#if PG_VERSION_NUM >= 90000
PGDLLEXPORT Datum	plv8_inline_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plcoffee_inline_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plls_inline_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_inline_handler);
PG_FUNCTION_INFO_V1(plcoffee_inline_handler);
PG_FUNCTION_INFO_V1(plls_inline_handler);
#endif
} // extern "C"

using namespace v8;

typedef struct plv8_proc_key {
	Oid fn_oid;
	Oid user_id;
	char user_ctx[NAMEDATALEN];
} plv8_proc_key;

typedef struct plv8_proc_cache
{
	plv8_proc_key			key;

	Persistent<Function>	function;
	char					proname[NAMEDATALEN];
	char				   *prosrc;

	TransactionId			fn_xmin;
	ItemPointerData			fn_tid;

	int						nargs;
	bool					retset;		/* true if SRF */
	Oid						rettype;
	Oid						argtypes[FUNC_MAX_ARGS];
} plv8_proc_cache;

plv8_runtime *current_runtime = nullptr;
size_t plv8_memory_limit = 0;
size_t plv8_last_heap_size = 0;

/*
 * The function and context are created at the first invocation.  Their
 * lifetime is same as plv8_proc, but they are not palloc'ed memory,
 * so we need to clear them at the end of transaction.
 */
typedef struct plv8_exec_env
{
	Isolate 			   *isolate;
	Persistent<Object>		recv;
	struct plv8_exec_env   *next;
} plv8_exec_env;

/*
 * We cannot cache plv8_type inter executions because it has FmgrInfo fields.
 * So, we cache rettype and argtype in fn_extra only during one execution.
 */
typedef struct plv8_proc
{
	plv8_proc_cache		   *cache;
	plv8_exec_env		   *xenv;
	TypeFuncClass			functypclass;			/* For SRF */
	plv8_type				rettype;
	plv8_type				argtypes[FUNC_MAX_ARGS];
} plv8_proc;

static HTAB *plv8_proc_cache_hash = NULL;

static plv8_exec_env		   *exec_env_head = NULL;

extern const unsigned char coffee_script_binary_data[];
extern const unsigned char livescript_binary_data[];

static void ClearProcCache(plv8_runtime *runtime, const char *context_id = nullptr);
static void KillRuntime(plv8_runtime *runtime);
static bool CheckTermination(Isolate *isolate);

/*
 * lower_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo,
		bool validate, char ***argnames) throw();
static void plv8_xact_cb(XactEvent event, void *arg);

/*
 * CamelCaseFunctions are C++ functions.
 * They could raise errors with C++ throw statements, or never throw exceptions.
 */
static plv8_exec_env *CreateExecEnv(Handle<Function> function, plv8_runtime *runtime);
static plv8_exec_env *CreateExecEnv(Persistent<Function>& function, plv8_runtime *runtime);
static plv8_proc *Compile(Oid fn_oid, FunctionCallInfo fcinfo,
					bool validate, bool is_trigger, Dialect dialect);
static Local<Function> CompileFunction(plv8_runtime *runtime,
									   const char *proname, int proarglen,
									   const char *proargs[], const char *prosrc,
									   bool is_trigger, bool retset, Dialect dialect);
static Datum CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv);
static plv8_runtime *GetPlv8Runtime();
static Local<ObjectTemplate> GetGlobalObjectTemplate(plv8_runtime *runtime);
static void CreateIsolate(plv8_runtime *runtime);

/* A GUC to specify a custom start up function to call (superuser only) */
static char *plv8_boot_proc = NULL;

/* A GUC to specify a custom start up function to call */
static char *plv8_start_proc = NULL;

/* A GUC to specify V8 flags (e.g. --es_staging) */
static char *plv8_v8_flags = NULL;

/* A GUC to specify the ICU data directory */
static char *plv8_icu_data = NULL;

/* A GUC to specify the remote debugger port */
static int plv8_debugger_port;

#ifdef EXECUTION_TIMEOUT
static int plv8_execution_timeout = 300;
#endif

/* A GUC to specify the user context LRU cache size (number of entries) */
static size_t plv8_context_cache_size = 8;

/* A GUC to specify custom execution context */
char *plv8_user_context = nullptr;

/* A GUC to specify max code size for eval(), setting to -1 disables limits */
static int plv8_max_eval_size = -1;

static std::unique_ptr<v8::Platform> v8_platform = NULL;

/*
 * We use vector instead of hash since the size of this array
 * is expected to be short in most cases.
 */
static std::vector<plv8_runtime *> RuntimeCache;

#ifdef ENABLE_DEBUGGER_SUPPORT
v8::Persistent<v8::Context> debug_message_context;

void DispatchDebugMessages() {
  // We are in some random thread. We should already have v8::Locker acquired
  // (we requested this when registered this callback). We was called
  // because new debug messages arrived; they may have already been processed,
  // but we shouldn't worry about this.
  //
  // All we have to do is to set context and call ProcessDebugMessages.
  //
  // We should decide which V8 context to use here. This is important for
  // "evaluate" command, because it must be executed some context.
  // In our sample we have only one context, so there is nothing really to
  // think about.
  v8::Context::Scope scope(debug_message_context);

  v8::Debug::ProcessDebugMessages();
}
#endif  // ENABLE_DEBUGGER_SUPPORT

void OOMErrorHandler(const char* location, bool is_heap_oom) {
	Isolate *isolate = Isolate::GetCurrent();
	isolate->TerminateExecution();
	// set it to kill the whole user runtime
	current_runtime->is_dead = true;
	// here lie the fattest dragons of the whole kingdom
	// elog(ERROR, ...) will long jump over all the non-trivial destructors in V8
	// which is _undefined behavior_
	// we will kill the OOMed isolate with fire next time we get control, see KillRuntime() usage
	// but if not for this hack - we die and bring the whole postgresql with us
	elog(ERROR, "out of memory");
}

void GCEpilogueCallback(Isolate* isolate, GCType type, GCCallbackFlags /* flags */) {
	HeapStatistics heap_statistics;
	isolate->GetHeapStatistics(&heap_statistics);
	if (type != GCType::kGCTypeIncrementalMarking
		&& heap_statistics.used_heap_size() > plv8_memory_limit * 1_MB) {
		isolate->TerminateExecution();
	}
	if (heap_statistics.used_heap_size() > plv8_memory_limit * 1_MB / 0.9
		&& plv8_last_heap_size < plv8_memory_limit * 1_MB / 0.9) {
		isolate->LowMemoryNotification();
	}
	plv8_last_heap_size = heap_statistics.used_heap_size();
}

size_t NearHeapLimitHandler(void* data, size_t current_heap_limit,
								size_t initial_heap_limit) {
	Isolate *isolate = Isolate::GetCurrent();
	isolate->TerminateExecution();
	// need to give back more space
	// to make sure it can unwind the stack and process exceptions
	return current_heap_limit + 1_MB;
}

ModifyCodeGenerationFromStringsResult CodeGenCallback(Local<Context> /* context */, Local<v8::Value> source) {
	ModifyCodeGenerationFromStringsResult result;
	if (source->IsString() && source.As<String>()->Length() > static_cast<int>(plv8_max_eval_size)) {
		CString src(source.As<String>());
		elog(LOG_SERVER_ONLY, "plv8: eval refused, source length > %d: %s", plv8_max_eval_size, src.str());
		result.codegen_allowed = false;
	} else {
		result.codegen_allowed = true;
	}
	return result;
}

static void
CreateIsolate(plv8_runtime *runtime) {
	Isolate *isolate;
	Isolate::CreateParams params;
	params.array_buffer_allocator = new ArrayAllocator(plv8_memory_limit * 1_MB);
	ResourceConstraints rc;
	rc.ConfigureDefaults(plv8_memory_limit * 1_MB * 2, plv8_memory_limit * 1_MB * 2);
	params.constraints = rc;
	isolate = Isolate::New(params);
	isolate->SetOOMErrorHandler(OOMErrorHandler);
	isolate->AddGCEpilogueCallback(GCEpilogueCallback);
	isolate->AddNearHeapLimitCallback(NearHeapLimitHandler, NULL);
	if (plv8_max_eval_size >= 0)
		isolate->SetModifyCodeGenerationFromStringsCallback(CodeGenCallback);
	runtime->isolate = isolate;
	runtime->array_buffer_allocator = params.array_buffer_allocator;
}

static bool
user_context_check_hook(char **newvalue, void **extra, GucSource source)
{
	if (newvalue != nullptr && *newvalue != nullptr && strnlen(*newvalue, NAMEDATALEN) == NAMEDATALEN)
	{
		GUC_check_errdetail("user context id is too long, max %d characters", (NAMEDATALEN - 1));
		return false;
	}
	else if (Isolate::GetCurrent() != nullptr && Isolate::GetCurrent()->IsInUse())
	{
		GUC_check_errdetail("cannot set context from inside a running transaction");
		return false;
	}
	return true;
}

void
_PG_init(void)
{
	HASHCTL    hash_ctl = { 0 };

	hash_ctl.keysize = sizeof(plv8_proc_key);
	hash_ctl.entrysize = sizeof(plv8_proc_cache);
	plv8_proc_cache_hash = hash_create("PLv8 Procedures", 32,
									   &hash_ctl, HASH_ELEM | HASH_BLOBS);

	config_generic *guc_value;

#define BOOT_PROC_VAR "plv8.boot_proc"
	guc_value = plv8_find_option(BOOT_PROC_VAR);
	if (guc_value != NULL) {
		plv8_boot_proc = plv8_string_option(guc_value);
	} else {
		DefineCustomStringVariable(BOOT_PROC_VAR,
								   gettext_noop("PLV8 function to run once when any context is first used."),
								   gettext_noop("Superuser only, if set disables plv8.start_proc"),
								   &plv8_boot_proc,
								   NULL,
								   PGC_SUSET, GUC_SUPERUSER_ONLY,
#if PG_VERSION_NUM >= 90100
								   NULL,
#endif
								   NULL,
								   NULL);
	}
#undef BOOT_PROC_VAR

#define START_PROC_VAR "plv8.start_proc"
	guc_value = plv8_find_option(START_PROC_VAR);
	if (guc_value != NULL) {
		plv8_start_proc = plv8_string_option(guc_value);
	} else {
		DefineCustomStringVariable(START_PROC_VAR,
								   gettext_noop("PLV8 function to run once when PLV8 is first used."),
								   NULL,
								   &plv8_start_proc,
								   NULL,
								   PGC_USERSET, 0,
#if PG_VERSION_NUM >= 90100
								   NULL,
#endif
								   NULL,
								   NULL);
	}
#undef START_PROC_VAR

#define ICU_DATA_VAR "plv8.icu_data"
	guc_value = plv8_find_option(ICU_DATA_VAR);
	if (guc_value != NULL) {
		plv8_icu_data = plv8_string_option(guc_value);
	} else {
		DefineCustomStringVariable(ICU_DATA_VAR,
								   gettext_noop("ICU data file directory."),
								   NULL,
								   &plv8_icu_data,
								   NULL,
								   PGC_SUSET, 0,
#if PG_VERSION_NUM >= 90100
								   NULL,
#endif
								   NULL,
								   NULL);
	}
#undef ICU_DATA_VAR

#define V8_FLAGS_VAR "plv8.v8_flags"
	guc_value = plv8_find_option(V8_FLAGS_VAR);
	if (guc_value != NULL) {
		plv8_v8_flags = plv8_string_option(guc_value);
	} else {
		DefineCustomStringVariable(V8_FLAGS_VAR,
								   gettext_noop(
										   "V8 engine initialization flags (e.g. --harmony for all current harmony features)."),
								   NULL,
								   &plv8_v8_flags,
								   NULL,
								   PGC_SUSET, 0,
#if PG_VERSION_NUM >= 90100
								   NULL,
#endif
								   NULL,
								   NULL);
	}
#undef V8_FLAGS_VAR

#define DEBUGGER_PORT_VAR "plv8.debugger_port"
	guc_value = plv8_find_option(DEBUGGER_PORT_VAR);
	if (guc_value != NULL) {
		plv8_debugger_port = plv8_int_option(guc_value);
	} else {
		DefineCustomIntVariable(DEBUGGER_PORT_VAR,
								gettext_noop("V8 remote debug port."),
								gettext_noop("The default value is 35432.  "
											 "This is effective only if PLV8 is built with ENABLE_DEBUGGER_SUPPORT."),
								&plv8_debugger_port,
								35432, 0, 65536,
								PGC_USERSET, 0,
#if PG_VERSION_NUM >= 90100
								NULL,
#endif
								NULL,
								NULL);
	}
#undef DEBUGGER_PORT_VAR

#ifdef EXECUTION_TIMEOUT
#define EXECUTION_TIMEOUT_VAR "plv8.execution_timeout"
	guc_value = plv8_find_option(EXECUTION_TIMEOUT_VAR);
	if (guc_value != NULL) {
		plv8_execution_timeout = plv8_int_option(guc_value);
	} else {
		DefineCustomIntVariable(EXECUTION_TIMEOUT_VAR,
								gettext_noop("V8 execution timeout."),
								gettext_noop("The default value is 300 seconds.  "
											 "This allows you to override the default execution timeout."),
								&plv8_execution_timeout,
								300, 1, 65536,
								PGC_SUSET, 0,
#if PG_VERSION_NUM >= 90100
								NULL,
#endif
								NULL,
								NULL);
	}
#undef EXECUTION_TIMEOUT_VAR
#endif

#define MEMORY_LIMIT_VAR "plv8.memory_limit"
	guc_value = plv8_find_option(MEMORY_LIMIT_VAR);
	if (guc_value != NULL) {
		plv8_memory_limit = plv8_int_option(guc_value);
	} else {
		DefineCustomIntVariable(MEMORY_LIMIT_VAR,
								gettext_noop("Per-isolate memory limit in MBytes"),
								gettext_noop("The default value is 256 MB"),
								(int *) &plv8_memory_limit,
								256, 256, 3096, // hardcoded v8 limits for isolates
								PGC_SUSET, 0,
#if PG_VERSION_NUM >= 90100
								NULL,
#endif
								NULL,
								NULL);
	}
#undef MEMORY_LIMIT_VAR

#define USER_CONTEXT_VAR "plv8.context"
	guc_value = plv8_find_option(USER_CONTEXT_VAR);
	if (guc_value != NULL) {
		plv8_user_context = plv8_string_option(guc_value);
	} else {
		DefineCustomStringVariable(USER_CONTEXT_VAR,
								   gettext_noop("User-defined context id"),
								   gettext_noop("Can be used to set up different execution contexts"),
								   &plv8_user_context,
								   nullptr,
								   PGC_USERSET, 0,
#if PG_VERSION_NUM >= 90100
								   &user_context_check_hook,
#endif
								   nullptr,
								   nullptr);
	}
#undef USER_CONTEXT_VAR

#define USER_CONTEXT_SIZE_VAR "plv8.context_cache_size"
	guc_value = plv8_find_option(USER_CONTEXT_SIZE_VAR);
	if (guc_value != NULL) {
		plv8_context_cache_size = plv8_int_option(guc_value);
	} else {
		DefineCustomIntVariable(USER_CONTEXT_SIZE_VAR,
								gettext_noop("Number of live contexts in user context cache"),
								gettext_noop("The default is 8, which means 10 contexts overall (+ default + compile)."
											 "The cache will evict and destroy contexts over this number (using LRU)"),
								(int *)&plv8_context_cache_size,
								8, 0, 64,
								PGC_SUSET, 0,
#if PG_VERSION_NUM >= 90100
								NULL,
#endif
								NULL,
								NULL);
	}
#undef USER_CONTEXT_SIZE_VAR

#define MAX_EVAL_SIZE_VAR "plv8.max_eval_size"
	guc_value = plv8_find_option(MAX_EVAL_SIZE_VAR);
	if (guc_value != NULL) {
		plv8_max_eval_size = plv8_int_option(guc_value);
	} else {
		DefineCustomIntVariable(MAX_EVAL_SIZE_VAR,
								gettext_noop("Max size in bytes for code to eval()"),
								gettext_noop("The default is -1 (limit disabled), setting to 0 disables eval() completely!"
											 "Setting to any value > 0 will refuse to eval() a string larger than that"),
								(int *)&plv8_max_eval_size,
								-1, -1, 128_MB,
								PGC_SUSET, 0,
#if PG_VERSION_NUM >= 90100
								NULL,
#endif
								NULL,
								NULL);
	}
#undef MAX_EVAL_SIZE_VAR

	RegisterXactCallback(plv8_xact_cb, NULL);

	EmitWarningsOnPlaceholders("plv8");

	if (plv8_icu_data == NULL) {
		elog(DEBUG1, "no icu dir");
		V8::InitializeICU();
	} else {
		elog(DEBUG1, "init icu data %s", plv8_icu_data);
		V8::InitializeICU(plv8_icu_data);
	}

#if (V8_MAJOR_VERSION == 4 && V8_MINOR_VERSION >= 6) || V8_MAJOR_VERSION >= 5
	V8::InitializeExternalStartupData("plv8");
#endif
	if (!v8_platform) {
		v8_platform = platform::NewDefaultPlatform();
	}
	V8::InitializePlatform(v8_platform.get());
	V8::Initialize();
	if (plv8_v8_flags != NULL) {
		V8::SetFlagsFromString(plv8_v8_flags, strlen(plv8_v8_flags));
	}
}

static void
plv8_xact_cb(XactEvent event, void *arg)
{
	plv8_exec_env	   *env = exec_env_head;

	while (env)
	{
		if (!env->recv.IsEmpty())
		{
			env->recv.Reset();
		}
		env = env->next;
		/*
		 * Each item was allocated in TopTransactionContext, so
		 * it will be freed eventually.
		 */
	}
	exec_env_head = NULL;
}

static inline plv8_exec_env *
plv8_new_exec_env(Isolate *isolate)
{
	plv8_exec_env	   *xenv = (plv8_exec_env *)
		MemoryContextAllocZero(TopTransactionContext, sizeof(plv8_exec_env));

	new(&xenv->recv) Persistent<Object>();
	xenv->isolate = isolate;

	/*
	 * Add it to the list, which will be freed in the end of top transaction.
	 */
	xenv->next = exec_env_head;
	exec_env_head = xenv;

	return xenv;
}

static Datum
common_pl_call_handler(PG_FUNCTION_ARGS, Dialect dialect) throw()
{
	Oid		fn_oid = fcinfo->flinfo->fn_oid;
	bool	is_trigger = CALLED_AS_TRIGGER(fcinfo);

	try
	{
		current_runtime = GetPlv8Runtime();
#ifdef ENABLE_DEBUGGER_SUPPORT
		Locker				lock;
#endif  // ENABLE_DEBUGGER_SUPPORT
		Isolate::Scope	scope(current_runtime->isolate);
		HandleScope	handle_scope(current_runtime->isolate);

		if (!fcinfo->flinfo->fn_extra)
		{
			plv8_proc	   *proc = Compile(fn_oid, fcinfo,
										   false, is_trigger, dialect);
			proc->xenv = CreateExecEnv(proc->cache->function, current_runtime);
			fcinfo->flinfo->fn_extra = proc;
		}

		plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
		plv8_proc_cache *cache = proc->cache;

		if (is_trigger)
			return CallTrigger(fcinfo, proc->xenv);
		else if (cache->retset)
			return CallSRFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
		else
			return CallFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

Datum
plv8_call_handler(PG_FUNCTION_ARGS)
{
	return common_pl_call_handler(fcinfo, PLV8_DIALECT_NONE);
}

Datum
plcoffee_call_handler(PG_FUNCTION_ARGS)
{
	return common_pl_call_handler(fcinfo, PLV8_DIALECT_COFFEE);
}

Datum
plls_call_handler(PG_FUNCTION_ARGS)
{
	return common_pl_call_handler(fcinfo, PLV8_DIALECT_LIVESCRIPT);
}

static void ClearProcCache(plv8_runtime *runtime, const char *context_id)
{
	HASH_SEQ_STATUS		status;
	plv8_proc_cache*	cache;

	hash_seq_init(&status, plv8_proc_cache_hash);
	cache = (plv8_proc_cache *) hash_seq_search(&status);
	while (cache != nullptr)
	{
		if (cache->key.user_id == runtime->user_id)
		{
			if (context_id == nullptr)
			{
				cache->function.Reset();
				if (hash_search(plv8_proc_cache_hash,
								(void *) &cache->key,
								HASH_REMOVE,
								nullptr) == nullptr)
					elog(ERROR, "proc hash table corrupted");
			}
			else if (strncmp(cache->key.user_ctx, context_id, NAMEDATALEN) == 0)
			{
				cache->function.Reset();
				if (hash_search(plv8_proc_cache_hash,
								(void *) &cache->key,
								HASH_REMOVE,
								nullptr) == nullptr)
					elog(ERROR, "proc hash table corrupted");
			}
		}
		cache = (plv8_proc_cache *) hash_seq_search(&status);
	}
}

static void KillRuntime(plv8_runtime *runtime)
{
	runtime->isolate->Dispose();
	delete runtime->array_buffer_allocator;
}

Datum
plv8_reset(PG_FUNCTION_ARGS)
{
	Oid					user_id = GetUserId();
	unsigned long		i;
	const char 			*ptr = PG_GETARG_POINTER(0);
	const char			*context_id;

	if (ptr != nullptr)
		context_id = text_to_cstring(DatumGetTextPP(ptr));
	else
		context_id = nullptr;
	for (i = 0; i < RuntimeCache.size(); i++)
	{
		if (RuntimeCache[i]->user_id == user_id)
		{
			plv8_runtime *runtime = RuntimeCache[i];
			if (runtime->isolate->IsInUse()) // was launched from SPI, cannot kill "self"
				elog(ERROR, "Cannot be run from inside the transaction context");
			if (context_id != nullptr)
				runtime->removeContext(context_id);
			else
			{
				// kill the runtime completely
				if (current_runtime == runtime)
					current_runtime = nullptr;
				ClearProcCache(runtime);
				KillRuntime(runtime);
				RuntimeCache.erase(RuntimeCache.begin() + i);
				pfree(runtime);
			}
			break;
		}
	}
	return (Datum) 0;
}

Datum
plv8_info(PG_FUNCTION_ARGS)
{
	unsigned long		i;
	unsigned long 		size = RuntimeCache.size();
	char 				*infos[size];
	size_t 				lengths[size];
	size_t 				total_length = 3; // length of "[]\0"

	for (i = 0; i < size; i++)
	{
		Isolate 	   	   *isolate = RuntimeCache[i]->isolate;
		Isolate::Scope		scope(isolate);
		HandleScope			handle_scope(isolate);
		Local<Context>		context = RuntimeCache[i]->compile_context.Get(isolate);
		Context::Scope		context_scope(context);
		JSONObject 			JSON;
		Local<v8::Value>	result;
		Local<v8::Object>	infoObj = v8::Object::New(isolate);
		Local<v8::Array>  	contextList = v8::Array::New(isolate);

#if PG_VERSION_NUM >= 90500
		char 			   *username = GetUserNameFromId(RuntimeCache[i]->user_id, false);
#else
		char 			   *username = GetUserNameFromId(ContextVector[i]->user_id);
#endif
		infoObj->Set(context, String::NewFromUtf8Literal(isolate, "user"),
					 String::NewFromUtf8(isolate, username).ToLocalChecked()).Check();
		GetMemoryInfo(infoObj);
		size_t idx = 0;
		for (auto &it: RuntimeCache[i]->ctx_queue) {
			Local<v8::String> key = String::NewFromUtf8(isolate, std::get<0>(it).c_str()).ToLocalChecked();
			contextList->Set(context, idx++, key).Check();
		}
		infoObj->Set(context, String::NewFromUtf8Literal(isolate, "contexts"), contextList).Check();

		result = JSON.Stringify(infoObj);
		CString str(result);

		infos[i] = pstrdup(str.str());
		lengths[i] = strlen(infos[i]);
		total_length += lengths[i] + 1; // add 1 byte for ','
	}
	char *out = (char *) palloc0(total_length);
	out[0] = '[';
	size_t current = 0;
	for (i = 0; i < size; i++)
	{
		++current;
		strcpy(out + current, infos[i]);
		pfree(infos[i]);
		current += lengths[i];
		out[current] = ',';
	}
	// replace ",\0" with "]\0" at the end
	out[(current > 0) ? current : total_length - 2] = ']';
	return CStringGetTextDatum(out);
}

#if PG_VERSION_NUM >= 90000
static Datum
common_pl_inline_handler(PG_FUNCTION_ARGS, Dialect dialect) throw()
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));

	Assert(IsA(codeblock, InlineCodeBlock));

	try
	{
#ifdef ENABLE_DEBUGGER_SUPPORT
		Locker				lock;
#endif  // ENABLE_DEBUGGER_SUPPORT
		current_runtime = GetPlv8Runtime();
		Isolate::Scope		scope(current_runtime->isolate);
		HandleScope			handle_scope(current_runtime->isolate);
		char			   *source_text = codeblock->source_text;

		Local<Function>	function = CompileFunction(current_runtime,
													  NULL, 0, NULL,
													  source_text, false, false, dialect);
		plv8_exec_env	   *xenv = CreateExecEnv(function, current_runtime);
		return CallFunction(fcinfo, xenv, 0, NULL, NULL);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

Datum
plv8_inline_handler(PG_FUNCTION_ARGS)
{
	return common_pl_inline_handler(fcinfo, PLV8_DIALECT_NONE);
}

Datum
plcoffee_inline_handler(PG_FUNCTION_ARGS)
{
	return common_pl_inline_handler(fcinfo, PLV8_DIALECT_COFFEE);
}

Datum
plls_inline_handler(PG_FUNCTION_ARGS)
{
	return common_pl_inline_handler(fcinfo, PLV8_DIALECT_LIVESCRIPT);
}
#endif

#ifdef EXECUTION_TIMEOUT
/*
 * Breakout -- break out of a Call, with a thread
 *
 * This function breaks out of a javascript execution context.
 */
#ifdef _MSC_VER // windows
DWORD WINAPI
Breakout (LPVOID lpParam)
{
	Isolate *isolate = (Isolate *) lpParam;
	Sleep(plv8_execution_timeout * 1000);
	isolate->TerminateExecution();

	return 0;
}
#else // posix
void *
Breakout (void *d)
{
	auto *isolate = (Isolate *) d;
	sleep(plv8_execution_timeout);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	isolate->TerminateExecution();

	return NULL;
}
#endif
#endif
void *int_handler = NULL;
void *term_handler = NULL;

/*
 * signal handler
 *
 * This function kills the execution of the v8 process if a signal is called
 */
void
signal_handler (int sig) {
	elog(DEBUG1, "cancelling execution");
	auto runtime = GetPlv8Runtime();
	runtime->interrupted = true;
	runtime->isolate->TerminateExecution();
}

/*
 * DoCall -- Call a JS function with SPI support.
 *
 * This function could throw C++ exceptions, but must not throw PG exceptions.
 */
static Local<v8::Value>
DoCall(Local<Context> ctx, Handle<Function> fn, Handle<Object> receiver,
	int nargs, Handle<v8::Value> args[], bool nonatomic)
{
	Isolate 	   *isolate = ctx->GetIsolate();
	TryCatch		try_catch(isolate);

	CheckTermination(isolate);

#if PG_VERSION_NUM >= 110000
	if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT)
		throw js_error("could not connect to SPI manager");
#else
	if (SPI_connect() != SPI_OK_CONNECT)
		throw js_error("could not connect to SPI manager");
#endif

	// set up the signal handlers
	int_handler = (void *) signal(SIGINT, signal_handler);
	term_handler = (void *) signal(SIGTERM, signal_handler);

#ifdef EXECUTION_TIMEOUT
	bool timeout = false;
#ifdef _MSC_VER // windows
	HANDLE  hThread;
	DWORD dwThreadId;
	hThread = CreateThread(NULL, 0, Breakout, isolate, 0, &dwThreadId);
#else
	pthread_t breakout_thread;
	// set up the thread to break out the execution if needed
	pthread_create(&breakout_thread, NULL, Breakout, isolate);
#endif
#endif

	MaybeLocal<v8::Value> result = fn->Call(ctx, receiver, nargs, args);
	int	status = SPI_finish();

#ifdef EXECUTION_TIMEOUT
#ifdef _MSC_VER
	if (TerminateThread(hThread, NULL) == 0) {
		timeout = true;
	}
#else
	void *thread_result;
	pthread_cancel(breakout_thread);
	pthread_join(breakout_thread, &thread_result);
	if (thread_result == NULL) {
		timeout = true;
	}
#endif
#endif

	// reset signal handlers
	signal(SIGINT, (void (*)(int)) int_handler);
	signal(SIGTERM, (void (*)(int)) term_handler);

	if (result.IsEmpty()) {
		if (CheckTermination(isolate))
		{
			if (current_runtime->interrupted)
			{
				current_runtime->interrupted = false;
				throw js_error("Signal caught: interrupted");
			}
#ifdef EXECUTION_TIMEOUT
			if (timeout)
				throw js_error("execution timeout exceeded");
#endif
			// currently the only "un-catchable" error is OOM, that's why we just assume it here
			throw js_error("Out of memory error");
		}
		throw js_error(try_catch);
	}

	if (status < 0)
		throw js_error(FormatSPIStatus(status));

	return result.ToLocalChecked();
}

static Datum
CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	Local<Context>		context = current_runtime->localContext();
	Context::Scope		context_scope(context);
	Handle<v8::Value>	args[FUNC_MAX_ARGS];
	Handle<Object>		plv8obj;

#if PG_VERSION_NUM >= 110000
	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;
#else
  bool nonatomic = false;
#endif

	WindowFunctionSupport support(context, fcinfo);

	/*
	 * In window function case, we cannot see the argument datum
	 * in fcinfo.  Instead, get them by WinGetFuncArgCurrent().
	 */
	if (support.IsWindowCall())
	{
		WindowObject winobj = support.GetWindowObject();
		for (int i = 0; i < nargs; i++)
		{
			bool isnull;
			Datum arg = WinGetFuncArgCurrent(winobj, i, &isnull);
			args[i] = ToValue(arg, isnull, &argtypes[i]);
		}
	}
	else
	{
		for (int i = 0; i < nargs; i++) {
#if PG_VERSION_NUM < 120000
			args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);
#else
			args[i] = ToValue(fcinfo->args[i].value, fcinfo->args[i].isnull, &argtypes[i]);
#endif
		}
	}

	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn =
		Local<Function>::Cast(recv->GetInternalField(0));
	Local<v8::Value> result =
		DoCall(context, fn, recv, nargs, args, nonatomic);

	if (rettype)
		return ToDatum(result, &fcinfo->isnull, rettype);
	else
		PG_RETURN_VOID();
}

static Tuplestorestate *
CreateTupleStore(PG_FUNCTION_ARGS, TupleDesc *tupdesc)
{
	Tuplestorestate	   *tupstore;

	PG_TRY();
	{
		ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		MemoryContext	per_query_ctx;
		MemoryContext	oldcontext;
		plv8_proc	   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;

		/* check to see if caller supports us returning a tuplestore */
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		if (!(rsinfo->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not " \
							"allowed in this context")));

		if (!proc->functypclass)
			proc->functypclass = get_call_result_type(fcinfo, NULL, NULL);

		per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		tupstore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->returnMode = SFRM_Materialize;
		rsinfo->setResult = tupstore;
		/* Build a tuple descriptor for our result type */
		if (proc->rettype.typid == RECORDOID)
		{
			if (proc->functypclass != TYPEFUNC_COMPOSITE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
		}
		if (!rsinfo->setDesc)
		{
			*tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
			rsinfo->setDesc = *tupdesc;
		}
		else
			*tupdesc = rsinfo->setDesc;

		MemoryContextSwitchTo(oldcontext);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return tupstore;
}

static Datum
CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	plv8_proc		   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;

#if PG_VERSION_NUM >= 110000
	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;
#else
  bool nonatomic = false;
#endif

	tupstore = CreateTupleStore(fcinfo, &tupdesc);

	Handle<Context>		context = current_runtime->localContext();
	Context::Scope		context_scope(context);
	Converter			conv(tupdesc, proc->functypclass == TYPEFUNC_SCALAR);
	Handle<v8::Value>	args[FUNC_MAX_ARGS + 1];

	/*
	 * In case this is nested via SPI, stash pre-registered converters
	 * for the previous SRF.
	 */
	SRFSupport support(context, &conv, tupstore);

	for (int i = 0; i < nargs; i++) {
#if PG_VERSION_NUM < 120000
		args[i] = ToValue(fcinfo->arg[i], fcinfo->argnull[i], &argtypes[i]);
#else
		args[i] = ToValue(fcinfo->args[i].value, fcinfo->args[i].isnull, &argtypes[i]);
#endif
	}

	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn =
		Local<Function>::Cast(recv->GetInternalField(0));

	Handle<v8::Value> result = DoCall(context, fn, recv, nargs, args, nonatomic);

	if (result->IsUndefined())
	{
		// no additional values
	}
	else if (result->IsArray())
	{
		Handle<Array> array = Handle<Array>::Cast(result);
		// return an array of records.
		int	length = array->Length();
		for (int i = 0; i < length; i++)
			conv.ToDatum(array->Get(context, i).ToLocalChecked(), tupstore);
	}
	else
	{
		// return a record or a scalar
		conv.ToDatum(result, tupstore);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

static Datum
CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv)
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

#if PG_VERSION_NUM >= 110000
	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;
#else
  bool nonatomic = false;
#endif

	Handle<Context>		context = current_runtime->localContext();
	Context::Scope		context_scope(context);

	if (TRIGGER_FIRED_FOR_ROW(event))
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);

		if (TRIGGER_FIRED_BY_INSERT(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_trigtuple);
			// OLD
			args[1] = Undefined(xenv->isolate);
		}
		else if (TRIGGER_FIRED_BY_DELETE(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = Undefined(xenv->isolate);
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
		args[0] = args[1] = Undefined(xenv->isolate);
	}

	// 2: TG_NAME
	args[2] = ToString(trig->tg_trigger->tgname);

	// 3: TG_WHEN
	if (TRIGGER_FIRED_BEFORE(event))
		args[3] = String::NewFromUtf8Literal(xenv->isolate, "BEFORE");
	else
		args[3] = String::NewFromUtf8Literal(xenv->isolate, "AFTER");

	// 4: TG_LEVEL
	if (TRIGGER_FIRED_FOR_ROW(event))
		args[4] = String::NewFromUtf8Literal(xenv->isolate, "ROW");
	else
		args[4] = String::NewFromUtf8Literal(xenv->isolate, "STATEMENT");

	// 5: TG_OP
	if (TRIGGER_FIRED_BY_INSERT(event))
		args[5] = String::NewFromUtf8Literal(xenv->isolate, "INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(event))
		args[5] = String::NewFromUtf8Literal(xenv->isolate, "DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(event))
		args[5] = String::NewFromUtf8Literal(xenv->isolate, "UPDATE");
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(event))
		args[5] = String::NewFromUtf8Literal(xenv->isolate, "TRUNCATE");
#endif
	else
		args[5] = String::NewFromUtf8Literal(xenv->isolate, "?");

	// 6: TG_RELID
	args[6] = Uint32::New(xenv->isolate, RelationGetRelid(rel));

	// 7: TG_TABLE_NAME
	args[7] = ToString(RelationGetRelationName(rel));

	// 8: TG_TABLE_SCHEMA
	args[8] = ToString(get_namespace_name(RelationGetNamespace(rel)));

	// 9: TG_ARGV
	Handle<Array> tgargs = Array::New(xenv->isolate, trig->tg_trigger->tgnargs);
	for (int i = 0; i < trig->tg_trigger->tgnargs; i++)
		tgargs->Set(context, i, ToString(trig->tg_trigger->tgargs[i])).Check();
	args[9] = tgargs;

	TryCatch			try_catch(xenv->isolate);
	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn =
		Local<Function>::Cast(recv->GetInternalField(0));
	Handle<v8::Value> newtup =
		DoCall(context, fn, recv, lengthof(args), args, nonatomic);

	if (newtup.IsEmpty())
		throw js_error(try_catch);

	/*
	 * If the function specifically returned null, return NULL to
	 * tell executor to skip the operation.  Otherwise, the function
	 * result is the tuple to be returned.
	 */
	if (newtup->IsNull() || !TRIGGER_FIRED_FOR_ROW(event))
	{
		result = PointerGetDatum(NULL);
	}
	else if (!newtup->IsUndefined())
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);
		HeapTupleHeader	header;

		header = DatumGetHeapTupleHeader(conv.ToDatum(newtup));

		/* We know it's there; heap_form_tuple stores with this layout. */
		result = PointerGetDatum((char *) header - HEAPTUPLESIZE);
	}

	return result;
}

static Datum
common_pl_call_validator(PG_FUNCTION_ARGS, Dialect dialect) throw()
{
	Oid				fn_oid = PG_GETARG_OID(0);
	HeapTuple		tuple;
	Form_pg_proc	proc;
	char			functyptype;
	bool			is_trigger = false;

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, fn_oid))
		PG_RETURN_VOID();

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, INTERNAL, VOID or polymorphic types */
	if (functyptype == TYPTYPE_PSEUDO)
	{
#if PG_VERSION_NUM >= 130000
		if (proc->prorettype == TRIGGEROID)
#else
		/* we assume OPAQUE with no arguments means a trigger */
		if (proc->prorettype == TRIGGEROID ||
				(proc->prorettype == OPAQUEOID && proc->pronargs == 0))
#endif
			is_trigger = true;
		else if (proc->prorettype != RECORDOID &&
			proc->prorettype != VOIDOID &&
			proc->prorettype != INTERNALOID &&
			!IsPolymorphicType(proc->prorettype))
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/v8 functions cannot return type %s",
						format_type_be(proc->prorettype))));
	}

	ReleaseSysCache(tuple);

	try
	{
		current_runtime = GetPlv8Runtime();
		Isolate::Scope  scope(current_runtime->isolate);
#ifdef ENABLE_DEBUGGER_SUPPORT
		Locker				lock;
#endif  // ENABLE_DEBUGGER_SUPPORT
		/* Don't use validator's fcinfo */
		plv8_proc	   *proc = Compile(fn_oid, NULL,
									   true, is_trigger, dialect);
		(void) CreateExecEnv(proc->cache->function, current_runtime);
		/* the result of a validator is ignored */
		PG_RETURN_VOID();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

Datum
plv8_call_validator(PG_FUNCTION_ARGS)
{
	return common_pl_call_validator(fcinfo, PLV8_DIALECT_NONE);
}

Datum
plcoffee_call_validator(PG_FUNCTION_ARGS)
{
	return common_pl_call_validator(fcinfo, PLV8_DIALECT_COFFEE);
}

Datum
plls_call_validator(PG_FUNCTION_ARGS)
{
	return common_pl_call_validator(fcinfo, PLV8_DIALECT_LIVESCRIPT);
}

static plv8_proc *
plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, char ***argnames) throw()
{
	HeapTuple			procTup;
	plv8_proc_cache	   *cache;
	bool				found;
	bool				isnull;
	Datum				prosrc;
	Oid				   *argtypes;
	char			   *argmodes;
	MemoryContext		oldcontext;
	plv8_proc_key		hkey = {};

	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	hkey.fn_oid = fn_oid;
	hkey.user_id = GetUserId();
	if (plv8_user_context != nullptr && plv8_user_context[0] != '\0') {
		strlcpy(hkey.user_ctx, plv8_user_context, NAMEDATALEN);
	}
	cache = (plv8_proc_cache *)
		hash_search(plv8_proc_cache_hash, &hkey, HASH_ENTER, &found);

	if (found)
	{
		bool	uptodate;

		/*
		 * We need to check user id and dispose it if it's different from
		 * the previous cache user id, as the V8 function is associated
		 * with the context where it was generated.  In most cases,
		 * we can expect this doesn't affect runtime performance.
		 */
		uptodate = (!cache->function.IsEmpty() &&
			cache->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			ItemPointerEquals(&cache->fn_tid, &procTup->t_self));

		if (!uptodate)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Reset();
		}
		else
		{
			ReleaseSysCache(procTup);
		}
	}
	else
	{
		new(&cache->function) Persistent<Function>();
		cache->prosrc = NULL;
	}

	if (cache->function.IsEmpty())
	{
		Form_pg_proc	procStruct;

		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		prosrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		cache->retset = procStruct->proretset;
		cache->rettype = procStruct->prorettype;

		strlcpy(cache->proname, NameStr(procStruct->proname), NAMEDATALEN);
		cache->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		cache->fn_tid = procTup->t_self;

		int nargs = get_func_arg_info(procTup, &argtypes, argnames, &argmodes);

		if (validate)
		{
			/*
			 * Disallow non-polymorphic pseudotypes in arguments
			 * (either IN or OUT).  Internal type is used to declare
			 * js functions for find_function().
			 */
			for (int i = 0; i < nargs; i++)
			{
				if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO &&
						argtypes[i] != INTERNALOID &&
						!IsPolymorphicType(argtypes[i]))
					ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/v8 functions cannot accept type %s",
								format_type_be(argtypes[i]))));
			}
		}

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		cache->prosrc = TextDatumGetCString(prosrc);
		MemoryContextSwitchTo(oldcontext);

		ReleaseSysCache(procTup);

		int	inargs = 0;
		for (int i = 0; i < nargs; i++)
		{
			Oid		argtype = argtypes[i];
			char	argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			switch (argmode)
			{
			case PROARGMODE_IN:
			case PROARGMODE_INOUT:
			case PROARGMODE_VARIADIC:
				break;
			default:
				continue;
			}

			if (*argnames)
				(*argnames)[inargs] = (*argnames)[i];
			cache->argtypes[inargs] = argtype;
			inargs++;
		}
		cache->nargs = inargs;
	}

	MemoryContext mcxt = CurrentMemoryContext;
	if (fcinfo)
		mcxt = fcinfo->flinfo->fn_mcxt;

	plv8_proc *proc = (plv8_proc *) MemoryContextAllocZero(mcxt,
		offsetof(plv8_proc, argtypes) + sizeof(plv8_type) * cache->nargs);

	proc->cache = cache;
	for (int i = 0; i < cache->nargs; i++)
	{
		Oid		argtype = cache->argtypes[i];
		/* Resolve polymorphic types, if this is an actual call context. */
		if (fcinfo && IsPolymorphicType(argtype))
			argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		plv8_fill_type(&proc->argtypes[i], argtype, mcxt);
	}

	Oid		rettype = cache->rettype;
	/* Resolve polymorphic return type if this is an actual call context. */
	if (fcinfo && IsPolymorphicType(rettype))
		rettype = get_fn_expr_rettype(fcinfo->flinfo);
	plv8_fill_type(&proc->rettype, rettype, mcxt);

	return proc;
}

static plv8_exec_env *
CreateExecEnv(Persistent<Function>& function, plv8_runtime *runtime)
{
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope(runtime->isolate);

	PG_TRY();
	{
		xenv = plv8_new_exec_env(runtime->isolate);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Local<Context>		ctx = current_runtime->localContext();
	Context::Scope		scope(ctx);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(runtime->isolate, runtime->recv_templ);
	Local<Object> obj = templ->NewInstance(ctx).ToLocalChecked();
	Local<Function> f = Local<Function>::New(runtime->isolate, function);
	obj->SetInternalField(0, f);
	xenv->recv.Reset(runtime->isolate, obj);

	return xenv;
}

static plv8_exec_env *
CreateExecEnv(Handle<Function> function, plv8_runtime *runtime)
{
	Isolate::Scope	    iscope(runtime->isolate);
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope(runtime->isolate);

	PG_TRY();
	{
		xenv = plv8_new_exec_env(runtime->isolate);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	Local<Context>		ctx = current_runtime->localContext();
	Context::Scope		scope(ctx);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(runtime->isolate, runtime->recv_templ);
	Local<Object> obj = templ->NewInstance(ctx).ToLocalChecked();
	Local<Function> f = Local<Function>::New(runtime->isolate, function);
	obj->SetInternalField(0, f);
	xenv->recv.Reset(runtime->isolate, obj);

	return xenv;
}

/* Source transformation from a dialect (coffee or ls) to js */
static char *
CompileDialect(const char *src, Dialect dialect, plv8_runtime *runtime)
{
	Isolate		   *isolate = Isolate::GetCurrent();
	HandleScope		handle_scope(isolate);
	Local<Context>	ctx = Local<Context>::New(isolate, runtime->compile_context);
	Context::Scope	context_scope(ctx);
	TryCatch		try_catch(isolate);
	Local<String>	key;
	char		   *cresult;
	const char	   *dialect_binary_data;

	CheckTermination(isolate);

	switch (dialect)
	{
		case PLV8_DIALECT_COFFEE:
			if (coffee_script_binary_data[0] == '\0')
				throw js_error("CoffeeScript is not enabled");
			key = String::NewFromUtf8Literal(isolate, "CoffeeScript", NewStringType::kInternalized);
			dialect_binary_data = (const char *) coffee_script_binary_data;
			break;
		case PLV8_DIALECT_LIVESCRIPT:
			if (livescript_binary_data[0] == '\0')
				throw js_error("LiveScript is not enabled");
			key = String::NewFromUtf8Literal(isolate, "LiveScript", NewStringType::kInternalized);
			dialect_binary_data = (const char *) livescript_binary_data;
			break;
		default:
			throw js_error("Unknown Dialect");
	}

	if (ctx->Global()->Get(ctx, key).ToLocalChecked()->IsUndefined())
	{
		v8::ScriptOrigin origin(key);
		v8::Local<v8::Script> script;
		if (!Script::Compile(isolate->GetCurrentContext(), ToString(dialect_binary_data), &origin).ToLocal(&script))
			throw js_error(try_catch);
		if (script.IsEmpty())
			throw js_error(try_catch);
		v8::Local<v8::Value> result;
		if (!script->Run(isolate->GetCurrentContext()).ToLocal(&result))
			throw js_error(try_catch);
		if (result.IsEmpty()) {
			if (CheckTermination(isolate))
			{
				if (current_runtime->interrupted)
				{
					current_runtime->interrupted = false;
					throw js_error("Signal caught: interrupted");
				}
				throw js_error("Script is out of memory");
			}
			throw js_error(try_catch);
		}
	}

	Local<Object> compiler = Local<Object>::Cast(ctx->Global()->Get(ctx, key).ToLocalChecked());
    Local<Function>	func = Local<Function>::Cast(
            compiler->Get(ctx, String::NewFromUtf8Literal(isolate, "compile",
												   NewStringType::kInternalized)).ToLocalChecked());
	const int		nargs = 1;
	Handle<v8::Value>	args[nargs];

	args[0] = ToString(src);
	MaybeLocal<v8::Value>	value = func->Call(ctx, compiler, nargs, args);

	if (value.IsEmpty()) {
		if (CheckTermination(isolate))
		{
			if (current_runtime->interrupted)
			{
				current_runtime->interrupted = false;
				throw js_error("Signal caught: interrupted");
			}
			throw js_error("Script is out of memory");
		}
		throw js_error(try_catch);
	}
	CString		result(value.ToLocalChecked());

	PG_TRY();
	{
		MemoryContext	oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		cresult = pstrdup(result.str());
		MemoryContextSwitchTo(oldcontext);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return cresult;
}

/*
 * fcinfo should be passed if this is an actual function call context, where
 * we can resolve polymorphic types and use function's memory context.
 */
static plv8_proc *
Compile(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, bool is_trigger,
		Dialect dialect)
{
	plv8_proc  *proc;
	char	  **argnames;

	PG_TRY();
	{
		proc = plv8_get_proc(fn_oid, fcinfo, validate, &argnames);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	plv8_proc_cache *cache = proc->cache;

	if (cache->function.IsEmpty())
	{
		/*
		 * We need to create global context before entering CompileFunction
		 * because GetPlv8Context could call startup procedure, which
		 * could be this cache->function itself.  In this scenario,
		 * Compile is called recursively and plv8_get_proc tries to refresh
		 * cache because cache->function is still not yet ready at this
		 * point.  Then some pointers of cache will become stale by pfree
		 * and CompileFunction ends up compiling freed function source.
		 */
		current_runtime = GetPlv8Runtime();
		Isolate::Scope	scope(current_runtime->isolate);
		HandleScope		handle_scope(current_runtime->isolate);
		cache->function.Reset(current_runtime->isolate, CompileFunction(
						current_runtime,
						cache->proname,
						cache->nargs,
						(const char **) argnames,
						cache->prosrc,
						is_trigger,
						cache->retset,
						dialect));
	}

	return proc;
}

static Local<Function>
CompileFunction(
	plv8_runtime *runtime,
	const char *proname,
	int proarglen,
	const char *proargs[],
	const char *prosrc,
	bool is_trigger,
	bool retset,
	Dialect dialect)
{
	Isolate					   *isolate = Isolate::GetCurrent();
	EscapableHandleScope		handle_scope(isolate);
	StringInfoData	src;

	initStringInfo(&src);

	if (dialect != PLV8_DIALECT_NONE)
		prosrc = CompileDialect(prosrc, dialect, runtime);
	/*
	 *  (function (<arg1, ...>){
	 *    <prosrc>
	 *  })
	 */
	appendStringInfo(&src, "(function (");
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
	if (dialect)
		appendStringInfo(&src, "){\nreturn %s\n})", prosrc);
	else
		appendStringInfo(&src, "){\n%s\n})", prosrc);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined(isolate);
	Local<String> source = ToString(src.data, src.len);
	pfree(src.data);

	Local<Context>	context = runtime->localContext();
	Context::Scope	context_scope(context);
	TryCatch		try_catch(isolate);
	v8::ScriptOrigin origin(name);

	// set up the signal handlers
	int_handler = (void *) signal(SIGINT, signal_handler);
	term_handler = (void *) signal(SIGTERM, signal_handler);

#ifdef EXECUTION_TIMEOUT
	bool timeout = false;
#ifdef _MSC_VER // windows
	HANDLE  hThread;
	DWORD dwThreadId;
	hThread = CreateThread(NULL, 0, Breakout, isolate, 0, &dwThreadId);
#else
	pthread_t breakout_thread;
	// set up the thread to break out the execution if needed
	pthread_create(&breakout_thread, NULL, Breakout, isolate);
#endif
#endif

	v8::Local<v8::Script> script;
	v8::Local<v8::Value> result;
	if (Script::Compile(context, source, &origin).ToLocal(&script)) {
		if (!script.IsEmpty()) {
			if (!script->Run(context).ToLocal(&result))
				throw js_error(try_catch);
		}
	}

#ifdef EXECUTION_TIMEOUT
#ifdef _MSC_VER
	if (TerminateThread(hThread, NULL) == 0) {
		timeout = true;
	}
#else
	void *thread_result;
	pthread_cancel(breakout_thread);
	pthread_join(breakout_thread, &thread_result);
	if (thread_result == NULL) {
		timeout = true;
	}
#endif
#endif
	signal(SIGINT, (void (*)(int)) int_handler);
	signal(SIGTERM, (void (*)(int)) term_handler);

	if (result.IsEmpty()) {
		if (CheckTermination(isolate))
		{
			if (current_runtime->interrupted) {
				current_runtime->interrupted = false;
				throw js_error("Signal caught: interrupted");
			}
#ifdef EXECUTION_TIMEOUT
			if (timeout)
				throw js_error("compiler timeout exceeded");
#endif
			throw js_error("Script is out of memory");
		}
		throw js_error(try_catch);
	}

	return handle_scope.Escape(Local<Function>::Cast(result));
}

Local<Function>
find_js_function(Oid fn_oid)
{
	HeapTuple		tuple;
	Form_pg_proc	pg_proc;
	Oid				prolang;
	NameData		langnames[] = { {"plv8"}, {"plcoffee"}, {"plls"} };
	int				langno;
	int				langlen = sizeof(langnames) / sizeof(NameData);
	Local<Function> func;
	Isolate			*isolate = Isolate::GetCurrent();

	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	pg_proc = (Form_pg_proc) GETSTRUCT(tuple);
	prolang = pg_proc->prolang;
	ReleaseSysCache(tuple);

	/* Should not happen? */
	if (!OidIsValid(prolang))
		return func;

	/* See if the function language is a compatible one */
	for (langno = 0; langno < langlen; langno++)
	{
		tuple = SearchSysCache(LANGNAME, NameGetDatum(&langnames[langno]), 0, 0, 0);
		if (HeapTupleIsValid(tuple))
		{
#if PG_VERSION_NUM < 120000
			Oid langtupoid = HeapTupleGetOid(tuple);
#else
			Form_pg_database datForm = (Form_pg_database) GETSTRUCT(tuple);
			Oid langtupoid = datForm->oid;
#endif
			ReleaseSysCache(tuple);
			if (langtupoid == prolang)
				break;
		}
	}

	/* Not found or non-JS function */
	if (langno >= langlen)
		return func;

	try
	{
		plv8_proc		   *proc = Compile(fn_oid, NULL,
										   true, false,
										   (Dialect) (PLV8_DIALECT_NONE + langno));

		TryCatch			try_catch(isolate);

		func = Local<Function>::New(isolate, proc->cache->function);
	}
	catch (js_error& e) { e.rethrow(); }
	catch (pg_error& e) { e.rethrow(); }

	return func;
}

/*
 * NOTICE: the returned buffer could be an internal static buffer.
 */
const char *
FormatSPIStatus(int status) throw()
{
	static char	private_buf[1024];

	if (status > 0)
		return "OK";

	switch (status)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
		case SPI_ERROR_TRANSACTION:
			return "current transaction is aborted, "
				   "commands ignored until end of transaction block";
		case SPI_ERROR_CURSOR:
			return "SPI_ERROR_CURSOR";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		default:
			snprintf(private_buf, sizeof(private_buf),
				"SPI_ERROR: %d", status);
			return private_buf;
	}
}

static text *
charToText(char *string)
{
	int len = strlen(string);
	text *result = (text *) palloc(len + 1 + VARHDRSZ);

	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), string, len + 1);

	return result;
}

static void CreateDefaultContext(plv8_runtime *runtime) {
	Isolate 			   *isolate = runtime->isolate;
	Isolate::Scope			scope(isolate);
	HandleScope				handle_scope(isolate);

	Local<Context> newContext = Context::New(isolate, NULL, GetGlobalObjectTemplate(runtime));
	if (plv8_max_eval_size >= 0)
		newContext->AllowCodeGenerationFromStrings(false);
	runtime->default_context.Reset(isolate, newContext);
}

static void RunStartProc(plv8_runtime *runtime) {
	const char *start_proc = (plv8_boot_proc != nullptr && plv8_boot_proc[0] != '\0') ? plv8_boot_proc : plv8_start_proc;
	if (start_proc == nullptr || start_proc[0] == '\0')
		return; // bail out if no start_proc or boot_proc exists

	Local<Function>		func;

	Isolate				*isolate = runtime->isolate;
	HandleScope			handle_scope(isolate);
	Local<Context>		context = runtime->localContext();
	Context::Scope		context_scope(context);
	TryCatch			try_catch(isolate);
	MemoryContext		ctx = CurrentMemoryContext;
	text *arg;
#if PG_VERSION_NUM < 120000
	FunctionCallInfoData fake_fcinfo;
#else
	// Stack-allocate FunctionCallInfoBaseData with
		// space for 2 arguments:
		LOCAL_FCINFO(fake_fcinfo, 2);
#endif
	FmgrInfo	flinfo;

	char perm[16];
	strcpy(perm, "EXECUTE");
	arg = charToText(perm);

	PG_TRY();
			{
				Oid funcoid = DatumGetObjectId(DirectFunctionCall1(regprocin, CStringGetDatum(start_proc)));
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
					elog(WARNING, "failed to find js function %s", start_proc);
				} else {
					if (DatumGetBool(ret)) {
						func = find_js_function(funcoid);
					} else {
						elog(WARNING, "no permission to execute js function %s", start_proc);
					}
				}
			}
		PG_CATCH();
			{
				ErrorData	   *edata;

				MemoryContextSwitchTo(ctx);
				edata = CopyErrorData();
				elog(WARNING, "failed to find js function %s", edata->message);
				FlushErrorState();
				FreeErrorData(edata);
			}
	PG_END_TRY();

	pfree(arg);

	if (!func.IsEmpty())
	{
		Handle<v8::Value>	result =
				DoCall(context, func, context->Global(), 0, NULL, false);
		if (result.IsEmpty())
			throw js_error(try_catch);
	}
}

static plv8_runtime*
GetPlv8Runtime() {
	Oid					user_id = GetUserId();
	plv8_runtime		*runtime = nullptr;

	// shortcut: use current_runtime if it's the same user and not dead
	if (current_runtime != nullptr && current_runtime->user_id == user_id && !current_runtime->wasKilled())
		runtime = current_runtime;
	else
	{
		unsigned int	i;
		for (i = 0; i < RuntimeCache.size(); i++)
		{
			if (RuntimeCache[i]->user_id == user_id)
			{
				runtime = RuntimeCache[i];
				break;
			}
		}
		if (runtime != nullptr && runtime->wasKilled())
		{
			// the isolate is dead because of OOM, kill it and dispose
			char *username = GetUserNameFromId(runtime->user_id, false);
			elog(LOG_SERVER_ONLY, "Disposing of a dead isolate for: %s", username);
			RuntimeCache.erase(RuntimeCache.begin() + i);
			if (runtime->isolate && runtime->isolate->IsInUse())
				runtime->isolate->Exit();
			if (current_runtime == runtime)
				current_runtime = nullptr;
			ClearProcCache(runtime);
			KillRuntime(runtime);
			pfree(runtime);
			runtime = nullptr;
		}
		if (runtime == nullptr) // need to create a new runtime
		{
			runtime = (plv8_runtime *) MemoryContextAlloc(TopMemoryContext,
														  sizeof(plv8_runtime));
			runtime->is_dead = false;
			runtime->interrupted = false;
			runtime->user_id = user_id;
			CreateIsolate(runtime);
			Isolate 			   *isolate = runtime->isolate;
			Isolate::Scope			scope(isolate);
			HandleScope				handle_scope(isolate);

			new(&runtime->recv_templ) Persistent<ObjectTemplate>();
			Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
			templ->SetInternalFieldCount(1);
			runtime->recv_templ.Reset(isolate, templ);

			new(&runtime->default_context) Persistent<Context>();

			new(&runtime->compile_context) Persistent<Context>();
			Local<Context> compile_context = Context::New(isolate, (ExtensionConfiguration*)NULL);
			runtime->compile_context.Reset(isolate, compile_context);

			new(&runtime->global_template) Persistent<ObjectTemplate>();

			auto toStringAttr = static_cast<PropertyAttribute>(v8::ReadOnly | v8::DontEnum);
			auto toStringSymbol = v8::Symbol::GetToStringTag(isolate);

			new(&runtime->plan_template) Persistent<ObjectTemplate>();
			Local<FunctionTemplate> base = FunctionTemplate::New(isolate);
			Local<String> planClassName = String::NewFromUtf8Literal(isolate, "PreparedPlan",
																	 NewStringType::kInternalized);
			base->SetClassName(planClassName);
			base->PrototypeTemplate()->Set(toStringSymbol, planClassName, toStringAttr);
			templ = base->InstanceTemplate();
			SetupPrepFunctions(templ);
			runtime->plan_template.Reset(isolate, templ);

			new(&runtime->cursor_template) Persistent<ObjectTemplate>();
			base = FunctionTemplate::New(isolate);
			Local<String> cursorClassName = String::NewFromUtf8Literal(isolate, "Cursor",
																	   NewStringType::kInternalized);
			base->SetClassName(cursorClassName);
			base->PrototypeTemplate()->Set(toStringSymbol, cursorClassName, toStringAttr);
			templ = base->InstanceTemplate();
			SetupCursorFunctions(templ);
			runtime->cursor_template.Reset(isolate, templ);

			new(&runtime->window_template) Persistent<ObjectTemplate>();
			base = FunctionTemplate::New(isolate);
			Local<String> windowClassName = String::NewFromUtf8Literal(isolate, "WindowObject",
																	   NewStringType::kInternalized);
			base->SetClassName(windowClassName);
			base->PrototypeTemplate()->Set(toStringSymbol, windowClassName, toStringAttr);
			templ = base->InstanceTemplate();
			SetupWindowFunctions(templ);
			runtime->window_template.Reset(isolate, templ);

			new(&runtime->ctx_queue) std::list<std::tuple<std::string, v8::Global<v8::Context>>>();
			new(&runtime->ctx_map) std::unordered_map<std::string, std::list<std::tuple<std::string,
					v8::Global<v8::Context>>>::iterator>();

			/*
			 * Need to register it before running any code, as the code
			 * recursively may want to get the runtime.
			 */
			RuntimeCache.push_back(runtime);

#ifdef ENABLE_DEBUGGER_SUPPORT
			debug_message_context = v8::Persistent<v8::Context>::New(global_context);

			v8::Locker locker;

			v8::Debug::SetDebugMessageDispatchHandler(DispatchDebugMessages, true);

			v8::Debug::EnableAgent("plv8", plv8_debugger_port, false);
#endif  // ENABLE_DEBUGGER_SUPPORT
		}
	}
	if (plv8_user_context == nullptr || plv8_user_context[0] == '\0')
	{
		if (runtime->default_context.IsEmpty())
		{
			Isolate::Scope scope(runtime->isolate);
			HandleScope handle_scope(runtime->isolate);
			CreateDefaultContext(runtime);
			RunStartProc(runtime);
		}
	}
	else
		runtime->touchContext(plv8_user_context);
	return runtime;
}

static Local<ObjectTemplate>
GetGlobalObjectTemplate(plv8_runtime *runtime)
{
	Isolate	*isolate = runtime->isolate;
	auto &global = runtime->global_template;

	if (global.IsEmpty())
	{
		HandleScope				handle_scope(isolate);

		Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
		// ERROR levels for elog
		templ->Set(String::NewFromUtf8Literal(isolate, "DEBUG5", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG5));
		templ->Set(String::NewFromUtf8Literal(isolate, "DEBUG4", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG4));
		templ->Set(String::NewFromUtf8Literal(isolate, "DEBUG3", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG3));
		templ->Set(String::NewFromUtf8Literal(isolate, "DEBUG2", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG2));
		templ->Set(String::NewFromUtf8Literal(isolate, "DEBUG1", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG1));
		templ->Set(String::NewFromUtf8Literal(isolate, "DEBUG", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG5));
		templ->Set(String::NewFromUtf8Literal(isolate, "LOG", NewStringType::kInternalized),
				   Int32::New(isolate, LOG));
		templ->Set(String::NewFromUtf8Literal(isolate, "INFO", NewStringType::kInternalized),
				   Int32::New(isolate, INFO));
		templ->Set(String::NewFromUtf8Literal(isolate, "NOTICE", NewStringType::kInternalized),
				   Int32::New(isolate, NOTICE));
		templ->Set(String::NewFromUtf8Literal(isolate, "WARNING", NewStringType::kInternalized),
				   Int32::New(isolate, WARNING));
		templ->Set(String::NewFromUtf8Literal(isolate, "ERROR", NewStringType::kInternalized),
				   Int32::New(isolate, ERROR));
		global.Reset(isolate, templ);

		Local<ObjectTemplate> plv8 = ObjectTemplate::New(isolate);

		SetupPlv8Functions(plv8);
		plv8->Set(String::NewFromUtf8Literal(isolate, "version", NewStringType::kInternalized),
				  String::NewFromUtf8Literal(isolate, PLV8_VERSION));
		plv8->Set(String::NewFromUtf8Literal(isolate, "v8_version", NewStringType::kInternalized),
				  String::NewFromUtf8Literal(isolate, V8_VERSION_STRING));

		templ->Set(String::NewFromUtf8Literal(isolate, "plv8", NewStringType::kInternalized), plv8);
	}
	return Local<ObjectTemplate>::New(isolate, global);
}

static bool CheckTermination (Isolate *isolate) {
	bool wasTerminating = isolate->IsExecutionTerminating();
	if (wasTerminating)
		// it just means we got control back and are ready to send new code to the isolate
		isolate->CancelTerminateExecution();
	return wasTerminating;
}

/*
 * Accessor to plv8_type stored in fcinfo.
 */
plv8_type *
get_plv8_type(PG_FUNCTION_ARGS, int argno)
{
	plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	return &proc->argtypes[argno];
}

Converter::Converter(TupleDesc tupdesc) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts),
	m_is_scalar(false),
	m_memcontext(NULL)
{
	Init();
}

Converter::Converter(TupleDesc tupdesc, bool is_scalar) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts),
	m_is_scalar(is_scalar),
	m_memcontext(NULL)
{
	Init();
}

Converter::~Converter()
{
	if (m_memcontext != NULL)
	{
		MemoryContext ctx = CurrentMemoryContext;

		PG_TRY();
		{
			MemoryContextDelete(m_memcontext);
		}
		PG_CATCH();
		{
			ErrorData	   *edata;

			MemoryContextSwitchTo(ctx);
			// don't throw out from deconstructor
			edata = CopyErrorData();
			elog(WARNING, "~Converter: %s", edata->message);
			FlushErrorState();
			FreeErrorData(edata);
		}
		PG_END_TRY();
		m_memcontext = NULL;
	}
}

void
Converter::Init()
{
	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
			continue;

		m_colnames[c] = ToString(NameStr(TupleDescAttr(m_tupdesc, c)->attname));

		PG_TRY();
		{
			if (m_memcontext == NULL)
#if PG_VERSION_NUM < 110000
				m_memcontext = AllocSetContextCreate(
									CurrentMemoryContext,
									"ConverterContext",
									ALLOCSET_SMALL_MINSIZE,
									ALLOCSET_SMALL_INITSIZE,
									ALLOCSET_SMALL_MAXSIZE);
			plv8_fill_type(&m_coltypes[c],
						   m_tupdesc->attrs[c]->atttypid,
						   m_memcontext);
#else
				m_memcontext = AllocSetContextCreate(
									CurrentMemoryContext,
									"ConverterContext",
									ALLOCSET_DEFAULT_SIZES);
			plv8_fill_type(&m_coltypes[c],
						   m_tupdesc->attrs[c].atttypid,
						   m_memcontext);
#endif
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
Local<Object>
Converter::ToValue(HeapTuple tuple)
{
	Isolate		   *isolate = Isolate::GetCurrent();
    Local<Context>  context = isolate->GetCurrentContext();
	Local<Object>	obj = Object::New(isolate);

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Datum		datum;
		bool		isnull;

		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
			continue;

#if PG_VERSION_NUM >= 90000
		datum = heap_getattr(tuple, c + 1, m_tupdesc, &isnull);
#else
		/*
		 * Due to the difference between C and C++ rules,
		 * we cannot call heap_getattr from < 9.0 unfortunately.
		 */
		datum = nocachegetattr(tuple, c + 1, m_tupdesc, &isnull);
#endif

		obj->Set(context, m_colnames[c], ::ToValue(datum, isnull, &m_coltypes[c])).Check();
	}

	return obj;
}

Datum
Converter::ToDatum(Handle<v8::Value> value, Tuplestorestate *tupstore)
{
	Isolate		   *isolate = Isolate::GetCurrent();
    Local<Context>  context = isolate->GetCurrentContext();
	Datum			result;
	TryCatch		try_catch(isolate);
	Handle<Object>	obj;

	if (!m_is_scalar)
	{
		if (!value->IsObject())
			throw js_error("argument must be an object");
		obj = Handle<Object>::Cast(value);
		if (obj.IsEmpty())
			throw js_error(try_catch);
	}

	/*
	 * Use vector<char> instead of vector<bool> because <bool> version is
	 * s specialized and different from bool[].
	 */
	Datum  *values = (Datum *) palloc(sizeof(Datum) * m_tupdesc->natts);
	bool   *nulls = (bool *) palloc(sizeof(bool) * m_tupdesc->natts);

	if (!m_is_scalar)
	{
		Handle<Array> names = obj->GetPropertyNames(isolate->GetCurrentContext()).ToLocalChecked();

		for (int c = 0; c < m_tupdesc->natts; c++)
		{
			if (TupleDescAttr(m_tupdesc, c)->attisdropped)
				continue;

			bool found = false;
			CString  colname(m_colnames[c]);
			for (int d = 0; d < m_tupdesc->natts; d++)
			{
				CString fname(names->Get(context, d).ToLocalChecked());
				if (strcmp(colname, fname) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found)
				throw js_error("field name / property name mismatch");
		}
	}

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		/* Make sure dropped columns are skipped by backend code. */
#if PG_VERSION_NUM < 110000
		if (m_tupdesc->attrs[c]->attisdropped)
#else
		if (m_tupdesc->attrs[c].attisdropped)
#endif
		{
			nulls[c] = true;
			continue;
		}

		Handle<v8::Value> attr = m_is_scalar ? value : obj->Get(context, m_colnames[c]).ToLocalChecked();
		if (attr.IsEmpty() || attr->IsUndefined() || attr->IsNull())
			nulls[c] = true;
		else
			values[c] = ::ToDatum(attr, &nulls[c], &m_coltypes[c]);
	}

	if (tupstore)
	{
		tuplestore_putvalues(tupstore, m_tupdesc, values, nulls);
		result = (Datum) 0;
	}
	else
	{
		result = HeapTupleGetDatum(heap_form_tuple(m_tupdesc, values, nulls));
	}

	pfree(values);
	pfree(nulls);

	return result;
}

js_error::js_error() noexcept
	: m_msg(nullptr), m_code(0), m_detail(nullptr), m_hint(nullptr), m_context(nullptr)
{
}

js_error::js_error(const char *msg) noexcept : js_error()
{
	m_msg = pstrdup(msg);
}

js_error::js_error(Isolate *isolate, v8::Local<v8::Value> exception, v8::Local<v8::Message> message) noexcept : js_error() {
	init(isolate, exception, message);
}

js_error::js_error(v8::TryCatch &try_catch) noexcept : js_error() {
	Isolate		   		*isolate = Isolate::GetCurrent();
	HandleScope			handle_scope(isolate);

	init(isolate, try_catch.Exception(), try_catch.Message());
}

void
js_error::init(Isolate *isolate, v8::Local<v8::Value> exception, v8::Local<Message> message) noexcept
{
	HandleScope			handle_scope(isolate);
	String::Utf8Value	err_message(isolate, exception);
	Local<Context>      context = isolate->GetCurrentContext();

	try
	{
		m_msg = ToCStringCopy(err_message);
        StringInfoData	detailStr;
        StringInfoData	hintStr;
        StringInfoData	contextStr;
        initStringInfo(&detailStr);
        initStringInfo(&hintStr);
        initStringInfo(&contextStr);
		Handle<v8::Object> err;
		if (exception->ToObject(context).ToLocal(&err))
        {
            if (!err.IsEmpty())
            {
                v8::Local<v8::Value> errCode;
                if (err->Get(context,
                             String::NewFromUtf8Literal(isolate, "code")).ToLocal(&errCode))
                {
                    if (!errCode->IsUndefined() && !errCode->IsNull())
                    {
                        int32_t code = errCode->Int32Value(context).FromJust();
                        m_code = code;
                    }
                }

                v8::Local<v8::Value> errDetail;
                if (err->Get(context,
                             String::NewFromUtf8Literal(isolate, "detail")).ToLocal(&errDetail))
                {
                    if (!errDetail->IsUndefined() && !errDetail->IsNull())
                    {
                        CString detail(errDetail);
                        appendStringInfo(&detailStr, "%s", detail.str("?"));
                        m_detail = detailStr.data;
                    }
                }

                v8::Local<v8::Value> errHint;
                if (err->Get(context,
                             String::NewFromUtf8Literal(isolate, "hint")).ToLocal(&errHint))
                {
                    if (!errHint->IsUndefined() && !errHint->IsNull())
                    {
                        CString hint(errHint);
                        appendStringInfo(&hintStr, "%s", hint.str("?"));
                        m_hint = hintStr.data;
                    }
                }

                v8::Local<v8::Value> errContext;
                if (err->Get(context, String::NewFromUtf8Literal(isolate, "context")).ToLocal(&errContext))
                    if (!errContext->IsUndefined() && !errContext->IsNull())
                    {
                        CString str_context(errContext);
                        appendStringInfo(&contextStr, "%s\n", str_context.str("?"));
                    }
            }
        }

		if (!message.IsEmpty())
		{
			CString		script(message->GetScriptResourceName());
			int		lineno = message->GetLineNumber(context).FromJust();
			CString		source(message->GetSourceLine(context).ToLocalChecked());
			// TODO: Get stack trace?
			//Handle<StackTrace> stackTrace(message->GetStackTrace());

			/*
			 * Report lineno - 1 because "function _(...){" was added
			 * at the first line to the javascript code.
			 */
			if (strstr(m_msg, "Error: ") == m_msg)
				m_msg += 7;

			appendStringInfo(&contextStr, "%s() LINE %d: %s",
				script.str("?"), lineno - 1, source.str("?"));
		}

		m_context = contextStr.data;
	}
	catch (...)
	{
		// nested error, keep quiet.
	}
}

Local<v8::Value>
js_error::error_object()
{
	char *msg = pstrdup(m_msg ? m_msg : "unknown exception");
	/*
	 * Trim leading "Error: ", in case the message is generated from
	 * another Error.
	 */
	if (strstr(msg, "Error: ") == msg)
		msg += 7;
	Local<String> message = ToString(msg);
	return Exception::Error(message);
}

void
js_error::log(int elevel, const char *msg_format) noexcept {
	if (elevel >= ERROR) {
		return rethrow(msg_format);
	}
	ereport(elevel,
			(
					m_code ? errcode(m_code): 0,
					m_msg ? errmsg((msg_format ? msg_format : "%s"), m_msg) : 0,
					m_detail ? errdetail("%s", m_detail) : 0,
					m_hint ? errhint("%s", m_hint) : 0,
					m_context ? errcontext("%s", m_context) : 0
			));
}

__attribute__((noreturn))
void
js_error::rethrow(const char *msg_format) noexcept
{
	ereport(ERROR,
			(
					m_code ? errcode(m_code): 0,
					m_msg ? errmsg((msg_format ? msg_format : "%s"), m_msg) : 0,
					m_detail ? errdetail("%s", m_detail) : 0,
					m_hint ? errhint("%s", m_hint) : 0,
					m_context ? errcontext("%s", m_context) : 0
			));
	exit(0);	// keep compiler quiet
}

__attribute__((noreturn))
void
pg_error::rethrow() throw()
{
	PG_RE_THROW();
	exit(0);	// keep compiler quiet
}

void plv8_runtime::touchContext(const char *context_id)
{
	if (ctx_map.find(context_id) == ctx_map.end())
	{
		Isolate::Scope			scope(isolate);
		HandleScope				handle_scope(isolate);
		if (!ctx_queue.empty() && ctx_queue.size() >= plv8_context_cache_size)
		{
			auto &key = std::get<0>(ctx_queue.back());
			ClearProcCache(this, key.c_str());
			disposeContext(ctx_queue.back());
			ctx_map.erase(key);
			ctx_queue.pop_back();
		}
		auto	newContext = Context::New(isolate, NULL, GetGlobalObjectTemplate(this));
		if (plv8_max_eval_size >= 0)
			newContext->AllowCodeGenerationFromStrings(false);
		ctx_queue.emplace_front(context_id, Global<Context>(isolate, newContext));
		ctx_map[context_id] = ctx_queue.begin();
		RunStartProc(this);
	}
	else
	{
		auto it = ctx_map[context_id];
		if (it != ctx_queue.begin())
		{
			ctx_queue.splice(ctx_queue.begin(), ctx_queue, it);
			ctx_map[context_id] = ctx_queue.begin();
		}
	}
}

Local<Context> plv8_runtime::localContext() const
{
	if (plv8_user_context == nullptr || plv8_user_context[0] == '\0') {
		return default_context.Get(isolate);
	}
	return std::get<1>(ctx_queue.front()).Get(isolate);
}

void plv8_runtime::removeContext(const char *context_id)
{
	auto it = ctx_map.find(context_id);
	if (it != ctx_map.end())
	{
		ClearProcCache(this, context_id);
		disposeContext(*it->second);
		ctx_queue.erase(it->second);
		ctx_map.erase(it);
	}
}

void plv8_runtime::disposeContext (std::tuple<std::string, Global<Context>> &tuple) const
{
	Isolate::Scope			scope(isolate);
	HandleScope				handle_scope(isolate);
	Local<Context> 			context = std::get<1>(tuple).Get(isolate);
	std::get<1>(tuple).Reset();
	Context::Scope context_scope(context);
	isolate->ContextDisposedNotification();
	isolate->IdleNotificationDeadline(v8_platform->MonotonicallyIncreasingTime());
}
