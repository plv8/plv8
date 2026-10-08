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
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


extern "C" {
#define String PG_Node_String
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_database.h"
#include "catalog/pg_language.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM >= 190000
#include "utils/hsearch.h"
#endif

#include <signal.h>

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
	.name = "plv8",
	.version = PLV8_VERSION
);
#else
PG_MODULE_MAGIC;
#endif

PGDLLEXPORT Datum	plv8_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_call_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_reset(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_info(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_save_snapshot(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);
PG_FUNCTION_INFO_V1(plv8_reset);
PG_FUNCTION_INFO_V1(plv8_info);
PG_FUNCTION_INFO_V1(plv8_save_snapshot);


PGDLLEXPORT void _PG_init(void);

PGDLLEXPORT Datum	plv8_inline_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_inline_handler);
#undef String
} // extern "C"

using namespace v8;

static pthread_t main_pg_thread_id;
static bool main_pg_thread_initialized = false;

bool
is_main_pg_thread(void)
{
	if (!main_pg_thread_initialized)
		return true;
	return pthread_equal(pthread_self(), main_pg_thread_id) != 0;
}

void
plv8_assert_main_pg_thread(const char *api_name)
{
	if (!is_main_pg_thread())
	{
		fprintf(stderr, "FATAL: PL/v8 attempted to call PostgreSQL backend API (%s) from a non-main V8 worker thread!\n",
				api_name ? api_name : "unknown");
		abort();
	}
}

static void
refresh_main_pg_thread_after_fork(void)
{
	main_pg_thread_id = pthread_self();
	main_pg_thread_initialized = true;
}

/*
 * ScopedSignalBlockerForV8Workers temporarily masks asynchronous PostgreSQL
 * backend signals before V8 spawns any background platform or compiler threads.
 * Synchronous hardware fault/trap signals (SIGSEGV, SIGBUS, SIGFPE, SIGILL,
 * SIGTRAP, SIGSYS) are intentionally left unblocked so V8 trap handlers and
 * crash diagnostics continue to work normally.
 */
class ScopedSignalBlockerForV8Workers
{
private:
	sigset_t old_mask_;
	bool active_;

public:
	ScopedSignalBlockerForV8Workers() : active_(false)
	{
		sigset_t block_mask;
		sigemptyset(&block_mask);
		sigaddset(&block_mask, SIGHUP);
		sigaddset(&block_mask, SIGINT);
		sigaddset(&block_mask, SIGQUIT);
		sigaddset(&block_mask, SIGUSR1);
		sigaddset(&block_mask, SIGALRM);
		sigaddset(&block_mask, SIGTERM);
		sigaddset(&block_mask, SIGCHLD);
		if (pthread_sigmask(SIG_BLOCK, &block_mask, &old_mask_) == 0)
			active_ = true;
	}

	~ScopedSignalBlockerForV8Workers()
	{
		if (active_)
			pthread_sigmask(SIG_SETMASK, &old_mask_, nullptr);
	}
};

typedef struct plv8_proc_cache
{
	Oid						fn_oid;

	Persistent<Function>	function;
	char					proname[NAMEDATALEN];
	char				   *prosrc;

	TransactionId			fn_xmin;
	ItemPointerData			fn_tid;
	Oid						user_id;

	int						nhandlers;
	plv8_handler_dep		handlers[PLV8_MAX_LANG_HANDLER_DEPTH];

	int						nargs;
	bool					retset;		/* true if SRF */
	Oid						rettype;
	Oid						argtypes[FUNC_MAX_ARGS];
} plv8_proc_cache;

plv8_context *current_context = nullptr;
static v8::Isolate *current_isolate = nullptr;
static ArrayAllocator *current_allocator = nullptr;
static bool plv8_isolate_oom_killed = false;
bool plv8_pass_user_types_as_bytes = false;
static uint64 next_context_id = 1;
size_t plv8_memory_limit = 256;
size_t plv8_last_heap_size = 0;
static int plv8_wasm_cache_size = 64;
static int plv8_thread_pool_size = 0;
static char *plv8_boot_script_file = NULL;
static char *plv8_snapshot_file = NULL;
static int plv8_execution_timeout = 300;

#define PLV8_SNAPSHOT_MAGIC "PLV8SN01"
#define PLV8_SNAPSHOT_VERSION 1

struct Plv8SnapshotHeader
{
	char		magic[8];
	uint32		version;
	uint32		num_functions;
	uint64		blob_size;
};

struct Plv8SnapshotSlot
{
	Oid					fn_oid;
	TransactionId		fn_xmin;
	ItemPointerData		fn_tid;
	int					nhandlers;
	plv8_handler_dep	handlers[PLV8_MAX_LANG_HANDLER_DEPTH];
};

static v8::StartupData plv8_active_snapshot_blob = { nullptr, 0 };
static std::vector<Plv8SnapshotSlot> plv8_snapshot_slots;
static bool plv8_snapshot_is_mmap = false;
static void *plv8_snapshot_mmap_addr = nullptr;
static size_t plv8_snapshot_mmap_len = 0;
static uint64 plv8_snapshot_functions_restored = 0;
static char plv8_snapshot_source[256] = "none";
static bool plv8_bypass_snapshot_restore = false;

#define PLV8_WASM_KEY_MAX_LEN 128

struct Plv8SharedState
{
	LWLock			   *lock;
	int					dsa_tranche_id;
	int					dshash_tranche_id;
	dsa_handle			wasm_dsa_handle;
	dshash_table_handle	wasm_dshash_handle;
	pg_atomic_uint64	bytes_used;
	pg_atomic_uint64	entries_count;
	pg_atomic_uint64	hits;
	pg_atomic_uint64	misses;
	pg_atomic_uint64	stores;
};

struct Plv8WasmCacheEntry
{
	char				key[PLV8_WASM_KEY_MAX_LEN];
	dsa_pointer			code_ptr;
	uint32				code_size;
};

static Plv8SharedState *plv8_shared_state = nullptr;
static dsa_area *local_wasm_dsa = nullptr;
static dshash_table *local_wasm_dshash = nullptr;

static shmem_request_hook_type prev_shmem_request_hook = nullptr;
static shmem_startup_hook_type prev_shmem_startup_hook = nullptr;

static void
plv8_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(Plv8SharedState)));
	RequestNamedLWLockTranche("plv8_wasm_cache", 1);
}

static void
plv8_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	bool found = false;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	plv8_shared_state = static_cast<Plv8SharedState *>(
		ShmemInitStruct("plv8_shared_state", sizeof(Plv8SharedState), &found));
	if (!found)
	{
		memset(plv8_shared_state, 0, sizeof(Plv8SharedState));
		plv8_shared_state->lock = &(GetNamedLWLockTranche("plv8_wasm_cache"))[0].lock;
#if PG_VERSION_NUM >= 190000
		plv8_shared_state->dsa_tranche_id = LWLockNewTrancheId("plv8_wasm_dsa");
		plv8_shared_state->dshash_tranche_id = LWLockNewTrancheId("plv8_wasm_dshash");
#else
		plv8_shared_state->dsa_tranche_id = LWLockNewTrancheId();
		LWLockRegisterTranche(plv8_shared_state->dsa_tranche_id, "plv8_wasm_dsa");
		plv8_shared_state->dshash_tranche_id = LWLockNewTrancheId();
		LWLockRegisterTranche(plv8_shared_state->dshash_tranche_id, "plv8_wasm_dshash");
#endif
		plv8_shared_state->wasm_dsa_handle = DSA_HANDLE_INVALID;
		plv8_shared_state->wasm_dshash_handle = DSHASH_HANDLE_INVALID;
		pg_atomic_init_u64(&plv8_shared_state->bytes_used, 0);
		pg_atomic_init_u64(&plv8_shared_state->entries_count, 0);
		pg_atomic_init_u64(&plv8_shared_state->hits, 0);
		pg_atomic_init_u64(&plv8_shared_state->misses, 0);
		pg_atomic_init_u64(&plv8_shared_state->stores, 0);
	}
	LWLockRelease(AddinShmemInitLock);
}

static dshash_table *
plv8_get_wasm_dshash()
{
	if (plv8_shared_state == nullptr || plv8_wasm_cache_size <= 0)
		return nullptr;

	if (local_wasm_dshash != nullptr)
		return local_wasm_dshash;

	dshash_parameters params = {
		PLV8_WASM_KEY_MAX_LEN,
		sizeof(Plv8WasmCacheEntry),
		dshash_memcmp,
		dshash_memhash,
		dshash_memcpy,
		plv8_shared_state->dshash_tranche_id
	};

	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	LWLockAcquire(plv8_shared_state->lock, LW_EXCLUSIVE);
	PG_TRY();
	{
#if PG_VERSION_NUM < 190000
		LWLockRegisterTranche(plv8_shared_state->dsa_tranche_id, "plv8_wasm_dsa");
		LWLockRegisterTranche(plv8_shared_state->dshash_tranche_id, "plv8_wasm_dshash");
#endif

		if (plv8_shared_state->wasm_dsa_handle == DSA_HANDLE_INVALID)
		{
			local_wasm_dsa = dsa_create(plv8_shared_state->dsa_tranche_id);
			dsa_pin(local_wasm_dsa);
			dsa_pin_mapping(local_wasm_dsa);
			dsa_set_size_limit(local_wasm_dsa,
							   (size_t) plv8_wasm_cache_size * 1024ULL * 1024ULL);
			plv8_shared_state->wasm_dsa_handle = dsa_get_handle(local_wasm_dsa);

			local_wasm_dshash = dshash_create(local_wasm_dsa, &params, nullptr);
			plv8_shared_state->wasm_dshash_handle =
				dshash_get_hash_table_handle(local_wasm_dshash);
		}
		else
		{
			local_wasm_dsa = dsa_attach(plv8_shared_state->wasm_dsa_handle);
			dsa_pin_mapping(local_wasm_dsa);
			local_wasm_dshash = dshash_attach(
				local_wasm_dsa, &params, plv8_shared_state->wasm_dshash_handle, nullptr);
		}
	}
	PG_CATCH();
	{
		LWLockRelease(plv8_shared_state->lock);
		MemoryContextSwitchTo(oldcxt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	LWLockRelease(plv8_shared_state->lock);
	MemoryContextSwitchTo(oldcxt);
	return local_wasm_dshash;
}

bool
plv8_wasm_cache_lookup(const char *key, std::vector<uint8_t> *out_bytes)
{
	PLV8_ASSERT_MAIN_PG_THREAD();
	dshash_table *ht = plv8_get_wasm_dshash();
	if (ht == nullptr)
		return false;

	char padded_key[PLV8_WASM_KEY_MAX_LEN];
	memset(padded_key, 0, sizeof(padded_key));
	strncpy(padded_key, key, PLV8_WASM_KEY_MAX_LEN - 1);

	Plv8WasmCacheEntry *entry = static_cast<Plv8WasmCacheEntry *>(
		dshash_find(ht, padded_key, false));
	if (entry == nullptr)
	{
		pg_atomic_fetch_add_u64(&plv8_shared_state->misses, 1);
		return false;
	}

	if (DsaPointerIsValid(entry->code_ptr) && entry->code_size > 0)
	{
		const uint8_t *src = static_cast<const uint8_t *>(
			dsa_get_address(local_wasm_dsa, entry->code_ptr));
		out_bytes->assign(src, src + entry->code_size);
		dshash_release_lock(ht, entry);
		pg_atomic_fetch_add_u64(&plv8_shared_state->hits, 1);
		return true;
	}

	dshash_release_lock(ht, entry);
	pg_atomic_fetch_add_u64(&plv8_shared_state->misses, 1);
	return false;
}

bool
plv8_wasm_cache_store(const char *key, const uint8_t *bytes, size_t len)
{
	PLV8_ASSERT_MAIN_PG_THREAD();
	if (bytes == nullptr || len == 0)
		return false;

	dshash_table *ht = plv8_get_wasm_dshash();
	if (ht == nullptr)
		return false;

	size_t max_bytes = (size_t) plv8_wasm_cache_size * 1024ULL * 1024ULL;
	if (pg_atomic_read_u64(&plv8_shared_state->bytes_used) + len > max_bytes)
		return false;

	char padded_key[PLV8_WASM_KEY_MAX_LEN];
	memset(padded_key, 0, sizeof(padded_key));
	strncpy(padded_key, key, PLV8_WASM_KEY_MAX_LEN - 1);

	bool found = false;
	Plv8WasmCacheEntry *entry = static_cast<Plv8WasmCacheEntry *>(
		dshash_find_or_insert(ht, padded_key, &found));
	if (entry == nullptr)
		return false;

	if (found && DsaPointerIsValid(entry->code_ptr))
	{
		dshash_release_lock(ht, entry);
		return true;
	}

	dsa_pointer dp = dsa_allocate_extended(local_wasm_dsa, len, DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(dp))
	{
		dshash_delete_entry(ht, entry);
		return false;
	}

	void *dst = dsa_get_address(local_wasm_dsa, dp);
	memcpy(dst, bytes, len);
	entry->code_ptr = dp;
	entry->code_size = static_cast<uint32>(len);
	dshash_release_lock(ht, entry);

	pg_atomic_fetch_add_u64(&plv8_shared_state->bytes_used, len);
	pg_atomic_fetch_add_u64(&plv8_shared_state->entries_count, 1);
	pg_atomic_fetch_add_u64(&plv8_shared_state->stores, 1);
	return true;
}

/*
 * The function and context are created at the first invocation.  Their
 * lifetime is same as plv8_proc, but they are not palloc'ed memory,
 * so we need to clear them at the end of transaction.
 */
typedef struct plv8_exec_env
{
	Isolate 			   *isolate;
	Persistent<Object>		recv;
	Persistent<Context>		context;
	Local<Context> localContext() { return Local<Context>::New(isolate, context) ; }
	uint64					context_id;	/* plv8_context.id this env was created for */
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

static void DisposePlv8ContextHandles(plv8_context *ctx);
static void KillIsolateAndAllContexts(void);
static void EnsureSharedIsolate(void);
static void plv8_cache_function_remove(Oid fn_oid);
static bool plv8_cache_handlers_valid(plv8_proc_cache *cache, HeapTuple proctuple, Oid *stale_handler_oid);
static bool plv8_is_js_language(Oid lang_oid);
static char *plv8_transpile_src(const char *src, Oid lang_oid, plv8_handler_dep *deps, int *ndeps, plv8_proc *proc);
static bool plv8_try_restore_function_from_snapshot(plv8_proc_cache *cache, plv8_context *global_context, const char *raw_prosrc);

/*
 * lower_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo,
		bool validate, char ***argnames, Oid *prolang_out, char **raw_prosrc_out) throw();
static void plv8_xact_cb(XactEvent event, void *arg);

/*
 * CamelCaseFunctions are C++ functions.
 * They could raise errors with C++ throw statements, or never throw exceptions.
 */
static plv8_exec_env *CreateExecEnv(Handle<Function> function, plv8_context *context);
static plv8_exec_env *CreateExecEnv(Persistent<Function>& function, plv8_context *context);
static plv8_proc *Compile(Oid fn_oid, FunctionCallInfo fcinfo,
					bool validate, bool is_trigger);
static Local<Function> CompileFunction(plv8_context *global_context,
					const char *proname, int proarglen,
					const char *proargs[], const char *prosrc,
					bool is_trigger, bool retset);
static Datum CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv);
static plv8_context *GetPlv8Context();
static Local<ObjectTemplate> GetGlobalObjectTemplate(Isolate *isolate);

/* A GUC to specify a custom start up function to call */
static char *plv8_start_proc = NULL;

/* A GUC to specify V8 flags (e.g. --es_staging) */
static char *plv8_v8_flags = NULL;

/* A GUC to specify the ICU data directory */
static char *plv8_icu_data = NULL;

/* A GUC to specify the remote debugger port */
static int plv8_debugger_port;

/*
 * ForkSafePlatform wraps a single-threaded V8 Platform in the postmaster
 * (so zero background threads exist before postmaster fork()), and lazily
 * creates a multi-threaded V8 DefaultPlatform inside each child backend
 * process after fork() with async PostgreSQL signals masked.
 */
class ForkSafePlatform : public v8::Platform
{
private:
	std::unique_ptr<v8::Platform> postmaster_platform_;
	std::unique_ptr<v8::Platform> backend_platform_;
	pid_t backend_platform_pid_;

	v8::Platform *Active()
	{
		if (backend_platform_ && backend_platform_pid_ == getpid())
			return backend_platform_.get();
		return postmaster_platform_.get();
	}

public:
	ForkSafePlatform()
		: postmaster_platform_(v8::platform::NewSingleThreadedDefaultPlatform()),
		  backend_platform_(nullptr),
		  backend_platform_pid_(0)
	{
	}

	void EnsureBackendPlatform()
	{
		pid_t cur_pid = getpid();
		if (!backend_platform_ || backend_platform_pid_ != cur_pid)
		{
			refresh_main_pg_thread_after_fork();
			ScopedSignalBlockerForV8Workers sig_blocker;
			int threads = plv8_thread_pool_size > 0 ? plv8_thread_pool_size : 0;
			backend_platform_ = v8::platform::NewDefaultPlatform(threads);
			backend_platform_pid_ = cur_pid;
		}
	}

	int ActiveWorkerCount() const
	{
		if (backend_platform_ && backend_platform_pid_ == getpid())
			return backend_platform_->NumberOfWorkerThreads();
		return 0;
	}

	v8::PageAllocator *GetPageAllocator() override
	{
		return postmaster_platform_->GetPageAllocator();
	}

	void OnCriticalMemoryPressure() override
	{
		Active()->OnCriticalMemoryPressure();
	}

	int NumberOfWorkerThreads() override
	{
		return Active()->NumberOfWorkerThreads();
	}

	std::shared_ptr<v8::TaskRunner> GetForegroundTaskRunner(
		v8::Isolate *isolate, v8::TaskPriority priority) override
	{
		return Active()->GetForegroundTaskRunner(isolate, priority);
	}

	void PostTaskOnWorkerThreadImpl(
		v8::TaskPriority priority,
		std::unique_ptr<v8::Task> task,
		const v8::SourceLocation &location) override
	{
		EnsureBackendPlatform();
		ScopedSignalBlockerForV8Workers sig_blocker;
		Active()->PostTaskOnWorkerThread(priority, std::move(task), location);
	}

	void PostDelayedTaskOnWorkerThreadImpl(
		v8::TaskPriority priority,
		std::unique_ptr<v8::Task> task,
		double delay_in_seconds,
		const v8::SourceLocation &location) override
	{
		EnsureBackendPlatform();
		ScopedSignalBlockerForV8Workers sig_blocker;
		Active()->PostDelayedTaskOnWorkerThread(
			priority, std::move(task), delay_in_seconds, location);
	}

	bool IdleTasksEnabled(v8::Isolate *isolate) override
	{
		return Active()->IdleTasksEnabled(isolate);
	}

	std::unique_ptr<v8::JobHandle> CreateJobImpl(
		v8::TaskPriority priority,
		std::unique_ptr<v8::JobTask> job_task,
		const v8::SourceLocation &location) override
	{
		EnsureBackendPlatform();
		ScopedSignalBlockerForV8Workers sig_blocker;
		return Active()->CreateJob(priority, std::move(job_task), location);
	}

	double MonotonicallyIncreasingTime() override
	{
		return Active()->MonotonicallyIncreasingTime();
	}

	double CurrentClockTimeMillis() override
	{
		return Active()->CurrentClockTimeMillis();
	}

	v8::TracingController *GetTracingController() override
	{
		return Active()->GetTracingController();
	}
};

static ForkSafePlatform *v8_platform = nullptr;
static bool v8_engine_initialized = false;

static void
plv8_init_v8_engine(void)
{
	refresh_main_pg_thread_after_fork();

	if (v8_engine_initialized)
	{
		return;
	}

	ScopedSignalBlockerForV8Workers sig_blocker;

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
		v8_platform = new ForkSafePlatform();
	}

	const char *default_wasm_flags = "--no-wasm-lazy-compilation --no-liftoff";
	V8::SetFlagsFromString(default_wasm_flags, strlen(default_wasm_flags));
	if (plv8_v8_flags != NULL) {
		V8::SetFlagsFromString(plv8_v8_flags, strlen(plv8_v8_flags));
	}

	V8::InitializePlatform(v8_platform);

	V8::Initialize();
	v8_engine_initialized = true;
}

static bool
LoadPlv8SnapshotFileInPreload(const char *path)
{
	if (path == nullptr || path[0] == '\0')
		return false;

	int fd = open(path, O_RDONLY);
	if (fd < 0)
	{
		elog(WARNING, "plv8: could not open snapshot_file \"%s\": %m",
			 path);
		return false;
	}

	struct stat st;
	if (fstat(fd, &st) < 0 || (size_t) st.st_size < sizeof(Plv8SnapshotHeader))
	{
		close(fd);
		elog(WARNING, "plv8: snapshot_file \"%s\" is invalid or too small", path);
		return false;
	}

	size_t map_len = (size_t) st.st_size;
	void *addr = mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (addr == MAP_FAILED)
	{
		elog(WARNING, "plv8: mmap failed for snapshot_file \"%s\": %m",
			 path);
		return false;
	}

	const Plv8SnapshotHeader *hdr = static_cast<const Plv8SnapshotHeader *>(addr);
	if (memcmp(hdr->magic, PLV8_SNAPSHOT_MAGIC, 8) != 0 ||
		hdr->version != PLV8_SNAPSHOT_VERSION)
	{
		munmap(addr, map_len);
		elog(WARNING, "plv8: snapshot_file \"%s\" header magic/version mismatch", path);
		return false;
	}

	size_t slots_bytes = (size_t) hdr->num_functions * sizeof(Plv8SnapshotSlot);
	size_t header_total = sizeof(Plv8SnapshotHeader) + slots_bytes;
	if (header_total + hdr->blob_size > map_len || hdr->blob_size == 0)
	{
		munmap(addr, map_len);
		elog(WARNING, "plv8: snapshot_file \"%s\" size corrupt", path);
		return false;
	}

	const Plv8SnapshotSlot *slots_ptr = reinterpret_cast<const Plv8SnapshotSlot *>(
		static_cast<const char *>(addr) + sizeof(Plv8SnapshotHeader));
	plv8_snapshot_slots.assign(slots_ptr, slots_ptr + hdr->num_functions);

	plv8_active_snapshot_blob.data =
		static_cast<const char *>(addr) + header_total;
	plv8_active_snapshot_blob.raw_size = static_cast<int>(hdr->blob_size);
	plv8_snapshot_is_mmap = true;
	plv8_snapshot_mmap_addr = addr;
	plv8_snapshot_mmap_len = map_len;
	snprintf(plv8_snapshot_source, sizeof(plv8_snapshot_source), "snapshot_file");
	return true;
}

static bool
BuildBootScriptSnapshotInPreload(const char *script_path)
{
	if (script_path == nullptr || script_path[0] == '\0')
		return false;

	int fd = open(script_path, O_RDONLY);
	if (fd < 0)
	{
		elog(WARNING, "plv8: could not open boot_script_file \"%s\": %m",
			 script_path);
		return false;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size <= 0)
	{
		close(fd);
		elog(WARNING, "plv8: boot_script_file \"%s\" is empty or unreadable", script_path);
		return false;
	}
	std::string script_src((size_t) st.st_size, '\0');
	ssize_t nread = read(fd, &script_src[0], (size_t) st.st_size);
	close(fd);
	if (nread != st.st_size)
		return false;

	/*
	 * Compile the boot script snapshot in a short-lived forked child process
	 * and write the resulting blob into a MAP_SHARED anonymous mapping.
	 * This guarantees the postmaster process itself never spawns any V8
	 * background threads before forking PostgreSQL backends.
	 */
	size_t max_shared_blob = 64 * 1024 * 1024;
	void *shared_mem = mmap(nullptr, max_shared_blob,
							PROT_READ | PROT_WRITE,
							MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared_mem == MAP_FAILED)
		return false;

	pid_t pid = fork();
	if (pid < 0)
	{
		munmap(shared_mem, max_shared_blob);
		return false;
	}

	if (pid == 0)
	{
		plv8_init_v8_engine();
		if (v8_platform)
			v8_platform->EnsureBackendPlatform();
		v8::ArrayBuffer::Allocator *alloc =
			v8::ArrayBuffer::Allocator::NewDefaultAllocator();
		{
			v8::Isolate::CreateParams create_params;
			create_params.array_buffer_allocator = alloc;
			create_params.external_references = plv8_external_references;
			v8::SnapshotCreator creator(create_params);
			v8::Isolate *iso = creator.GetIsolate();
			{
				v8::HandleScope hs(iso);
				v8::Local<v8::Context> def_ctx = v8::Context::New(iso);
				creator.SetDefaultContext(def_ctx);

				v8::Local<v8::ObjectTemplate> global = GetGlobalObjectTemplate(iso);
				v8::Local<v8::Context> ctx = v8::Context::New(iso, nullptr, global);
				{
					v8::Context::Scope cs(ctx);
					v8::TryCatch tc(iso);
					v8::Local<v8::String> src = v8::String::NewFromUtf8(
						iso, script_src.data(), v8::NewStringType::kNormal,
						(int) script_src.size()).ToLocalChecked();
					v8::Local<v8::Script> compiled;
					if (!v8::Script::Compile(ctx, src).ToLocal(&compiled) ||
						compiled->Run(ctx).IsEmpty())
					{
						_exit(2);
					}
				}
				creator.AddContext(ctx);
			}
			v8::StartupData blob = creator.CreateBlob(
				v8::SnapshotCreator::FunctionCodeHandling::kKeep);
			if (blob.data == nullptr || blob.raw_size <= 0 ||
				sizeof(uint64) + (size_t) blob.raw_size > max_shared_blob)
			{
				_exit(3);
			}
			uint64 sz = (uint64) blob.raw_size;
			memcpy(shared_mem, &sz, sizeof(uint64));
			memcpy(static_cast<char *>(shared_mem) + sizeof(uint64),
				   blob.data, (size_t) blob.raw_size);
			delete[] blob.data;
		}
		delete alloc;
		_exit(0);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0)
	{
		if (errno != EINTR)
			break;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		munmap(shared_mem, max_shared_blob);
		elog(WARNING, "plv8: failed to build boot snapshot from \"%s\"", script_path);
		return false;
	}

	uint64 blob_sz = 0;
	memcpy(&blob_sz, shared_mem, sizeof(uint64));
	if (blob_sz == 0 || sizeof(uint64) + blob_sz > max_shared_blob)
	{
		munmap(shared_mem, max_shared_blob);
		return false;
	}

	mprotect(shared_mem, max_shared_blob, PROT_READ);
	plv8_active_snapshot_blob.data =
		static_cast<const char *>(shared_mem) + sizeof(uint64);
	plv8_active_snapshot_blob.raw_size = static_cast<int>(blob_sz);
	plv8_snapshot_is_mmap = true;
	plv8_snapshot_mmap_addr = shared_mem;
	plv8_snapshot_mmap_len = max_shared_blob;
	snprintf(plv8_snapshot_source, sizeof(plv8_snapshot_source), "boot_script_prefork");
	return true;
}

/*
 * We use vector instead of hash since the size of this array
 * is expected to be short in most cases.
 */
static std::vector<plv8_context *> ContextVector;

#ifdef ENABLE_DEBUGGER_SUPPORT
v8::Persistent<v8::Context> debug_message_context;

void DispatchDebugMessages() {
  v8::Context::Scope scope(debug_message_context);

  v8::Debug::ProcessDebugMessages();
}
#endif  // ENABLE_DEBUGGER_SUPPORT

void OOMErrorHandler(const char* location, const v8::OOMDetails &) {
	Isolate *isolate = Isolate::GetCurrent();
	if (isolate)
		isolate->TerminateExecution();
	plv8_isolate_oom_killed = true;
	if (current_context)
		current_context->is_dead = true;
	elog(ERROR, "Out of memory error");
}

void GCEpilogueCallback(Isolate* isolate, GCType type, GCCallbackFlags /* flags */) {
	HeapStatistics heap_statistics;
	isolate->GetHeapStatistics(&heap_statistics);
	if (type != GCType::kGCTypeIncrementalMarking
		&& heap_statistics.used_heap_size() > plv8_memory_limit * 1_MB) {
		plv8_isolate_oom_killed = true;
		if (current_context)
			current_context->is_dead = true;
		const char *error_message = "Out of memory";
		Local<v8::String>	result = ToString(error_message, 13);
		isolate->ThrowException(result);

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
	HeapStatistics heap_statistics;
	isolate->GetHeapStatistics(&heap_statistics);
	if (heap_statistics.used_heap_size() > plv8_memory_limit * 1_MB) {
		plv8_isolate_oom_killed = true;
		if (current_context)
			current_context->is_dead = true;
		isolate->TerminateExecution();
	}
	// need to give back more space
	// to make sure it can unwind the stack and process exceptions
	return current_heap_limit + 1_MB;
}

void PromiseRejectCB(PromiseRejectMessage rejection) {
	if (!current_context)
		return;
	auto event = rejection.GetEvent();
	if (event != v8::kPromiseRejectWithNoHandler && event != v8::kPromiseHandlerAddedAfterReject)
		return;
	auto	promise = rejection.GetPromise();
	auto	isolate = Isolate::GetCurrent();

	if (rejection.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
		if (current_context->ignore_unhandled_promises) return;
		// Remove handled promises from the list
		auto& unhandled_promises = current_context->unhandled_promises;
		for (auto it = unhandled_promises.begin(); it != unhandled_promises.end(); ++it) {
			auto unhandled_promise = std::get<0>(*it).Get(isolate);
			if (unhandled_promise == promise) {
				unhandled_promises.erase(it--);
			}
		}
		return;
	}

	auto exception = rejection.GetValue();
	Local<Message> message;
	// Assume that all objects are stack-traces.
	if (exception->IsObject()) {
		message = Exception::CreateMessage(isolate, exception);
	}
	if (!exception->IsNativeError() &&
		(message.IsEmpty() || message->GetStackTrace().IsEmpty())) {
		// If there is no real Error object, manually throw and catch a stack trace.
		TryCatch try_catch(isolate);
		try_catch.SetVerbose(true);
		isolate->ThrowException(Exception::Error(
				v8::String::NewFromUtf8Literal(isolate, "Unhandled Promise.")));
		message = try_catch.Message();
		exception = try_catch.Exception();
	}
	if (current_context->ignore_unhandled_promises) return;
	current_context->unhandled_promises.emplace_back(
			v8::Global<v8::Promise>(isolate, promise),
			v8::Global<v8::Message>(isolate, message),
			v8::Global<v8::Value>(isolate, exception));
}

void HandleUnhandledPromiseRejections() {
	if (!current_context)
		return;
	// Avoid recursive calls to HandleUnhandledPromiseRejections.
	auto isolate = current_context->isolate;
	if (current_context->ignore_unhandled_promises) return;
	current_context->ignore_unhandled_promises = true;
	HandleScope scope(isolate);
	// Ignore promises that get added during error reporting.
	size_t i = 0;
	auto& unhandled_promises = current_context->unhandled_promises;
	for (; i < unhandled_promises.size(); i++) {
		const auto& tuple = unhandled_promises[i];
		Local<v8::Message> message = std::get<1>(tuple).Get(isolate);
		Local<v8::Value> exception = std::get<2>(tuple).Get(isolate);
		js_error error(isolate, exception, message);
		error.log(WARNING, "Unhandled Promise rejection: %s");
	}
	unhandled_promises.clear();
	current_context->ignore_unhandled_promises = false;
}

static void
EnsureSharedIsolate(void)
{
	plv8_init_v8_engine();
	if (v8_platform)
		v8_platform->EnsureBackendPlatform();

	if (current_isolate != nullptr &&
		!plv8_isolate_oom_killed &&
		!current_isolate->IsDead())
	{
		return;
	}

	if (current_isolate != nullptr)
	{
		KillIsolateAndAllContexts();
	}

	if (!plv8_snapshot_is_mmap &&
		plv8_snapshot_file != NULL && plv8_snapshot_file[0] != '\0')
	{
		LoadPlv8SnapshotFileInPreload(plv8_snapshot_file);
	}

	ScopedSignalBlockerForV8Workers sig_blocker;

	Isolate::CreateParams params;
	current_allocator = new ArrayAllocator(plv8_memory_limit * 1_MB);
	params.array_buffer_allocator = current_allocator;
	params.external_references = plv8_external_references;
	if (plv8_active_snapshot_blob.data != nullptr &&
		plv8_active_snapshot_blob.raw_size > 0)
	{
		params.snapshot_blob = &plv8_active_snapshot_blob;
	}

	ResourceConstraints rc;
	rc.ConfigureDefaults(plv8_memory_limit * 1_MB * 2, plv8_memory_limit * 1_MB * 2);
	params.constraints = rc;

	current_isolate = Isolate::New(params);
	plv8_isolate_oom_killed = false;
	plv8_last_heap_size = 0;

	current_isolate->SetOOMErrorHandler(OOMErrorHandler);
	current_isolate->AddGCEpilogueCallback(GCEpilogueCallback);
	current_isolate->AddNearHeapLimitCallback(NearHeapLimitHandler, NULL);
	current_isolate->SetPromiseRejectCallback(PromiseRejectCB);
}

void
_PG_init(void)
{
	HASHCTL    hash_ctl = { 0 };

	refresh_main_pg_thread_after_fork();

	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(plv8_proc_cache);
	hash_ctl.hash = oid_hash;
	plv8_proc_cache_hash = hash_create("PLv8 Procedures", 32,
									   &hash_ctl, HASH_ELEM | HASH_FUNCTION);

    config_generic *guc_value;

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
                                   NULL,
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
                                   PGC_USERSET, 0,
                                   NULL,
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
                                   gettext_noop("V8 engine initialization flags (e.g. --harmony for all current harmony features)."),
                                   NULL,
                                   &plv8_v8_flags,
                                   NULL,
                                   PGC_USERSET, 0,
                                   NULL,
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
                                NULL,
                                NULL,
                                NULL);
    }
#undef DEBUGGER_PORT_VAR

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
                                PGC_USERSET, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef EXECUTION_TIMEOUT_VAR

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
                                NULL,
                                NULL,
                                NULL);
    }
#undef MEMORY_LIMIT_VAR

#define THREAD_POOL_SIZE_VAR "plv8.thread_pool_size"
    guc_value = plv8_find_option(THREAD_POOL_SIZE_VAR);
    if (guc_value != NULL) {
        plv8_thread_pool_size = plv8_int_option(guc_value);
    } else {
        DefineCustomIntVariable(THREAD_POOL_SIZE_VAR,
                                gettext_noop("Number of V8 background platform worker threads per backend (0 = hardware default)."),
                                NULL,
                                &plv8_thread_pool_size,
                                0, 0, 64,
                                PGC_SUSET, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef THREAD_POOL_SIZE_VAR

    GucContext preload_guc_context = process_shared_preload_libraries_in_progress
        ? PGC_POSTMASTER
        : PGC_SUSET;

#define BOOT_SCRIPT_FILE_VAR "plv8.boot_script_file"
    guc_value = plv8_find_option(BOOT_SCRIPT_FILE_VAR);
    if (guc_value != NULL) {
        plv8_boot_script_file = plv8_string_option(guc_value);
    } else {
        DefineCustomStringVariable(BOOT_SCRIPT_FILE_VAR,
                                   gettext_noop("JavaScript file compiled into a shared V8 startup snapshot during postmaster preload."),
                                   NULL,
                                   &plv8_boot_script_file,
                                   NULL,
                                   preload_guc_context, 0,
                                   NULL,
                                   NULL,
                                   NULL);
    }
#undef BOOT_SCRIPT_FILE_VAR

#define SNAPSHOT_FILE_VAR "plv8.snapshot_file"
    guc_value = plv8_find_option(SNAPSHOT_FILE_VAR);
    if (guc_value != NULL) {
        plv8_snapshot_file = plv8_string_option(guc_value);
    } else {
        DefineCustomStringVariable(SNAPSHOT_FILE_VAR,
                                   gettext_noop("Path to precompiled PL/v8 V8 startup snapshot file."),
                                   NULL,
                                   &plv8_snapshot_file,
                                   NULL,
                                   preload_guc_context, 0,
                                   NULL,
                                   NULL,
                                   NULL);
    }
#undef SNAPSHOT_FILE_VAR

#define WASM_CACHE_SIZE_VAR "plv8.wasm_cache_size"
    guc_value = plv8_find_option(WASM_CACHE_SIZE_VAR);
    if (guc_value != NULL) {
        plv8_wasm_cache_size = plv8_int_option(guc_value);
    } else {
        DefineCustomIntVariable(WASM_CACHE_SIZE_VAR,
                                gettext_noop("Maximum shared memory cache size (MB) for serialized WebAssembly native code."),
                                NULL,
                                &plv8_wasm_cache_size,
                                64, 0, 2048,
                                preload_guc_context, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef WASM_CACHE_SIZE_VAR

	if (process_shared_preload_libraries_in_progress)
	{
		prev_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = plv8_shmem_request;
		prev_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = plv8_shmem_startup;

		plv8_init_v8_engine();
		if (plv8_snapshot_file != NULL && plv8_snapshot_file[0] != '\0')
		{
			LoadPlv8SnapshotFileInPreload(plv8_snapshot_file);
		}
		else if (plv8_boot_script_file != NULL && plv8_boot_script_file[0] != '\0')
		{
			BuildBootScriptSnapshotInPreload(plv8_boot_script_file);
		}
	}

	RegisterXactCallback(plv8_xact_cb, NULL);

	EmitWarningsOnPlaceholders("plv8");

	if (!process_shared_preload_libraries_in_progress)
	{
		plv8_init_v8_engine();
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
		if (!env->context.IsEmpty())
		{
			env->context.Reset();
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
plv8_new_exec_env(plv8_context *context)
{
	plv8_exec_env	   *xenv = (plv8_exec_env *)
		MemoryContextAllocZero(TopTransactionContext, sizeof(plv8_exec_env));

	new(&xenv->context) Persistent<Context>();
	new(&xenv->recv) Persistent<Object>();
	xenv->isolate = context->isolate;
	xenv->context_id = context->id;

	/*
	 * Add it to the list, which will be freed in the end of top transaction.
	 */
	xenv->next = exec_env_head;
	exec_env_head = xenv;

	return xenv;
}

Datum
plv8_call_handler(PG_FUNCTION_ARGS)
{
	Oid		fn_oid = fcinfo->flinfo->fn_oid;
	bool	is_trigger = CALLED_AS_TRIGGER(fcinfo);

	try
	{
		plv8_context   *context = GetPlv8Context();
		CurrentContextScope	context_scope(context);
		Isolate::Scope	scope(context->isolate);
		HandleScope	handle_scope(context->isolate);
		plv8_proc	   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;

		if (proc)
		{
			bool valid = (proc->xenv->context_id == context->id &&
						  !proc->cache->function.IsEmpty());
			if (valid && proc->cache->nhandlers > 0)
			{
				PG_TRY();
				{
					Oid stale_handler_oid = InvalidOid;
					if (!plv8_cache_handlers_valid(proc->cache, NULL, &stale_handler_oid))
					{
						valid = false;
						if (OidIsValid(stale_handler_oid))
							plv8_cache_function_remove(stale_handler_oid);
					}
				}
				PG_CATCH();
				{
					throw pg_error();
				}
				PG_END_TRY();
			}
			if (!valid)
			{
				pfree(proc);
				proc = NULL;
				fcinfo->flinfo->fn_extra = NULL;
			}
		}

		if (!proc)
		{
			proc = Compile(fn_oid, fcinfo, false, is_trigger);
			proc->xenv = CreateExecEnv(proc->cache->function, context);
			fcinfo->flinfo->fn_extra = proc;
		}

		plv8_proc_cache *cache = proc->cache;
		struct PassUserTypesScope {
			bool prev;
			explicit PassUserTypesScope(bool val) : prev(plv8_pass_user_types_as_bytes) {
				plv8_pass_user_types_as_bytes = val;
			}
			~PassUserTypesScope() {
				plv8_pass_user_types_as_bytes = prev;
			}
		} user_types_scope(cache->nhandlers > 0);

		if (is_trigger)
			return CallTrigger(fcinfo, proc->xenv);
		else if (cache->retset)
			return CallSRFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
		else
			return CallFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
	}
	catch (js_error& e)
	{
		if (current_context == nullptr &&
			(plv8_isolate_oom_killed ||
			 (current_allocator && current_allocator->hasOOM())))
		{
			KillIsolateAndAllContexts();
		}
		e.rethrow();
	}
	catch (pg_error& e)
	{
		if (current_context == nullptr &&
			(plv8_isolate_oom_killed ||
			 (current_allocator && current_allocator->hasOOM())))
		{
			KillIsolateAndAllContexts();
		}
		e.rethrow();
	}

	return (Datum) 0;	// keep compiler quiet
}


static void
DisposePlv8ContextHandles(plv8_context *ctx)
{
	HASH_SEQ_STATUS		status;
	plv8_proc_cache*	cache;

	if (plv8_proc_cache_hash)
	{
		hash_seq_init(&status, plv8_proc_cache_hash);
		cache = (plv8_proc_cache *) hash_seq_search(&status);
		while (cache != nullptr) {
			if (cache->user_id == ctx->user_id) {
				if (cache->prosrc)
				{
					pfree(cache->prosrc);
					cache->prosrc = NULL;
				}
				cache->function.Reset();
				cache->nhandlers = 0;
			}
			cache = (plv8_proc_cache *) hash_seq_search(&status);
		}
	}

	for (plv8_exec_env *env = exec_env_head; env != NULL; env = env->next)
	{
		if (env->context_id == ctx->id)
		{
			env->recv.Reset();
			env->context.Reset();
		}
	}

	for (auto &tuple : ctx->unhandled_promises)
	{
		std::get<0>(tuple).Reset();
		std::get<1>(tuple).Reset();
		std::get<2>(tuple).Reset();
	}
	ctx->unhandled_promises.clear();
	ctx->unhandled_promises.~vector();

	ctx->context.Reset();
	ctx->compile_context.Reset();
	ctx->recv_templ.Reset();
	ctx->plan_template.Reset();
	ctx->cursor_template.Reset();
	ctx->window_template.Reset();
	ctx->microtask_queue = nullptr;
}

static void
KillIsolateAndAllContexts(void)
{
	for (size_t i = 0; i < ContextVector.size(); i++)
	{
		plv8_context *ctx = ContextVector[i];
		if (ctx != nullptr)
		{
			if (current_context == ctx)
				current_context = nullptr;
			DisposePlv8ContextHandles(ctx);
			pfree(ctx);
		}
	}
	ContextVector.clear();

	if (plv8_proc_cache_hash)
	{
		HASH_SEQ_STATUS status;
		plv8_proc_cache *cache;
		hash_seq_init(&status, plv8_proc_cache_hash);
		while ((cache = (plv8_proc_cache *) hash_seq_search(&status)) != nullptr)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Reset();
			cache->nhandlers = 0;
		}
	}

	for (plv8_exec_env *env = exec_env_head; env != NULL; env = env->next)
	{
		env->recv.Reset();
		env->context.Reset();
	}

	if (current_isolate != nullptr)
	{
		while (current_isolate->IsInUse())
			current_isolate->Exit();
		current_isolate->Dispose();
		current_isolate = nullptr;
	}

	if (current_allocator != nullptr)
	{
		delete current_allocator;
		current_allocator = nullptr;
	}

	plv8_isolate_oom_killed = false;
	plv8_last_heap_size = 0;
}

Datum
plv8_reset(PG_FUNCTION_ARGS)
{
	Oid					user_id = GetUserId();
	unsigned long		i;

	for (i = 0; i < ContextVector.size(); i++)
	{
		if (ContextVector[i]->user_id == user_id)
		{
			plv8_context *context = ContextVector[i];
			ContextVector.erase(ContextVector.begin() + i);
			if (current_context == context)
				current_context = nullptr;
			DisposePlv8ContextHandles(context);
			pfree(context);
			break;
		}
	}
	return (Datum) 0;
}

Datum
plv8_info(PG_FUNCTION_ARGS)
{
	PLV8_ASSERT_MAIN_PG_THREAD();

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call plv8_info")));

	if (ContextVector.empty())
	{
		try
		{
			(void) GetPlv8Context();
		}
		catch (js_error& e)	{ e.rethrow(); }
		catch (pg_error& e)	{ e.rethrow(); }
	}

	uint64 wasm_bytes = 0, wasm_entries = 0, wasm_hits = 0, wasm_misses = 0, wasm_stores = 0;
	bool wasm_cache_enabled = (plv8_shared_state != nullptr && plv8_wasm_cache_size > 0);
	if (wasm_cache_enabled)
	{
		wasm_bytes = pg_atomic_read_u64(&plv8_shared_state->bytes_used);
		wasm_entries = pg_atomic_read_u64(&plv8_shared_state->entries_count);
		wasm_hits = pg_atomic_read_u64(&plv8_shared_state->hits);
		wasm_misses = pg_atomic_read_u64(&plv8_shared_state->misses);
		wasm_stores = pg_atomic_read_u64(&plv8_shared_state->stores);
	}

	bool all_same_isolate = true;
	for (size_t i = 0; i < ContextVector.size(); i++)
	{
		if (ContextVector[i]->isolate != current_isolate)
			all_same_isolate = false;
	}

	size_t total_heap = 0, used_heap = 0, ext_mem = 0;
	if (current_isolate != nullptr)
	{
		HeapStatistics v8_heap_stats;
		current_isolate->GetHeapStatistics(&v8_heap_stats);
		total_heap = v8_heap_stats.total_heap_size();
		used_heap = v8_heap_stats.used_heap_size();
		ext_mem = v8_heap_stats.external_memory();
	}

	bool snap_loaded = (plv8_active_snapshot_blob.data != nullptr &&
						plv8_active_snapshot_blob.raw_size > 0);

	StringInfoData roles_buf;
	initStringInfo(&roles_buf);
	appendStringInfoChar(&roles_buf, '[');
	for (size_t i = 0; i < ContextVector.size(); i++)
	{
		if (i > 0)
			appendStringInfoChar(&roles_buf, ',');
		char *username = GetUserNameFromId(ContextVector[i]->user_id, false);
		appendStringInfo(&roles_buf,
						 "{\"user\":\"%s\",\"total_heap_size\":%zu,\"used_heap_size\":%zu,\"external_memory\":%zu}",
						 username ? username : "unknown",
						 total_heap, used_heap, ext_mem);
	}
	appendStringInfoChar(&roles_buf, ']');

	StringInfoData buf;
	initStringInfo(&buf);
	appendStringInfo(
		&buf,
		"{\"backend_pid\":%d,"
		"\"single_isolate\":%s,"
		"\"isolate_shared\":%s,"
		"\"active_role_contexts\":%zu,"
		"\"contexts\":%zu,"
		"\"roles\":%s,"
		"\"total_heap_size\":%zu,"
		"\"used_heap_size\":%zu,"
		"\"external_memory\":%zu,"
		"\"thread_pool_size\":%d,"
		"\"active_worker_threads\":%d,"
		"\"snapshot_active\":%s,"
		"\"snapshot_source\":\"%s\","
		"\"snapshot_blob_bytes\":%d,"
		"\"snapshot_fn_count\":%zu,"
		"\"snapshot_functions_restored\":%llu,"
		"\"snapshot\":{\"loaded\":%s,\"bytes\":%d,\"functions_count\":%zu,\"functions_restored\":%llu,\"source\":\"%s\"},"
		"\"wasm_cache\":{\"enabled\":%s,\"entries\":%llu,\"bytes_used\":%llu,\"hits\":%llu,\"misses\":%llu,\"stores\":%llu}}",
		(int) getpid(),
		all_same_isolate ? "true" : "false",
		all_same_isolate ? "true" : "false",
		ContextVector.size(),
		ContextVector.size(),
		roles_buf.data,
		total_heap,
		used_heap,
		ext_mem,
		plv8_thread_pool_size,
		v8_platform ? v8_platform->ActiveWorkerCount() : 0,
		snap_loaded ? "true" : "false",
		plv8_snapshot_source,
		plv8_active_snapshot_blob.raw_size,
		plv8_snapshot_slots.size(),
		(unsigned long long) plv8_snapshot_functions_restored,
		snap_loaded ? "true" : "false",
		plv8_active_snapshot_blob.raw_size,
		plv8_snapshot_slots.size(),
		(unsigned long long) plv8_snapshot_functions_restored,
		plv8_snapshot_source,
		wasm_cache_enabled ? "true" : "false",
		(unsigned long long) wasm_entries,
		(unsigned long long) wasm_bytes,
		(unsigned long long) wasm_hits,
		(unsigned long long) wasm_misses,
		(unsigned long long) wasm_stores);

	pfree(roles_buf.data);
	Datum res = DirectFunctionCall1(jsonb_in, CStringGetDatum(buf.data));
	pfree(buf.data);
	PG_RETURN_DATUM(res);
}

Datum
plv8_save_snapshot(PG_FUNCTION_ARGS)
{
	PLV8_ASSERT_MAIN_PG_THREAD();

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call plv8_save_snapshot")));

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("output_path must not be null")));

	char *out_path = text_to_cstring(PG_GETARG_TEXT_PP(0));
	std::vector<Oid> target_oids;

	if (PG_NARGS() >= 2 && !PG_ARGISNULL(1))
	{
		ArrayType *arr = PG_GETARG_ARRAYTYPE_P(1);
		Datum *elems = nullptr;
		bool *nulls = nullptr;
		int nelems = 0;
		deconstruct_array(arr, REGPROCEDUREOID, sizeof(Oid), true, TYPALIGN_INT,
						  &elems, &nulls, &nelems);
		for (int i = 0; i < nelems; i++)
		{
			if (!nulls[i])
			{
				Oid fn_oid = DatumGetObjectId(elems[i]);
				if (OidIsValid(fn_oid))
					target_oids.push_back(fn_oid);
			}
		}
	}

	NameData plv8_lang_name = {"plv8"};
	HeapTuple langTup = SearchSysCache1(LANGNAME, NameGetDatum(&plv8_lang_name));
	if (!HeapTupleIsValid(langTup))
		ereport(ERROR, (errmsg("language \"plv8\" does not exist")));
	Oid plv8_lang_oid = ((Form_pg_language) GETSTRUCT(langTup))->oid;
	ReleaseSysCache(langTup);

	plv8_init_v8_engine();

	struct BypassSnapshotScope
	{
		bool prev;
		BypassSnapshotScope() : prev(plv8_bypass_snapshot_restore)
		{
			plv8_bypass_snapshot_restore = true;
		}
		~BypassSnapshotScope()
		{
			plv8_bypass_snapshot_restore = prev;
		}
	} bypass_scope;

	struct PendingSnapFn
	{
		Plv8SnapshotSlot slot;
		std::string proname;
		std::string wrapped_src;
	};
	std::vector<PendingSnapFn> pending_fns;
	int prewarmed_wasm_fns = 0;

	for (size_t idx = 0; idx < target_oids.size(); idx++)
	{
		Oid fn_oid = target_oids[idx];
		HeapTuple procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
		if (!HeapTupleIsValid(procTup))
			ereport(ERROR, (errmsg("cache lookup failed for function %u", fn_oid)));

		Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
		Oid prolang = procStruct->prolang;
		if (!plv8_is_js_language(prolang))
		{
			ReleaseSysCache(procTup);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function %u is not a PL/v8 or PL/<any> JS function", fn_oid)));
		}

		bool isnull = false;
		Datum prosrc_datum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
		{
			ReleaseSysCache(procTup);
			ereport(ERROR, (errmsg("null prosrc for function %u", fn_oid)));
		}

		char proname[NAMEDATALEN];
		strlcpy(proname, NameStr(procStruct->proname), NAMEDATALEN);
		TransactionId fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		ItemPointerData fn_tid = procTup->t_self;
		bool is_trigger = (procStruct->prorettype == TRIGGEROID);

		Oid *argtypes = nullptr;
		char **argnames = nullptr;
		char *argmodes = nullptr;
		int nargs = get_func_arg_info(procTup, &argtypes, &argnames, &argmodes);
		char *prosrc_cstr = TextDatumGetCString(prosrc_datum);
		ReleaseSysCache(procTup);

		int inargs = 0;
		for (int i = 0; i < nargs; i++)
		{
			char argmode = argmodes ? argmodes[i] : PROARGMODE_IN;
			if (argmode == PROARGMODE_IN ||
				argmode == PROARGMODE_INOUT ||
				argmode == PROARGMODE_VARIADIC)
			{
				if (argnames)
					argnames[inargs] = argnames[i];
				inargs++;
			}
		}

		PendingSnapFn item;
		memset(&item.slot, 0, sizeof(item.slot));
		item.slot.fn_oid = fn_oid;
		item.slot.fn_xmin = fn_xmin;
		item.slot.fn_tid = fn_tid;
		item.proname = proname;

		if (prolang != plv8_lang_oid)
		{
			pfree(prosrc_cstr);
			prosrc_cstr = nullptr;
			try
			{
				plv8_context *ctx = GetPlv8Context();
				CurrentContextScope context_scope(ctx);
				Isolate::Scope scope(ctx->isolate);
				HandleScope handle_scope(ctx->isolate);
				if (plv8_proc_cache_hash != NULL)
					plv8_cache_function_remove(fn_oid);
				plv8_proc *compiled_proc = Compile(fn_oid, NULL, false, is_trigger);
				if (compiled_proc && compiled_proc->cache && compiled_proc->cache->prosrc)
				{
					prosrc_cstr = pstrdup(compiled_proc->cache->prosrc);
					item.slot.nhandlers = compiled_proc->cache->nhandlers;
					if (item.slot.nhandlers > 0)
					{
						memcpy(item.slot.handlers, compiled_proc->cache->handlers,
							   sizeof(plv8_handler_dep) * item.slot.nhandlers);
					}
					prewarmed_wasm_fns++;
				}
			}
			catch (js_error& e)	{ e.rethrow(); }
			catch (pg_error& e)	{ e.rethrow(); }

			if (prosrc_cstr == nullptr)
				ereport(ERROR, (errmsg("failed to transpile function %u for snapshot", fn_oid)));
		}

		StringInfoData src;
		initStringInfo(&src);
		appendStringInfo(&src, "(function (");
		if (is_trigger)
		{
			appendStringInfo(&src,
				"NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
				"TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
		}
		else
		{
			for (int i = 0; i < inargs; i++)
			{
				if (i > 0)
					appendStringInfoChar(&src, ',');
				if (argnames && argnames[i])
					appendStringInfoString(&src, argnames[i]);
				else
					appendStringInfo(&src, "$%d", i + 1);
			}
		}
		appendStringInfo(&src, "){\n%s\n})", prosrc_cstr);
		pfree(prosrc_cstr);

		item.wrapped_src.assign(src.data, src.len);
		pfree(src.data);
		pending_fns.push_back(std::move(item));
	}

	std::vector<Plv8SnapshotSlot> compiled_slots;
	v8::StartupData blob = { nullptr, 0 };

	{
		ScopedSignalBlockerForV8Workers sig_blocker;
		v8::ArrayBuffer::Allocator *snap_allocator =
			v8::ArrayBuffer::Allocator::NewDefaultAllocator();
		{
			v8::Isolate::CreateParams create_params;
			create_params.array_buffer_allocator = snap_allocator;
			create_params.external_references = plv8_external_references;
			v8::SnapshotCreator creator(create_params);
			v8::Isolate *snap_isolate = creator.GetIsolate();
			{
				v8::HandleScope handle_scope(snap_isolate);
				v8::Local<v8::Context> default_ctx = v8::Context::New(snap_isolate);
				creator.SetDefaultContext(default_ctx);

				v8::Local<v8::ObjectTemplate> global = GetGlobalObjectTemplate(snap_isolate);
				v8::Local<v8::Context> context = v8::Context::New(snap_isolate, nullptr, global);
				{
					v8::Context::Scope context_scope(context);

					if (plv8_boot_script_file != NULL && plv8_boot_script_file[0] != '\0')
					{
						int bfd = open(plv8_boot_script_file, O_RDONLY);
						if (bfd >= 0)
						{
							struct stat bst;
							if (fstat(bfd, &bst) == 0 && bst.st_size > 0)
							{
								std::string bsrc((size_t) bst.st_size, '\0');
								if (read(bfd, &bsrc[0], (size_t) bst.st_size) == bst.st_size)
								{
									v8::TryCatch tc(snap_isolate);
									v8::Local<v8::String> s = v8::String::NewFromUtf8(
										snap_isolate, bsrc.data(),
										v8::NewStringType::kNormal,
										(int) bsrc.size()).ToLocalChecked();
									v8::Local<v8::Script> sc;
									if (v8::Script::Compile(context, s).ToLocal(&sc))
									{
										v8::Local<v8::Value> unused_res;
										if (!sc->Run(context).ToLocal(&unused_res)) {}
									}
								}
							}
							close(bfd);
						}
					}

					for (size_t idx = 0; idx < pending_fns.size(); idx++)
					{
						const PendingSnapFn &item = pending_fns[idx];
						v8::TryCatch tc(snap_isolate);
						v8::Local<v8::String> fn_name = v8::String::NewFromUtf8(
							snap_isolate, item.proname.c_str()).ToLocalChecked();
						v8::Local<v8::String> fn_source = v8::String::NewFromUtf8(
							snap_isolate, item.wrapped_src.data(),
							v8::NewStringType::kNormal,
							(int) item.wrapped_src.size()).ToLocalChecked();

						v8::ScriptOrigin origin(fn_name);
						v8::Local<v8::Script> script;
						v8::Local<v8::Value> fn_val;
						if (!v8::Script::Compile(context, fn_source, &origin).ToLocal(&script) ||
							!script->Run(context).ToLocal(&fn_val) ||
							!fn_val->IsFunction())
						{
							ereport(ERROR,
									(errmsg("failed to compile function \"%s\" (%u) into V8 snapshot",
											item.proname.c_str(), item.slot.fn_oid)));
						}

						char prop_buf[64];
						snprintf(prop_buf, sizeof(prop_buf), "__plv8_snap_fn_%zu",
								 compiled_slots.size());
						v8::Local<v8::String> prop_name = v8::String::NewFromUtf8(
							snap_isolate, prop_buf,
							v8::NewStringType::kInternalized).ToLocalChecked();
						context->Global()->DefineOwnProperty(
							context, prop_name, fn_val,
							static_cast<v8::PropertyAttribute>(v8::DontEnum)).Check();

						compiled_slots.push_back(item.slot);
					}
				}
				creator.AddContext(context);
			}
			blob = creator.CreateBlob(
				v8::SnapshotCreator::FunctionCodeHandling::kKeep);
		}
		delete snap_allocator;
	}

	if (blob.data == nullptr || blob.raw_size <= 0)
		ereport(ERROR, (errmsg("v8::SnapshotCreator::CreateBlob() failed")));

	std::string tmp_path = std::string(out_path) + ".tmp";
	int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
	{
		delete[] blob.data;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open snapshot output file \"%s\": %m",
						tmp_path.c_str())));
	}

	Plv8SnapshotHeader hdr;
	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, PLV8_SNAPSHOT_MAGIC, 8);
	hdr.version = PLV8_SNAPSHOT_VERSION;
	hdr.num_functions = static_cast<uint32>(compiled_slots.size());
	hdr.blob_size = static_cast<uint64>(blob.raw_size);

	bool write_ok = true;
	if (write(fd, &hdr, sizeof(hdr)) != (ssize_t) sizeof(hdr))
		write_ok = false;

	if (write_ok && !compiled_slots.empty())
	{
		size_t slots_bytes = compiled_slots.size() * sizeof(Plv8SnapshotSlot);
		if (write(fd, compiled_slots.data(), slots_bytes) != (ssize_t) slots_bytes)
			write_ok = false;
	}

	if (write_ok)
	{
		const char *p = blob.data;
		size_t rem = (size_t) blob.raw_size;
		while (rem > 0)
		{
			ssize_t w = write(fd, p, rem);
			if (w <= 0)
			{
				write_ok = false;
				break;
			}
			p += w;
			rem -= (size_t) w;
		}
	}

	int raw_size = blob.raw_size;
	delete[] blob.data;
	close(fd);

	if (!write_ok || rename(tmp_path.c_str(), out_path) != 0)
	{
		unlink(tmp_path.c_str());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("failed to write snapshot file \"%s\"", out_path)));
	}

	StringInfoData buf;
	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "{\"status\":\"ok\",\"path\":\"%s\",\"bytes\":%d,\"bytes_written\":%zu,\"functions_compiled\":%zu,\"snapshotted_js_functions\":%zu,\"prewarmed_wasm_functions\":%d,\"v8_version\":\"%s\"}",
					 out_path,
					 raw_size,
					 sizeof(hdr) + compiled_slots.size() * sizeof(Plv8SnapshotSlot) + (size_t) raw_size,
					 compiled_slots.size(),
					 compiled_slots.size(),
					 prewarmed_wasm_fns,
					 v8::V8::GetVersion());

	Datum jsonb_res = DirectFunctionCall1(jsonb_in, CStringGetDatum(buf.data));
	pfree(buf.data);
	pfree(out_path);
	PG_RETURN_DATUM(jsonb_res);
}

Datum
plv8_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));

	Assert(IsA(codeblock, InlineCodeBlock));

	try
	{
		plv8_context	   *context = GetPlv8Context();
		CurrentContextScope	context_scope(context);
		Isolate::Scope		scope(context->isolate);
		HandleScope			handle_scope(context->isolate);
		char			   *source_text = plv8_transpile_src(
										codeblock->source_text,
										codeblock->langOid,
										NULL, NULL, NULL);

		Local<Function>	function = CompileFunction(context,
										NULL, 0, NULL,
										source_text, false, false);
		plv8_exec_env	   *xenv = CreateExecEnv(function, context);
		return CallFunction(fcinfo, xenv, 0, NULL, NULL);
	}
	catch (js_error& e)
	{
		if (current_context == nullptr &&
			(plv8_isolate_oom_killed ||
			 (current_allocator && current_allocator->hasOOM())))
		{
			KillIsolateAndAllContexts();
		}
		e.rethrow();
	}
	catch (pg_error& e)
	{
		if (current_context == nullptr &&
			(plv8_isolate_oom_killed ||
			 (current_allocator && current_allocator->hasOOM())))
		{
			KillIsolateAndAllContexts();
		}
		e.rethrow();
	}

	return (Datum) 0;	// keep compiler quiet
}

static TimeoutId plv8_timeout_id = USER_TIMEOUT;
static bool plv8_timeout_registered = false;
static volatile sig_atomic_t plv8_in_js_execution = 0;
static volatile sig_atomic_t plv8_timeout_expired = 0;
static TimestampTz plv8_deadline_tz = 0;

static void
plv8_timeout_handler(void)
{
	if (!plv8_in_js_execution || current_isolate == nullptr)
		return;

	if (InterruptPending || QueryCancelPending || ProcDiePending)
	{
		if (current_context != nullptr)
			current_context->interrupted = true;
		current_isolate->TerminateExecution();
		disable_timeout(plv8_timeout_id, false);
		return;
	}

	if (plv8_deadline_tz > 0 && GetCurrentTimestamp() >= plv8_deadline_tz)
	{
		plv8_timeout_expired = 1;
		current_isolate->TerminateExecution();
		disable_timeout(plv8_timeout_id, false);
	}
}

static void
plv8_ensure_timeout_registered(void)
{
	if (!plv8_timeout_registered)
	{
		plv8_timeout_id = RegisterTimeout(USER_TIMEOUT, plv8_timeout_handler);
		plv8_timeout_registered = true;
	}
}

class Plv8ExecutionTimeoutGuard
{
private:
	bool outermost_;

public:
	Plv8ExecutionTimeoutGuard()
	{
		outermost_ = (plv8_in_js_execution == 0);
		if (outermost_)
		{
			plv8_ensure_timeout_registered();
			plv8_timeout_expired = 0;
			int timeout_sec = plv8_execution_timeout > 0 ? plv8_execution_timeout : 300;
			plv8_deadline_tz = TimestampTzPlusMilliseconds(
				GetCurrentTimestamp(), (int64) timeout_sec * 1000LL);
			plv8_in_js_execution = 1;
			enable_timeout_every(plv8_timeout_id,
								 TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 25),
								 25);
		}
	}

	~Plv8ExecutionTimeoutGuard()
	{
		if (outermost_)
		{
			plv8_in_js_execution = 0;
			disable_timeout(plv8_timeout_id, false);
		}
	}
};

/*
 * DoCall -- Call a JS function with SPI support.
 *
 * This function could throw C++ exceptions, but must not throw PG exceptions.
 */
static Local<v8::Value>
DoCall(Local<Context> ctx, Handle<Function> fn, Handle<Object> receiver,
	int nargs, Handle<v8::Value> args[], bool nonatomic)
{
	Isolate 	   *isolate = Isolate::GetCurrent();
	TryCatch		try_catch(isolate);

	if (isolate->IsExecutionTerminating() || current_context->interrupted) {
		isolate->CancelTerminateExecution();
		if (current_context->interrupted) {
			current_context->interrupted = false;
		}
	}
	if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT)
		throw js_error("could not connect to SPI manager");

	MaybeLocal<v8::Value> result;
	{
		Plv8ExecutionTimeoutGuard timeout_guard;
		result = fn->Call(ctx, receiver, nargs, args);
		if (!result.IsEmpty() && current_context && current_context->microtask_queue)
		{
			current_context->microtask_queue->PerformCheckpoint(isolate);
		}
	}

	int	status = SPI_finish();

	HandleUnhandledPromiseRejections();

	if (result.IsEmpty()) {
		bool was_interrupted = (current_context && current_context->interrupted) ||
							   QueryCancelPending || ProcDiePending;
		bool was_timeout = (plv8_timeout_expired != 0);

		if (isolate->IsExecutionTerminating() || was_interrupted || was_timeout) {
			isolate->CancelTerminateExecution();
			if (current_context)
				current_context->interrupted = false;
			plv8_timeout_expired = 0;

			if (was_interrupted) {
				PG_TRY();
				{
					CHECK_FOR_INTERRUPTS();
				}
				PG_CATCH();
				{
					throw pg_error();
				}
				PG_END_TRY();
				throw js_error("Signal caught: interrupted");
			}
			if (was_timeout) {
				throw js_error("execution timeout exceeded");
			}
			if (current_context)
				current_context->is_dead = true;
			plv8_isolate_oom_killed = true;
			throw js_error("Out of memory error");
		}
		if (plv8_isolate_oom_killed ||
			(current_allocator && current_allocator->hasOOM()))
		{
			if (current_context)
				current_context->is_dead = true;
			plv8_isolate_oom_killed = true;
			if (try_catch.HasCaught())
			{
				throw js_error(try_catch);
			}
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
	Local<Context>		context = xenv->localContext();
	Context::Scope		context_scope(context);
	Handle<v8::Value>	args[FUNC_MAX_ARGS];

	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

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
			args[i] = ToValue(fcinfo->args[i].value, fcinfo->args[i].isnull, &argtypes[i]);
		}
	}

	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn = recv->GetInternalField(0).As<Function>();
	
	Local<v8::Value> result =
		DoCall(context, fn, recv, nargs, args, nonatomic);

	Oid retoid = rettype ? rettype->typid : VOIDOID;
	if (retoid == RECORDOID)
	{
		Oid calltype;
		TupleDesc tupdesc;
		plv8_type type;

		PG_TRY();
		{
			get_call_result_type(fcinfo, &calltype, &tupdesc);
			if (tupdesc != NULL)
				plv8_fill_type(&type, calltype);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();

		if (tupdesc == NULL)
			return ToDatum(result, &fcinfo->isnull, rettype);

		return ToRecordDatum(result, &fcinfo->isnull, &type, tupdesc);
	}
	else
	{
		if (rettype)
			return ToDatum(result, &fcinfo->isnull, rettype);
		else
			PG_RETURN_VOID();
	}
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

	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	tupstore = CreateTupleStore(fcinfo, &tupdesc);

	Handle<Context>		context = xenv->localContext();
	Context::Scope		context_scope(context);
	Converter			conv(tupdesc, proc->functypclass == TYPEFUNC_SCALAR);
	Handle<v8::Value>	args[FUNC_MAX_ARGS + 1];

	/*
	 * In case this is nested via SPI, stash pre-registered converters
	 * for the previous SRF.
	 */
	SRFSupport support(context, &conv, tupstore);

	for (int i = 0; i < nargs; i++) {
		args[i] = ToValue(fcinfo->args[i].value, fcinfo->args[i].isnull, &argtypes[i]);
	}

	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn = recv->GetInternalField(0).As<Function>();

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

	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	Handle<Context>		context = xenv->localContext();
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
		args[3] = v8::String::NewFromUtf8Literal(xenv->isolate, "BEFORE");
	else
		args[3] = v8::String::NewFromUtf8Literal(xenv->isolate, "AFTER");

	// 4: TG_LEVEL
	if (TRIGGER_FIRED_FOR_ROW(event))
		args[4] = v8::String::NewFromUtf8Literal(xenv->isolate, "ROW");
	else
		args[4] = v8::String::NewFromUtf8Literal(xenv->isolate, "STATEMENT");

	// 5: TG_OP
	if (TRIGGER_FIRED_BY_INSERT(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "UPDATE");
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "TRUNCATE");
#endif
	else
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "?");

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
	Local<Function>		fn = recv->GetInternalField(0).As<Function>();
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

Datum
plv8_call_validator(PG_FUNCTION_ARGS)
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
	/* except for TRIGGER, RECORD, INTERNAL, VOID, LANGUAGE_HANDLER or polymorphic types */
	if (functyptype == TYPTYPE_PSEUDO)
	{
		if (proc->prorettype == TRIGGEROID)
			is_trigger = true;
		else if (proc->prorettype != RECORDOID &&
			proc->prorettype != VOIDOID &&
			proc->prorettype != INTERNALOID &&
			proc->prorettype != LANGUAGE_HANDLEROID &&
			!IsPolymorphicType(proc->prorettype))
		{
			Oid prorettype = proc->prorettype;
			ReleaseSysCache(tuple);
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/v8 functions cannot return type %s",
						format_type_be(prorettype))));
		}
	}

	ReleaseSysCache(tuple);

	/*
	 * Invalidate any cached entry for this function or any downstream
	 * functions whose language handler chain includes this function.
	 */
	plv8_cache_function_remove(fn_oid);

	if (!check_function_bodies)
		PG_RETURN_VOID();

	try
	{
		plv8_context   *context = GetPlv8Context();
		CurrentContextScope	context_scope(context);
		Isolate::Scope  scope(context->isolate);
		HandleScope		handle_scope(context->isolate);

		/* Don't use validator's fcinfo */
		plv8_proc	   *compiled = Compile(fn_oid, NULL,
										   true, is_trigger);
		(void) CreateExecEnv(compiled->cache->function, context);
		/* the result of a validator is ignored */
		PG_RETURN_VOID();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

static void
plv8_cache_function_remove(Oid fn_oid)
{
	HASH_SEQ_STATUS		status;
	plv8_proc_cache	   *cache;

	if (!plv8_proc_cache_hash || !OidIsValid(fn_oid))
		return;

	hash_seq_init(&status, plv8_proc_cache_hash);
	while ((cache = (plv8_proc_cache *) hash_seq_search(&status)) != nullptr)
	{
		bool remove = (cache->fn_oid == fn_oid);

		if (!remove)
		{
			for (int i = 0; i < cache->nhandlers; i++)
			{
				if (cache->handlers[i].fn_oid == fn_oid)
				{
					remove = true;
					break;
				}
			}
		}

		if (remove)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Reset();
			cache->nhandlers = 0;
		}
	}
}

static bool
plv8_cache_handlers_valid(plv8_proc_cache *cache, HeapTuple proctuple, Oid *stale_handler_oid)
{
	if (stale_handler_oid != NULL)
		*stale_handler_oid = InvalidOid;

	if (cache == NULL)
		return false;

	if (cache->nhandlers == 0)
		return true;

	Oid lang_oid = InvalidOid;
	if (proctuple != NULL)
	{
		Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(proctuple);
		lang_oid = procStruct->prolang;
	}
	else
	{
		HeapTuple fnTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(cache->fn_oid));
		if (!HeapTupleIsValid(fnTup))
			return false;
		if (cache->fn_xmin != HeapTupleHeaderGetXmin(fnTup->t_data) ||
			!ItemPointerEquals(&cache->fn_tid, &fnTup->t_self))
		{
			ReleaseSysCache(fnTup);
			return false;
		}
		Form_pg_proc fnStruct = (Form_pg_proc) GETSTRUCT(fnTup);
		lang_oid = fnStruct->prolang;
		ReleaseSysCache(fnTup);
	}

	NameData plv8_lang_name = {"plv8"};
	HeapTuple plv8Tup = SearchSysCache1(LANGNAME, NameGetDatum(&plv8_lang_name));
	if (!HeapTupleIsValid(plv8Tup))
		return false;
	Oid plv8_lang_oid = ((Form_pg_language) GETSTRUCT(plv8Tup))->oid;
	ReleaseSysCache(plv8Tup);

	for (int i = 0; i < cache->nhandlers; i++)
	{
		if (!OidIsValid(lang_oid) || lang_oid == plv8_lang_oid)
			return false;

		HeapTuple langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(lang_oid));
		if (!HeapTupleIsValid(langTup))
			return false;
		Form_pg_language langStruct = (Form_pg_language) GETSTRUCT(langTup);
		Oid handler_oid = langStruct->lanplcallfoid;
		ReleaseSysCache(langTup);

		if (handler_oid != cache->handlers[i].fn_oid)
		{
			if (stale_handler_oid != NULL)
				*stale_handler_oid = cache->handlers[i].fn_oid;
			return false;
		}

		HeapTuple handlerTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
		if (!HeapTupleIsValid(handlerTup))
		{
			if (stale_handler_oid != NULL)
				*stale_handler_oid = handler_oid;
			return false;
		}

		bool match = (cache->handlers[i].fn_xmin == HeapTupleHeaderGetXmin(handlerTup->t_data) &&
					  ItemPointerEquals(&cache->handlers[i].fn_tid, &handlerTup->t_self));
		Form_pg_proc handlerStruct = (Form_pg_proc) GETSTRUCT(handlerTup);
		lang_oid = handlerStruct->prolang;
		ReleaseSysCache(handlerTup);

		if (!match)
		{
			if (stale_handler_oid != NULL)
				*stale_handler_oid = handler_oid;
			return false;
		}
	}

	return (lang_oid == plv8_lang_oid);
}

static bool
plv8_is_js_language(Oid lang_oid)
{
	if (!OidIsValid(lang_oid))
		return false;

	NameData plv8_lang_name = {"plv8"};
	HeapTuple tuple = SearchSysCache1(LANGNAME, NameGetDatum(&plv8_lang_name));
	if (!HeapTupleIsValid(tuple))
		return false;
	Oid plv8_lang_oid = ((Form_pg_language) GETSTRUCT(tuple))->oid;
	ReleaseSysCache(tuple);

	Oid cur_lang_oid = lang_oid;
	for (int depth = 0; depth < PLV8_MAX_LANG_HANDLER_DEPTH; depth++)
	{
		if (cur_lang_oid == plv8_lang_oid)
			return true;

		tuple = SearchSysCache1(LANGOID, ObjectIdGetDatum(cur_lang_oid));
		if (!HeapTupleIsValid(tuple))
			return false;
		Form_pg_language langStruct = (Form_pg_language) GETSTRUCT(tuple);
		Oid handler_oid = langStruct->lanplcallfoid;
		ReleaseSysCache(tuple);

		if (!OidIsValid(handler_oid))
			return false;

		tuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
		if (!HeapTupleIsValid(tuple))
			return false;
		Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(tuple);
		cur_lang_oid = procStruct->prolang;
		ReleaseSysCache(tuple);

		if (!OidIsValid(cur_lang_oid))
			return false;
	}

	return false;
}

/*
 * Transpile source code from custom language to JS if needed.
 *
 * When lang_oid is not plv8, looks up the language's call handler chain,
 * records the catalog tuple identities in deps/ndeps, executes the handler
 * function in V8 with the raw source string as arguments[0], and returns a
 * palloc'd C string in CurrentMemoryContext containing the transpiled JS code.
 */
static char *
plv8_transpile_src(const char *src, Oid lang_oid, plv8_handler_dep *deps, int *ndeps, plv8_proc *proc)
{
	Oid				plv8_lang_oid = InvalidOid;
	Oid				lang_handler_oid = InvalidOid;
	char		   *result_cstr = NULL;

	if (ndeps != NULL)
		*ndeps = 0;

	PG_TRY();
	{
		NameData plv8_lang_name = {"plv8"};
		HeapTuple plv8Tup = SearchSysCache1(LANGNAME, NameGetDatum(&plv8_lang_name));
		if (HeapTupleIsValid(plv8Tup))
		{
			plv8_lang_oid = ((Form_pg_language) GETSTRUCT(plv8Tup))->oid;
			ReleaseSysCache(plv8Tup);
		}

		if (OidIsValid(plv8_lang_oid) && lang_oid == plv8_lang_oid)
		{
			result_cstr = pstrdup(src);
		}
		else
		{
			Oid cur_lang_oid = lang_oid;
			int depth = 0;

			while (!OidIsValid(plv8_lang_oid) || cur_lang_oid != plv8_lang_oid)
			{
				if (depth >= PLV8_MAX_LANG_HANDLER_DEPTH)
					ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("language handler chain depth limit (%d) exceeded",
								PLV8_MAX_LANG_HANDLER_DEPTH)));

				HeapTuple langTup = SearchSysCache1(LANGOID, ObjectIdGetDatum(cur_lang_oid));
				if (!HeapTupleIsValid(langTup))
					elog(ERROR, "cache lookup failed for language %u", cur_lang_oid);
				Form_pg_language langStruct = (Form_pg_language) GETSTRUCT(langTup);
				Oid handler_oid = langStruct->lanplcallfoid;
				ReleaseSysCache(langTup);

				if (!OidIsValid(handler_oid))
					elog(ERROR, "javascript language handler function is not found for language %u", lang_oid);

				if (depth == 0)
					lang_handler_oid = handler_oid;

				HeapTuple procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(handler_oid));
				if (!HeapTupleIsValid(procTup))
					elog(ERROR, "cache lookup failed for function %u", handler_oid);
				TransactionId handler_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
				ItemPointerData handler_tid = procTup->t_self;
				Form_pg_proc procStruct = (Form_pg_proc) GETSTRUCT(procTup);
				cur_lang_oid = procStruct->prolang;
				ReleaseSysCache(procTup);

				if (deps != NULL && ndeps != NULL)
				{
					deps[depth].fn_oid = handler_oid;
					deps[depth].fn_xmin = handler_xmin;
					deps[depth].fn_tid = handler_tid;
					(*ndeps) = depth + 1;
				}
				depth++;
			}
		}
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	if (result_cstr != NULL)
		return result_cstr;

	Isolate *isolate = current_context->isolate;
	Isolate::Scope iscope(isolate);
	HandleScope handle_scope(isolate);
	Local<Context> context = current_context->localContext();
	Context::Scope context_scope(context);
	HandlerExecutionScope handler_scope(context);

	Local<Function> fn = find_js_function(lang_handler_oid);
	if (fn.IsEmpty())
	{
		PG_TRY();
		{
			elog(ERROR, "javascript language handler function %u is not found", lang_handler_oid);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();
	}

	plv8_exec_env *xenv = CreateExecEnv(fn, current_context);
	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<v8::Value> args[2];
	args[0] = ToString(src);
	int call_argc = 1;
	if (proc != NULL && proc->cache != NULL)
	{
		plv8_proc_cache *cache = proc->cache;
		Local<Object> meta = Object::New(isolate);
		meta->Set(context, ToString("fn_oid"), Uint32::NewFromUnsigned(isolate, cache->fn_oid)).Check();
		meta->Set(context, ToString("proname"), ToString(cache->proname)).Check();
		meta->Set(context, ToString("rettype_oid"), Uint32::NewFromUnsigned(isolate, proc->rettype.typid)).Check();
		char ret_cat_str[2] = { proc->rettype.category, '\0' };
		meta->Set(context, ToString("rettype_category"), ToString(ret_cat_str)).Check();
		int32 ret_internal_len = (proc->rettype.len > (int16) VARHDRSZ)
			? (int32) (proc->rettype.len - VARHDRSZ)
			: (int32) proc->rettype.len;
		meta->Set(context, ToString("rettype_len"), Int32::New(isolate, ret_internal_len)).Check();

		Local<Array> arg_oids = Array::New(isolate, cache->nargs);
		Local<Array> arg_cats = Array::New(isolate, cache->nargs);
		Local<Array> arg_lens = Array::New(isolate, cache->nargs);
		for (int a = 0; a < cache->nargs; a++)
		{
			arg_oids->Set(context, a, Uint32::NewFromUnsigned(isolate, proc->argtypes[a].typid)).Check();
			char cat_str[2] = { proc->argtypes[a].category, '\0' };
			arg_cats->Set(context, a, ToString(cat_str)).Check();
			int32 a_len = (proc->argtypes[a].len > (int16) VARHDRSZ)
				? (int32) (proc->argtypes[a].len - VARHDRSZ)
				: (int32) proc->argtypes[a].len;
			arg_lens->Set(context, a, Int32::New(isolate, a_len)).Check();
		}
		meta->Set(context, ToString("argtype_oids"), arg_oids).Check();
		meta->Set(context, ToString("argtype_categories"), arg_cats).Check();
		meta->Set(context, ToString("argtype_lens"), arg_lens).Check();
		args[1] = meta;
		call_argc = 2;
	}

	Local<v8::Value> transpiled = DoCall(context, fn, recv, call_argc, args, false);
	if (transpiled.IsEmpty() || transpiled->IsNull() || transpiled->IsUndefined())
		throw js_error("language handler returned null or undefined source code");
	if (!transpiled->IsString())
	{
		PG_TRY();
		{
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("language handler function %u did not return a string",
						lang_handler_oid)));
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();
	}

	CString str(transpiled);
	PG_TRY();
	{
		result_cstr = pstrdup(str.str());
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return result_cstr;
}

static plv8_proc *
plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo, bool validate,
			  char ***argnames, Oid *prolang_out, char **raw_prosrc_out) throw()
{
	HeapTuple			procTup;
	plv8_proc_cache	   *cache;
	bool				found;
	bool				isnull;
	Datum				prosrc;
	Oid				   *argtypes;
	char			   *argmodes;

	if (prolang_out != NULL)
		*prolang_out = InvalidOid;
	if (raw_prosrc_out != NULL)
		*raw_prosrc_out = NULL;

	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	cache = (plv8_proc_cache *)
		hash_search(plv8_proc_cache_hash, &fn_oid, HASH_ENTER, &found);

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
			ItemPointerEquals(&cache->fn_tid, &procTup->t_self) &&
			cache->user_id == current_context->user_id);

		if (uptodate && cache->nhandlers > 0)
		{
			Oid stale_handler_oid = InvalidOid;
			if (!plv8_cache_handlers_valid(cache, procTup, &stale_handler_oid))
			{
				uptodate = false;
				if (OidIsValid(stale_handler_oid))
					plv8_cache_function_remove(stale_handler_oid);
			}
		}

		if (!uptodate)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Reset();
			cache->nhandlers = 0;
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
		cache->nhandlers = 0;
	}

	if (cache->function.IsEmpty())
	{
		Form_pg_proc	procStruct;

		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		prosrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
		{
			ReleaseSysCache(procTup);
			elog(ERROR, "null prosrc");
		}

		cache->retset = procStruct->proretset;
		cache->rettype = procStruct->prorettype;

		strlcpy(cache->proname, NameStr(procStruct->proname), NAMEDATALEN);
		cache->fn_xmin = HeapTupleHeaderGetXmin(procTup->t_data);
		cache->fn_tid = procTup->t_self;
		cache->user_id = current_context->user_id;
		if (prolang_out != NULL)
			*prolang_out = procStruct->prolang;

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
				{
					Oid bad_argtype = argtypes[i];
					ReleaseSysCache(procTup);
					ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/v8 functions cannot accept type %s",
								format_type_be(bad_argtype))));
				}
			}
		}

		if (raw_prosrc_out != NULL)
			*raw_prosrc_out = TextDatumGetCString(prosrc);

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
CreateExecEnv(Persistent<Function>& function, plv8_context *context)
{
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope(context->isolate);

	PG_TRY();
	{
		xenv = plv8_new_exec_env(context);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	xenv->context.Reset(context->isolate, context->context);
	Local<Context>		ctx = xenv->localContext();
	Context::Scope		scope(ctx);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(context->isolate, context->recv_templ);
	Local<Object> obj = templ->NewInstance(ctx).ToLocalChecked();
	Local<Function> f = Local<Function>::New(context->isolate, function);
	obj->SetInternalField(0, f);
	xenv->recv.Reset(context->isolate, obj);


	return xenv;
}

static plv8_exec_env *
CreateExecEnv(Handle<Function> function, plv8_context *context)
{
	Isolate::Scope	    iscope(context->isolate);
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope(context->isolate);

	PG_TRY();
	{
		xenv = plv8_new_exec_env(context);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	xenv->context.Reset(context->isolate, context->context);
	Local<Context>		ctx = xenv->localContext();
	Context::Scope		scope(ctx);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(context->isolate, context->recv_templ);
	Local<Object> obj = templ->NewInstance(ctx).ToLocalChecked();
	Local<Function> f = Local<Function>::New(context->isolate, function);
	obj->SetInternalField(0, f);
	xenv->recv.Reset(context->isolate, obj);


	return xenv;
}

static __attribute__((noinline)) bool
plv8_check_snapshot_handlers_valid(plv8_proc_cache *cache)
{
	bool valid_handlers = false;
	PG_TRY();
	{
		valid_handlers = plv8_cache_handlers_valid(cache, NULL, NULL);
	}
	PG_CATCH();
	{
		cache->nhandlers = 0;
		throw pg_error();
	}
	PG_END_TRY();
	return valid_handlers;
}

static __attribute__((noinline)) void
plv8_replace_snapshot_prosrc(plv8_proc_cache *cache, const char *raw_prosrc)
{
	PG_TRY();
	{
		if (cache->prosrc)
		{
			pfree(cache->prosrc);
			cache->prosrc = NULL;
		}
		if (raw_prosrc != NULL)
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
			cache->prosrc = pstrdup(raw_prosrc);
			MemoryContextSwitchTo(oldcontext);
		}
	}
	PG_CATCH();
	{
		cache->function.Reset();
		cache->nhandlers = 0;
		throw pg_error();
	}
	PG_END_TRY();
}

static __attribute__((noinline)) bool
plv8_try_restore_function_from_snapshot(plv8_proc_cache *cache,
										plv8_context *global_context,
										const char *raw_prosrc)
{
	if (plv8_bypass_snapshot_restore ||
		plv8_active_snapshot_blob.data == nullptr ||
		plv8_active_snapshot_blob.raw_size <= 0 ||
		plv8_snapshot_slots.empty())
	{
		return false;
	}

	for (size_t idx = 0; idx < plv8_snapshot_slots.size(); idx++)
	{
		const Plv8SnapshotSlot &slot = plv8_snapshot_slots[idx];
		if (slot.fn_oid != cache->fn_oid)
			continue;

		if (!TransactionIdEquals(slot.fn_xmin, cache->fn_xmin) ||
			!ItemPointerEquals(const_cast<ItemPointer>(&slot.fn_tid), &cache->fn_tid))
		{
			return false;
		}

		if (slot.nhandlers > 0)
		{
			cache->nhandlers = slot.nhandlers;
			memcpy(cache->handlers, slot.handlers,
				   sizeof(plv8_handler_dep) * slot.nhandlers);
			if (!plv8_check_snapshot_handlers_valid(cache))
			{
				cache->nhandlers = 0;
				return false;
			}
		}

		{
			Isolate *isolate = global_context->isolate;
			Isolate::Scope scope(isolate);
			HandleScope handle_scope(isolate);
			Local<Context> context = Local<Context>::New(isolate, global_context->context);
			Context::Scope context_scope(context);

			char prop_buf[64];
			snprintf(prop_buf, sizeof(prop_buf), "__plv8_snap_fn_%zu", idx);
			Local<v8::String> prop_name = v8::String::NewFromUtf8(
				isolate, prop_buf, v8::NewStringType::kInternalized).ToLocalChecked();
			Local<v8::Value> fn_val;
			if (!context->Global()->Get(context, prop_name).ToLocal(&fn_val) ||
				fn_val.IsEmpty() || !fn_val->IsFunction())
			{
				cache->nhandlers = 0;
				return false;
			}

			cache->function.Reset(isolate, Local<Function>::Cast(fn_val));
		}

		plv8_replace_snapshot_prosrc(cache, raw_prosrc);
		plv8_snapshot_functions_restored++;
		return true;
	}

	return false;
}

/*
 * fcinfo should be passed if this is an actual function call context, where
 * we can resolve polymorphic types and use function's memory context.
 */
static plv8_proc *
Compile(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, bool is_trigger)
{
	plv8_proc  *proc;
	char	  **argnames = NULL;
	Oid			prolang = InvalidOid;
	char	   *raw_prosrc = NULL;

	PG_TRY();
	{
		proc = plv8_get_proc(fn_oid, fcinfo, validate, &argnames, &prolang, &raw_prosrc);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	plv8_proc_cache *cache = proc->cache;

	if (cache->function.IsEmpty())
	{
		if (plv8_try_restore_function_from_snapshot(cache, current_context, raw_prosrc))
		{
			return proc;
		}

		plv8_handler_dep	deps[PLV8_MAX_LANG_HANDLER_DEPTH];
		int					ndeps = 0;
		char			   *transpiled = plv8_transpile_src(raw_prosrc, prolang, deps, &ndeps, proc);

		/*
		 * Compile into current_context, which the caller has already set
		 * up (and whose startup procedure, if any, has already run): the
		 * call handler and the validator point it at the effective user's
		 * context, and find_js_function() is invoked while that context
		 * is executing.  Looking the context up again here by user id
		 * would compile into the wrong isolate when they differ, and
		 * could re-enter Compile() through the startup procedure while
		 * this cache entry is still being filled in.
		 */
		Isolate::Scope	scope(current_context->isolate);
		HandleScope		handle_scope(current_context->isolate);
		Local<Function>	fn = CompileFunction(
						current_context,
						cache->proname,
						cache->nargs,
						(const char **) argnames,
						transpiled,
						is_trigger,
						cache->retset);

		if (cache->prosrc)
		{
			pfree(cache->prosrc);
			cache->prosrc = NULL;
		}
		cache->function.Reset(current_context->isolate, fn);
		PG_TRY();
		{
			MemoryContext oldcontext = MemoryContextSwitchTo(TopMemoryContext);
			cache->prosrc = pstrdup(transpiled);
			MemoryContextSwitchTo(oldcontext);
		}
		PG_CATCH();
		{
			cache->function.Reset();
			throw pg_error();
		}
		PG_END_TRY();
		cache->nhandlers = ndeps;
		if (ndeps > 0)
			memcpy(cache->handlers, deps, sizeof(plv8_handler_dep) * ndeps);
	}

	return proc;
}

static Local<Function>
CompileFunction(
	plv8_context *global_context,
	const char *proname,
	int proarglen,
	const char *proargs[],
	const char *prosrc,
	bool is_trigger,
	bool retset)
{
	Isolate					   *isolate = Isolate::GetCurrent();
	EscapableHandleScope		handle_scope(isolate);
	StringInfoData	src;

	initStringInfo(&src);

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

	appendStringInfo(&src, "){\n%s\n})", prosrc);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined(isolate);
	Local<v8::String> source = ToString(src.data, src.len);
	pfree(src.data);

	Local<Context> context = Local<Context>::New(isolate, global_context->context);
	Context::Scope	context_scope(context);
	TryCatch		try_catch(isolate);
	v8::ScriptOrigin origin(name);

	v8::Local<v8::Script> script;
	v8::Local<v8::Value> result;
	if (current_context->interrupted) {
		isolate->CancelTerminateExecution();
		current_context->interrupted = false;
	}
	{
		Plv8ExecutionTimeoutGuard timeout_guard;
		if (Script::Compile(isolate->GetCurrentContext(), source, &origin).ToLocal(&script)) {
			if (!script.IsEmpty()) {
				if (!script->Run(isolate->GetCurrentContext()).ToLocal(&result)) {}
			}
		}
	}

	HandleUnhandledPromiseRejections();

	if (result.IsEmpty()) {
		bool was_interrupted = (current_context && current_context->interrupted) ||
							   QueryCancelPending || ProcDiePending;
		bool was_timeout = (plv8_timeout_expired != 0);

		if (isolate->IsExecutionTerminating() || was_interrupted || was_timeout) {
			isolate->CancelTerminateExecution();
			if (current_context)
				current_context->interrupted = false;
			plv8_timeout_expired = 0;

			if (was_interrupted) {
				PG_TRY();
				{
					CHECK_FOR_INTERRUPTS();
				}
				PG_CATCH();
				{
					throw pg_error();
				}
				PG_END_TRY();
				throw js_error("Signal caught: interrupted");
			}
			if (was_timeout) {
				throw js_error("compiler timeout exceeded");
			}
			if (current_context)
				current_context->is_dead = true;
			plv8_isolate_oom_killed = true;
			throw js_error("Script is out of memory");
		}
		if (plv8_isolate_oom_killed ||
			(current_allocator && current_allocator->hasOOM()))
		{
			if (current_context)
				current_context->is_dead = true;
			plv8_isolate_oom_killed = true;
			if (try_catch.HasCaught())
			{
				throw js_error(try_catch);
			}
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
	Form_pg_proc	proc;
	Oid				prolang = InvalidOid;
	Local<Function> func;
	Isolate			*isolate = Isolate::GetCurrent();
	bool			is_js_lang = false;

	PG_TRY();
	{
		tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for function %u", fn_oid);
		proc = (Form_pg_proc) GETSTRUCT(tuple);
		prolang = proc->prolang;
		ReleaseSysCache(tuple);

		is_js_lang = plv8_is_js_language(prolang);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	/* Not found or non-JS function */
	if (!is_js_lang)
		return func;

	plv8_proc *compiled_proc = Compile(fn_oid, NULL, true, false);
	func = Local<Function>::New(isolate, compiled_proc->cache->function);

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

static plv8_context*
GetPlv8Context() {
	Oid					user_id = GetUserId();
	unsigned int		i;
	plv8_context		*my_context = nullptr;

	if (plv8_isolate_oom_killed ||
		(current_isolate && current_isolate->IsDead()) ||
		(current_allocator && current_allocator->hasOOM()))
	{
		KillIsolateAndAllContexts();
	}

	for (i = 0; i < ContextVector.size(); i++)
	{
		if (ContextVector[i]->is_dead)
		{
			KillIsolateAndAllContexts();
			my_context = nullptr;
			break;
		}
		if (ContextVector[i]->user_id == user_id)
		{
			my_context = ContextVector[i];
			break;
		}
	}

	if (!my_context)
	{
		EnsureSharedIsolate();
		Isolate 			   *isolate = current_isolate;
		Isolate::Scope			scope(isolate);
		HandleScope				handle_scope(isolate);

		my_context = (plv8_context *) MemoryContextAllocZero(TopMemoryContext,
														 sizeof(plv8_context));
		my_context->isolate = isolate;
		my_context->is_dead = false;
		my_context->interrupted = false;
		my_context->ignore_unhandled_promises = false;
		new(&my_context->unhandled_promises) std::vector<std::tuple<v8::Global<v8::Promise>, v8::Global<v8::Message>, v8::Global<v8::Value>>>();
		my_context->microtask_queue =
			v8::MicrotaskQueue::New(isolate, v8::MicrotasksPolicy::kAuto);

		Local<Context> role_ctx;
		if (plv8_active_snapshot_blob.data != nullptr &&
			plv8_active_snapshot_blob.raw_size > 0)
		{
			MaybeLocal<Context> snap_ctx = Context::FromSnapshot(
				isolate, 0, v8::DeserializeInternalFieldsCallback(),
				nullptr, MaybeLocal<v8::Value>(), my_context->microtask_queue);
			if (!snap_ctx.IsEmpty())
				role_ctx = snap_ctx.ToLocalChecked();
		}
		if (role_ctx.IsEmpty())
		{
			Local<ObjectTemplate> global = Local<ObjectTemplate>::New(
				isolate, GetGlobalObjectTemplate(isolate));
			role_ctx = Context::New(
				isolate, NULL, global, MaybeLocal<v8::Value>(),
				v8::DeserializeInternalFieldsCallback(),
				my_context->microtask_queue);
		}

		{
			Context::Scope role_cs(role_ctx);
			char sec_buf[64];
			snprintf(sec_buf, sizeof(sec_buf), "plv8_role_%u", (unsigned int) user_id);
			Local<v8::String> sec_tok = v8::String::NewFromUtf8(
				isolate, sec_buf, NewStringType::kInternalized).ToLocalChecked();
			role_ctx->SetSecurityToken(sec_tok);
		}

		new(&my_context->context) Persistent<Context>();
		my_context->context.Reset(isolate, role_ctx);
		my_context->user_id = user_id;
		my_context->id = next_context_id++;

		new(&my_context->recv_templ) Persistent<ObjectTemplate>();
		Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
		templ->SetInternalFieldCount(1);
		my_context->recv_templ.Reset(isolate, templ);

		new(&my_context->compile_context) Persistent<Context>();
		Local<Context> ctx = Context::New(isolate, (ExtensionConfiguration*)NULL);
		my_context->compile_context.Reset(isolate, ctx);

		auto toStringAttr = static_cast<PropertyAttribute>(v8::ReadOnly | v8::DontEnum);
		auto toStringSymbol = v8::Symbol::GetToStringTag(isolate);

		new(&my_context->plan_template) Persistent<ObjectTemplate>();
		Local<FunctionTemplate> base = FunctionTemplate::New(isolate);
		Local<v8::String> planClassName = v8::String::NewFromUtf8Literal(isolate, "PreparedPlan",
														   NewStringType::kInternalized);
		base->SetClassName(planClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, planClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupPrepFunctions(templ);
		my_context->plan_template.Reset(isolate, templ);

		new(&my_context->cursor_template) Persistent<ObjectTemplate>();
		base = FunctionTemplate::New(isolate);
		Local<v8::String> cursorClassName = v8::String::NewFromUtf8Literal(isolate, "Cursor",
															 NewStringType::kInternalized);
		base->SetClassName(cursorClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, cursorClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupCursorFunctions(templ);
		my_context->cursor_template.Reset(isolate, templ);

		new(&my_context->window_template) Persistent<ObjectTemplate>();
		base = FunctionTemplate::New(isolate);
		Local<v8::String> windowClassName = v8::String::NewFromUtf8Literal(isolate, "WindowObject",
															 NewStringType::kInternalized);
        base->SetClassName(windowClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, windowClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupWindowFunctions(templ);
		my_context->window_template.Reset(isolate, templ);
		/*
		 * Need to register it before running any code, as the code
		 * recursively may want to the global context.
		 */
		ContextVector.push_back(my_context);

		/*
		 * Run the start up procedure if configured.
		 */
		if (plv8_start_proc != NULL)
		{
			Local<Function>		func;
			Oid					funcoid = InvalidOid;
			bool				has_priv = false;

			CurrentContextScope	current_scope(my_context);
			HandleScope			handle_scope(isolate);
			Local<Context>		context = my_context->localContext();
			Context::Scope		context_scope(context);
			TryCatch			try_catch(isolate);
			MemoryContext		ctx = CurrentMemoryContext;
			text *arg;
			// Stack-allocate FunctionCallInfoBaseData with
			// space for 2 arguments:
			LOCAL_FCINFO(fake_fcinfo, 2);
			FmgrInfo	flinfo;

			char perm[16];
			strcpy(perm, "EXECUTE");
			arg = charToText(perm);
			PG_TRY();
			{
				funcoid = DatumGetObjectId(DirectFunctionCall1(regprocin, CStringGetDatum(plv8_start_proc)));
				MemSet(&flinfo, 0, sizeof(flinfo));
				fake_fcinfo->flinfo = &flinfo;
				flinfo.fn_oid = InvalidOid;
				flinfo.fn_mcxt = CurrentMemoryContext;
				fake_fcinfo->nargs = 2;
				fake_fcinfo->args[0].value = ObjectIdGetDatum(funcoid);
				fake_fcinfo->args[1].value = PointerGetDatum(arg);
				Datum ret = has_function_privilege_id(fake_fcinfo);

				if (ret == 0) {
					elog(WARNING, "failed to find js function %s", plv8_start_proc);
				} else {
					if (DatumGetBool(ret)) {
						has_priv = true;
					} else {
						elog(WARNING, "no permission to execute js function %s", plv8_start_proc);
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

			if (has_priv)
			{
				try
				{
					func = find_js_function(funcoid);
				}
				catch (js_error& e)
				{
					e.log(WARNING);
				}
				catch (pg_error& e)
				{
					ErrorData *edata;

					MemoryContextSwitchTo(ctx);
					edata = CopyErrorData();
					elog(WARNING, "failed to find js function %s", edata->message);
					FlushErrorState();
					FreeErrorData(edata);
				}
			}

			if (!func.IsEmpty())
			{
				Handle<v8::Value>	result =
						DoCall(context, func, my_context->localContext()->Global(), 0, NULL, false);
				if (result.IsEmpty())
					throw js_error(try_catch);
			}
		}
	}
	return my_context;
}

static Local<ObjectTemplate>
GetGlobalObjectTemplate(Isolate *isolate)
{
	EscapableHandleScope	handle_scope(isolate);
	Local<ObjectTemplate>	templ = ObjectTemplate::New(isolate);

	// ERROR levels for elog
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG5", NewStringType::kInternalized),
			   Int32::New(isolate, DEBUG5));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG4", NewStringType::kInternalized),
			   Int32::New(isolate, DEBUG4));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG3", NewStringType::kInternalized),
			   Int32::New(isolate, DEBUG3));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG2", NewStringType::kInternalized),
			   Int32::New(isolate, DEBUG2));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG1", NewStringType::kInternalized),
			   Int32::New(isolate, DEBUG1));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG", NewStringType::kInternalized),
			   Int32::New(isolate, DEBUG5));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "LOG", NewStringType::kInternalized),
			   Int32::New(isolate, LOG));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "INFO", NewStringType::kInternalized),
			   Int32::New(isolate, INFO));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "NOTICE", NewStringType::kInternalized),
			   Int32::New(isolate, NOTICE));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "WARNING", NewStringType::kInternalized),
			   Int32::New(isolate, WARNING));
	templ->Set(v8::String::NewFromUtf8Literal(isolate, "ERROR", NewStringType::kInternalized),
			   Int32::New(isolate, ERROR));

	Local<ObjectTemplate> plv8 = ObjectTemplate::New(isolate);

	SetupPlv8Functions(plv8);
	plv8->Set(v8::String::NewFromUtf8Literal(isolate, "version", NewStringType::kInternalized),
			  v8::String::NewFromUtf8Literal(isolate, PLV8_VERSION));
	plv8->Set(v8::String::NewFromUtf8Literal(isolate, "v8_version", NewStringType::kInternalized),
			  v8::String::NewFromUtf8Literal(isolate, V8_VERSION_STRING));

	templ->Set(v8::String::NewFromUtf8Literal(isolate, "plv8", NewStringType::kInternalized), plv8);

	return handle_scope.Escape(templ);
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
				m_memcontext = AllocSetContextCreate(
									CurrentMemoryContext,
									"ConverterContext",
									ALLOCSET_DEFAULT_SIZES);
			plv8_fill_type(&m_coltypes[c],
						   TupleDescAttr(m_tupdesc, c)->atttypid,
						   m_memcontext);
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

		datum = heap_getattr(tuple, c + 1, m_tupdesc, &isnull);

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
		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
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
	Local<Context>      context = isolate->GetCurrentContext();

	try
	{
		/*
		 * Converting the exception to a string may fail, e.g. when the
		 * JavaScript stack is exhausted (infinite recursion) and
		 * toString() cannot run, or when the exception is empty.  In
		 * that case Utf8Value yields NULL, so fall back to the message
		 * text V8 already formatted, and then to a generic message,
		 * rather than dereferencing NULL below.
		 */
		if (!exception.IsEmpty())
		{
			v8::String::Utf8Value	err_message(isolate, exception);
			m_msg = ToCStringCopy(err_message);
		}
		if (m_msg == NULL && !message.IsEmpty())
		{
			v8::String::Utf8Value	err_message(isolate, message->Get());
			m_msg = ToCStringCopy(err_message);
		}
		if (m_msg == NULL)
			m_msg = pstrdup("unknown exception");

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
                             v8::String::NewFromUtf8Literal(isolate, "code")).ToLocal(&errCode))
                {
                    if (!errCode->IsUndefined() && !errCode->IsNull())
                    {
                        int32_t code;
                        if (errCode->Int32Value(context).To(&code))
                            m_code = code;
                    }
                }

                v8::Local<v8::Value> errDetail;
                if (err->Get(context,
                             v8::String::NewFromUtf8Literal(isolate, "detail")).ToLocal(&errDetail))
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
                             v8::String::NewFromUtf8Literal(isolate, "hint")).ToLocal(&errHint))
                {
                    if (!errHint->IsUndefined() && !errHint->IsNull())
                    {
                        CString hint(errHint);
                        appendStringInfo(&hintStr, "%s", hint.str("?"));
                        m_hint = hintStr.data;
                    }
                }

                v8::Local<v8::Value> errContext;
                if (err->Get(context, v8::String::NewFromUtf8Literal(isolate, "context")).ToLocal(&errContext))
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
			int		lineno = message->GetLineNumber(context).FromMaybe(1);
			Local<v8::String>	sourceLine;
			/* leaves sourceLine empty on failure, printed as "?" */
			if (!message->GetSourceLine(context).ToLocal(&sourceLine)) {}
			CString		source(sourceLine);
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
	Local<v8::String> message = ToString(msg);
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
