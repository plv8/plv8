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
#include <sstream>
#include <thread>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


extern "C" {
#define String PG_Node_String
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_database.h"
#include "catalog/pg_language.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/supportnodes.h"
#include "parser/parse_func.h"
#include "parser/parse_relation.h"
#include "parser/parse_type.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/dsa.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/inval.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/rls.h"
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
PGDLLEXPORT Datum	plv8_modules_changed(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);
PG_FUNCTION_INFO_V1(plv8_reset);
PG_FUNCTION_INFO_V1(plv8_info);
PG_FUNCTION_INFO_V1(plv8_save_snapshot);
PG_FUNCTION_INFO_V1(plv8_modules_changed);


PGDLLEXPORT void _PG_init(void);

PGDLLEXPORT Datum	plv8_inline_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_source_in(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_source_out(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_base_type_in(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_base_type_out(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_call_support(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_inline_handler);
PG_FUNCTION_INFO_V1(plv8_source_in);
PG_FUNCTION_INFO_V1(plv8_source_out);
PG_FUNCTION_INFO_V1(plv8_base_type_in);
PG_FUNCTION_INFO_V1(plv8_base_type_out);
PG_FUNCTION_INFO_V1(plv8_call_support);
#undef String
} // extern "C"

static void plv8_reset_all_stamped_procs(void);
static void plv8_dispose_support_hooks_for_user(Oid user_id);

using namespace v8;

static pthread_t main_pg_thread_id;
static bool main_pg_thread_initialized = false;
static int plv8_max_module_depth = 32;
static int plv8_current_module_depth = 0;
static Oid plv8_cached_modules_relid = InvalidOid;
static Oid plv8_cached_packages_relid = InvalidOid;
static void plv8_modules_inval_cb(Datum arg, Oid relid);

struct Plv8ModuleDepthScope
{
	Plv8ModuleDepthScope() { plv8_current_module_depth++; }
	~Plv8ModuleDepthScope() { plv8_current_module_depth--; }
};

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
	uint64					tle_modules_fp;

	Oid						proowner;
	Oid						prolang;
	bool					prosecdef;

	int						nhandlers;
	plv8_handler_dep		handlers[PLV8_MAX_LANG_HANDLER_DEPTH];
	bool					pass_user_types_as_bytes;
	bool					preserve_numeric_precision;

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
bool plv8_allow_raw_user_types = false;
bool plv8_preserve_numeric_precision = false;
static uint64 next_context_id = 1;
size_t plv8_memory_limit = 256;
size_t plv8_last_heap_size = 0;
static int plv8_wasm_cache_size = 64;
static int plv8_wasm_cache_max_bytes = 0;
static int plv8_thread_pool_size = 0;
static char *plv8_boot_script_file = NULL;
static char *plv8_snapshot_file = NULL;
static int plv8_execution_timeout = 300;

#define PLV8_SNAPSHOT_MAGIC "PLV8SN02"
#define PLV8_SNAPSHOT_VERSION 2
#define PLV8_MAX_SNAPSHOT_FUNCTIONS 65536
#define PLV8_MAX_SNAPSHOT_BLOB_SIZE (256ULL * 1024ULL * 1024ULL)

struct Plv8SnapshotHeader
{
	char		magic[8];
	uint32		version;
	uint32		num_functions;
	uint64		blob_size;
	Oid			db_oid;
	char		v8_version[32];
	uint8		sha256[32];
};

struct Plv8SnapshotSlot
{
	Oid					fn_oid;
	TransactionId		fn_xmin;
	ItemPointerData		fn_tid;
	Oid					proowner;
	Oid					prolang;
	bool				prosecdef;
	bool				pass_user_types_as_bytes;
	bool				preserve_numeric_precision;
	int					nhandlers;
	plv8_handler_dep	handlers[PLV8_MAX_LANG_HANDLER_DEPTH];
};

static v8::StartupData plv8_active_snapshot_blob = { nullptr, 0 };
static std::vector<Plv8SnapshotSlot> plv8_snapshot_slots;
static Oid plv8_snapshot_db_oid = InvalidOid;
static bool plv8_snapshot_is_mmap = false;
static void *plv8_snapshot_mmap_addr = nullptr;
static size_t plv8_snapshot_mmap_len = 0;
static uint64 plv8_snapshot_functions_restored = 0;
static char plv8_snapshot_source[256] = "none";
static std::string plv8_loaded_snapshot_file;
static std::string plv8_loaded_boot_script_file;
static bool plv8_bypass_snapshot_restore = false;

struct Plv8WasmCacheKey
{
	Oid					db_oid;
	Oid					role_oid;
	uint8				wire_sha256[32];
};

struct Plv8WasmCacheEntry
{
	Plv8WasmCacheKey	key;
	dsa_pointer			code_ptr;
	uint32				code_size;
	uint64				last_used_seq;
};

#if PG_VERSION_NUM < 150000
#define PLV8_WASM_TRACKED_KEYS_MAX 512
#endif

struct Plv8SharedState
{
	LWLock			   *lock;
	LWLock				lock_storage;
	int					dsa_tranche_id;
	int					dshash_tranche_id;
	dsa_handle			wasm_dsa_handle;
	dshash_table_handle	wasm_dshash_handle;
	pg_atomic_uint64	bytes_used;
	pg_atomic_uint64	entries_count;
	pg_atomic_uint64	hits;
	pg_atomic_uint64	misses;
	pg_atomic_uint64	stores;
	pg_atomic_uint64	evictions;
	pg_atomic_uint64	access_seq;
#if PG_VERSION_NUM < 150000
	struct
	{
		bool				in_use;
		Plv8WasmCacheKey	key;
	}					tracked_keys[PLV8_WASM_TRACKED_KEYS_MAX];
#endif
};

static Plv8SharedState *plv8_shared_state = nullptr;
static dsa_area *local_wasm_dsa = nullptr;
static dshash_table *local_wasm_dshash = nullptr;

#ifndef DSA_HANDLE_INVALID
#define DSA_HANDLE_INVALID 0
#endif
#ifndef DSHASH_HANDLE_INVALID
#define DSHASH_HANDLE_INVALID 0
#endif

#if PG_VERSION_NUM >= 150000
static shmem_request_hook_type prev_shmem_request_hook = nullptr;
#endif
static shmem_startup_hook_type prev_shmem_startup_hook = nullptr;

#if PG_VERSION_NUM >= 150000
static void
plv8_shmem_request(void)
{
	if (prev_shmem_request_hook)
		prev_shmem_request_hook();

	RequestAddinShmemSpace(MAXALIGN(sizeof(Plv8SharedState)));
	RequestNamedLWLockTranche("plv8_wasm_cache", 1);
}
#endif

static void
plv8_init_shared_state_internal(void)
{
	if (plv8_shared_state != nullptr)
		return;

	bool found = false;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	PG_TRY();
	{
		plv8_shared_state = static_cast<Plv8SharedState *>(
			ShmemInitStruct("plv8_shared_state", sizeof(Plv8SharedState), &found));
		if (plv8_shared_state != nullptr && !found)
		{
			memset(plv8_shared_state, 0, sizeof(Plv8SharedState));
#if PG_VERSION_NUM >= 190000
			plv8_shared_state->dsa_tranche_id = LWLockNewTrancheId("plv8_wasm_dsa");
			plv8_shared_state->dshash_tranche_id = LWLockNewTrancheId("plv8_wasm_dshash");
#else
			plv8_shared_state->dsa_tranche_id = LWLockNewTrancheId();
			LWLockRegisterTranche(plv8_shared_state->dsa_tranche_id, "plv8_wasm_dsa");
			plv8_shared_state->dshash_tranche_id = LWLockNewTrancheId();
			LWLockRegisterTranche(plv8_shared_state->dshash_tranche_id, "plv8_wasm_dshash");
#endif
			LWLockInitialize(&plv8_shared_state->lock_storage, plv8_shared_state->dsa_tranche_id);
			plv8_shared_state->lock = &plv8_shared_state->lock_storage;
			plv8_shared_state->wasm_dsa_handle = DSA_HANDLE_INVALID;
			plv8_shared_state->wasm_dshash_handle = DSHASH_HANDLE_INVALID;
			pg_atomic_init_u64(&plv8_shared_state->bytes_used, 0);
			pg_atomic_init_u64(&plv8_shared_state->entries_count, 0);
			pg_atomic_init_u64(&plv8_shared_state->hits, 0);
			pg_atomic_init_u64(&plv8_shared_state->misses, 0);
			pg_atomic_init_u64(&plv8_shared_state->stores, 0);
			pg_atomic_init_u64(&plv8_shared_state->evictions, 0);
			pg_atomic_init_u64(&plv8_shared_state->access_seq, 0);
		}
	}
	PG_CATCH();
	{
		LWLockRelease(AddinShmemInitLock);
		FlushErrorState();
		plv8_shared_state = nullptr;
		return;
	}
	PG_END_TRY();
	LWLockRelease(AddinShmemInitLock);
}

static void
plv8_shmem_startup(void)
{
	if (prev_shmem_startup_hook)
		prev_shmem_startup_hook();

	plv8_init_shared_state_internal();
}

static dshash_table *
plv8_get_wasm_dshash()
{
	if (plv8_wasm_cache_size <= 0 && plv8_wasm_cache_max_bytes <= 0)
		return nullptr;

	if (plv8_shared_state == nullptr)
		plv8_init_shared_state_internal();

	if (plv8_shared_state == nullptr)
		return nullptr;

	if (local_wasm_dshash != nullptr)
		return local_wasm_dshash;

	dshash_parameters params = {
		sizeof(Plv8WasmCacheKey),
		sizeof(Plv8WasmCacheEntry),
		dshash_memcmp,
		dshash_memhash,
#if PG_VERSION_NUM >= 170000
		dshash_memcpy,
#endif
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
							   ((size_t) plv8_wasm_cache_size + 8) * 1024ULL * 1024ULL);
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

static bool
plv8_wasm_compute_key(const uint8_t *wire_bytes, size_t wire_len, Plv8WasmCacheKey *out_key)
{
	if (out_key == nullptr || (wire_bytes == nullptr && wire_len > 0))
		return false;

	memset(out_key, 0, sizeof(*out_key));
	out_key->db_oid = MyDatabaseId;
	out_key->role_oid = GetUserId();

	const char *v8_ver = v8::V8::GetVersion();
	pg_cryptohash_ctx *ctx = pg_cryptohash_create(PG_SHA256);
	if (ctx == nullptr)
		return false;

	if (pg_cryptohash_init(ctx) < 0 ||
		pg_cryptohash_update(ctx, reinterpret_cast<const uint8 *>(v8_ver), strlen(v8_ver) + 1) < 0 ||
		(wire_len > 0 && pg_cryptohash_update(ctx, reinterpret_cast<const uint8 *>(wire_bytes), wire_len) < 0) ||
		pg_cryptohash_final(ctx, out_key->wire_sha256, sizeof(out_key->wire_sha256)) < 0)
	{
		pg_cryptohash_free(ctx);
		return false;
	}
	pg_cryptohash_free(ctx);
	return true;
}

bool
plv8_wasm_cache_lookup(const uint8_t *wire_bytes, size_t wire_len, std::vector<uint8_t> *out_bytes)
{
	PLV8_ASSERT_MAIN_PG_THREAD();
	dshash_table *ht = plv8_get_wasm_dshash();
	if (ht == nullptr)
		return false;

	Plv8WasmCacheKey key;
	if (!plv8_wasm_compute_key(wire_bytes, wire_len, &key))
		return false;

	Plv8WasmCacheEntry *entry = static_cast<Plv8WasmCacheEntry *>(
		dshash_find(ht, &key, false));
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
		entry->last_used_seq = pg_atomic_fetch_add_u64(&plv8_shared_state->access_seq, 1) + 1;
		dshash_release_lock(ht, entry);
		pg_atomic_fetch_add_u64(&plv8_shared_state->hits, 1);
		return true;
	}

	dshash_release_lock(ht, entry);
	pg_atomic_fetch_add_u64(&plv8_shared_state->misses, 1);
	return false;
}

bool
plv8_wasm_cache_store(const uint8_t *wire_bytes, size_t wire_len, const uint8_t *bytes, size_t len)
{
	PLV8_ASSERT_MAIN_PG_THREAD();
	if (bytes == nullptr || len == 0)
		return false;

	dshash_table *ht = plv8_get_wasm_dshash();
	if (ht == nullptr)
		return false;

	size_t max_bytes = plv8_wasm_cache_max_bytes > 0
		? (size_t) plv8_wasm_cache_max_bytes
		: (size_t) plv8_wasm_cache_size * 1024ULL * 1024ULL;
	/* Do not allow a single entry to exceed 25% of the total cache */
	if (len > max_bytes / 4)
		return false;

	Plv8WasmCacheKey key;
	if (!plv8_wasm_compute_key(wire_bytes, wire_len, &key))
		return false;

	Plv8WasmCacheEntry *existing = static_cast<Plv8WasmCacheEntry *>(
		dshash_find(ht, &key, true));
	if (existing != nullptr)
	{
		if (DsaPointerIsValid(existing->code_ptr) && existing->code_size > 0)
		{
			existing->last_used_seq = pg_atomic_fetch_add_u64(&plv8_shared_state->access_seq, 1) + 1;
			dshash_release_lock(ht, existing);
			return true;
		}
		dshash_delete_entry(ht, existing);
	}

	size_t role_cap = superuser() ? max_bytes : (max_bytes / 4);
	size_t final_role_used = 0;

	LWLockAcquire(plv8_shared_state->lock, LW_EXCLUSIVE);
#if PG_VERSION_NUM >= 150000
	for (int iter = 0; iter < 64; iter++)
	{
		size_t role_used = 0;
		bool has_role_victim = false;
		uint64 role_min_seq = UINT64_MAX;
		Plv8WasmCacheKey role_victim_key;

		bool has_global_victim = false;
		uint64 global_min_seq = UINT64_MAX;
		Plv8WasmCacheKey global_victim_key;

		dshash_seq_status seq;
		dshash_seq_init(&seq, ht, false);
		Plv8WasmCacheEntry *cur;
		while ((cur = static_cast<Plv8WasmCacheEntry *>(dshash_seq_next(&seq))) != nullptr)
		{
			if (!DsaPointerIsValid(cur->code_ptr))
				continue;
			if (!has_global_victim || cur->last_used_seq < global_min_seq)
			{
				global_min_seq = cur->last_used_seq;
				global_victim_key = cur->key;
				has_global_victim = true;
			}
			if (cur->key.db_oid == key.db_oid && cur->key.role_oid == key.role_oid)
			{
				role_used += cur->code_size;
				if (!has_role_victim || cur->last_used_seq < role_min_seq)
				{
					role_min_seq = cur->last_used_seq;
					role_victim_key = cur->key;
					has_role_victim = true;
				}
			}
		}
		dshash_seq_term(&seq);
		final_role_used = role_used;

		bool over_role = (role_used + len > role_cap);
		bool over_global = (pg_atomic_read_u64(&plv8_shared_state->bytes_used) + len > max_bytes);
		if (!over_role && !over_global)
			break;

		Plv8WasmCacheKey victim_key;
		if (over_role && has_role_victim)
			victim_key = role_victim_key;
		else if (has_global_victim)
			victim_key = global_victim_key;
		else
			break;

		Plv8WasmCacheEntry *victim = static_cast<Plv8WasmCacheEntry *>(
			dshash_find(ht, &victim_key, true));
		if (victim != nullptr)
		{
			if (DsaPointerIsValid(victim->code_ptr))
			{
				dsa_free(local_wasm_dsa, victim->code_ptr);
				uint64 prev_used = pg_atomic_read_u64(&plv8_shared_state->bytes_used);
				if (prev_used >= victim->code_size)
					pg_atomic_fetch_sub_u64(&plv8_shared_state->bytes_used, victim->code_size);
				else
					pg_atomic_write_u64(&plv8_shared_state->bytes_used, 0);
				uint64 prev_cnt = pg_atomic_read_u64(&plv8_shared_state->entries_count);
				if (prev_cnt > 0)
					pg_atomic_fetch_sub_u64(&plv8_shared_state->entries_count, 1);
				pg_atomic_fetch_add_u64(&plv8_shared_state->evictions, 1);
			}
			dshash_delete_entry(ht, victim);
		}
	}
#else
	for (int iter = 0; iter < 64; iter++)
	{
		size_t role_used = 0;
		bool has_role_victim = false;
		uint64 role_min_seq = UINT64_MAX;
		Plv8WasmCacheKey role_victim_key;
		int role_victim_slot = -1;

		int active_slots = 0;
		bool has_global_victim = false;
		uint64 global_min_seq = UINT64_MAX;
		Plv8WasmCacheKey global_victim_key;
		int global_victim_slot = -1;

		for (int k = 0; k < PLV8_WASM_TRACKED_KEYS_MAX; k++)
		{
			if (!plv8_shared_state->tracked_keys[k].in_use)
				continue;
			Plv8WasmCacheEntry *cur = static_cast<Plv8WasmCacheEntry *>(
				dshash_find(ht, &plv8_shared_state->tracked_keys[k].key, false));
			if (cur == nullptr)
			{
				plv8_shared_state->tracked_keys[k].in_use = false;
				continue;
			}
			if (!DsaPointerIsValid(cur->code_ptr))
			{
				dshash_release_lock(ht, cur);
				plv8_shared_state->tracked_keys[k].in_use = false;
				continue;
			}
			uint64 seq_val = cur->last_used_seq;
			uint32 code_sz = cur->code_size;
			Plv8WasmCacheKey cur_key = cur->key;
			dshash_release_lock(ht, cur);

			active_slots++;
			if (!has_global_victim || seq_val < global_min_seq)
			{
				global_min_seq = seq_val;
				global_victim_key = cur_key;
				global_victim_slot = k;
				has_global_victim = true;
			}
			if (cur_key.db_oid == key.db_oid && cur_key.role_oid == key.role_oid)
			{
				role_used += code_sz;
				if (!has_role_victim || seq_val < role_min_seq)
				{
					role_min_seq = seq_val;
					role_victim_key = cur_key;
					role_victim_slot = k;
					has_role_victim = true;
				}
			}
		}
		final_role_used = role_used;

		bool over_role = (role_used + len > role_cap);
		bool over_global = (pg_atomic_read_u64(&plv8_shared_state->bytes_used) + len > max_bytes);
		bool over_slots = (active_slots >= PLV8_WASM_TRACKED_KEYS_MAX);
		if (!over_role && !over_global && !over_slots)
			break;

		Plv8WasmCacheKey victim_key;
		int victim_slot = -1;
		if (over_role && has_role_victim)
		{
			victim_key = role_victim_key;
			victim_slot = role_victim_slot;
		}
		else if (has_global_victim)
		{
			victim_key = global_victim_key;
			victim_slot = global_victim_slot;
		}
		else
			break;

		if (victim_slot >= 0)
			plv8_shared_state->tracked_keys[victim_slot].in_use = false;

		Plv8WasmCacheEntry *victim = static_cast<Plv8WasmCacheEntry *>(
			dshash_find(ht, &victim_key, true));
		if (victim != nullptr)
		{
			if (DsaPointerIsValid(victim->code_ptr))
			{
				dsa_free(local_wasm_dsa, victim->code_ptr);
				uint64 prev_used = pg_atomic_read_u64(&plv8_shared_state->bytes_used);
				if (prev_used >= victim->code_size)
					pg_atomic_fetch_sub_u64(&plv8_shared_state->bytes_used, victim->code_size);
				else
					pg_atomic_write_u64(&plv8_shared_state->bytes_used, 0);
				uint64 prev_cnt = pg_atomic_read_u64(&plv8_shared_state->entries_count);
				if (prev_cnt > 0)
					pg_atomic_fetch_sub_u64(&plv8_shared_state->entries_count, 1);
				pg_atomic_fetch_add_u64(&plv8_shared_state->evictions, 1);
			}
			dshash_delete_entry(ht, victim);
		}
	}
#endif

	if (final_role_used + len > role_cap ||
		pg_atomic_read_u64(&plv8_shared_state->bytes_used) + len > max_bytes)
	{
		LWLockRelease(plv8_shared_state->lock);
		return false;
	}

	bool found = false;
	Plv8WasmCacheEntry *entry = static_cast<Plv8WasmCacheEntry *>(
		dshash_find_or_insert(ht, &key, &found));
	if (entry == nullptr)
	{
		LWLockRelease(plv8_shared_state->lock);
		return false;
	}

	if (found && DsaPointerIsValid(entry->code_ptr))
	{
		dshash_release_lock(ht, entry);
		LWLockRelease(plv8_shared_state->lock);
		return true;
	}

	dsa_pointer dp = dsa_allocate_extended(local_wasm_dsa, len, DSA_ALLOC_NO_OOM);
	if (!DsaPointerIsValid(dp))
	{
		dshash_delete_entry(ht, entry);
		LWLockRelease(plv8_shared_state->lock);
		return false;
	}

	void *dst = dsa_get_address(local_wasm_dsa, dp);
	memcpy(dst, bytes, len);
	entry->code_ptr = dp;
	entry->code_size = static_cast<uint32>(len);
	entry->last_used_seq = pg_atomic_fetch_add_u64(&plv8_shared_state->access_seq, 1) + 1;
	dshash_release_lock(ht, entry);

#if PG_VERSION_NUM < 150000
	for (int k = 0; k < PLV8_WASM_TRACKED_KEYS_MAX; k++)
	{
		if (!plv8_shared_state->tracked_keys[k].in_use)
		{
			plv8_shared_state->tracked_keys[k].in_use = true;
			plv8_shared_state->tracked_keys[k].key = key;
			break;
		}
	}
#endif

	pg_atomic_fetch_add_u64(&plv8_shared_state->bytes_used, len);
	pg_atomic_fetch_add_u64(&plv8_shared_state->entries_count, 1);
	pg_atomic_fetch_add_u64(&plv8_shared_state->stores, 1);
	LWLockRelease(plv8_shared_state->lock);
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

static bool plv8_cached_tle_fp_valid = false;
static Oid plv8_cached_tle_user_id = InvalidOid;
static TransactionId plv8_cached_tle_xid = InvalidTransactionId;
static SubTransactionId plv8_cached_tle_subid = InvalidSubTransactionId;
static CommandId plv8_cached_tle_cid = 0;
static uint64 plv8_cached_tle_fp = 0;

static uint64 plv8_catalog_gen = 1;
static Oid plv8_cached_holder_relid = InvalidOid;
static bool plv8_in_internal_catalog_update = false;

uint64
plv8_get_catalog_gen(void)
{
	return plv8_catalog_gen;
}

static void DisposePlv8ContextHandles(plv8_context *ctx);
static void KillIsolateAndAllContexts(void);
static void EnsureSharedIsolate(void);
static void plv8_cache_function_remove(Oid fn_oid);
static bool plv8_cache_handlers_valid(plv8_proc_cache *cache, HeapTuple proctuple, Oid *stale_handler_oid);
static bool plv8_is_js_language(Oid lang_oid);
static char *plv8_transpile_src(const char *src, Oid lang_oid, plv8_handler_dep *deps, int *ndeps, plv8_proc *proc, bool *preserve_numeric_out = NULL);
static bool plv8_extract_hoisted_imports(const char *src, std::string *imports_out, std::string *body_out);
static Local<Object> plv8_compile_and_eval_anon_module(Local<Context> context, const char *name, const char *mod_src);
static void plv8_sync_tle_module_cache(plv8_context *ctx);
static bool plv8_try_restore_function_from_snapshot(plv8_proc_cache *cache, plv8_context *global_context, const char *raw_prosrc);
static void plv8_append_json_string(StringInfo buf, const char *str);

/*
 * lower_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo,
		bool validate, char ***argnames, Oid *prolang_out, char **raw_prosrc_out) throw();
static void plv8_xact_cb(XactEvent event, void *arg);
static void plv8_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
		SubTransactionId parentSubid, void *arg);

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
plv8_context *GetPlv8Context();
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

static void KillIsolateAndAllContexts(void);

static void
ReleaseActivePlv8Snapshot(void)
{
	if (current_isolate != nullptr)
		KillIsolateAndAllContexts();

	if (plv8_snapshot_is_mmap && plv8_snapshot_mmap_addr != nullptr && plv8_snapshot_mmap_len > 0)
	{
		munmap(plv8_snapshot_mmap_addr, plv8_snapshot_mmap_len);
	}
	plv8_snapshot_is_mmap = false;
	plv8_snapshot_mmap_addr = nullptr;
	plv8_snapshot_mmap_len = 0;
	plv8_active_snapshot_blob.data = nullptr;
	plv8_active_snapshot_blob.raw_size = 0;
	plv8_snapshot_slots.clear();
	plv8_snapshot_db_oid = InvalidOid;
	plv8_snapshot_functions_restored = 0;
	snprintf(plv8_snapshot_source, sizeof(plv8_snapshot_source), "none");
}

static bool
LoadPlv8SnapshotFileInPreload(const char *path)
{
	if (path == nullptr || path[0] == '\0')
		return false;

	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("plv8: could not open snapshot_file \"%s\": %m", path)));
	}

	struct stat st;
	const size_t max_file_size = sizeof(Plv8SnapshotHeader) +
		(size_t) PLV8_MAX_SNAPSHOT_FUNCTIONS * sizeof(Plv8SnapshotSlot) +
		PLV8_MAX_SNAPSHOT_BLOB_SIZE;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
		(size_t) st.st_size < sizeof(Plv8SnapshotHeader) ||
		(size_t) st.st_size > max_file_size)
	{
		close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("plv8: snapshot_file \"%s\" is invalid, not a regular file, or out of size bounds", path)));
	}

	size_t map_len = (size_t) st.st_size;
	void *addr = mmap(nullptr, map_len, PROT_READ | PROT_WRITE,
					  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED)
	{
		close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("plv8: anonymous mmap failed for snapshot_file \"%s\": %m", path)));
	}

	char *dst = static_cast<char *>(addr);
	size_t rem = map_len;
	while (rem > 0)
	{
		ssize_t r = read(fd, dst, rem);
		if (r <= 0)
		{
			if (r < 0 && errno == EINTR)
				continue;
			close(fd);
			munmap(addr, map_len);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("plv8: failed to read snapshot_file \"%s\"", path)));
		}
		dst += r;
		rem -= (size_t) r;
	}
	close(fd);

	const Plv8SnapshotHeader *hdr = static_cast<const Plv8SnapshotHeader *>(addr);
	if (memcmp(hdr->magic, PLV8_SNAPSHOT_MAGIC, 8) != 0 ||
		hdr->version != PLV8_SNAPSHOT_VERSION)
	{
		munmap(addr, map_len);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("plv8: snapshot_file \"%s\" header magic/version mismatch", path)));
	}

	if (hdr->num_functions > PLV8_MAX_SNAPSHOT_FUNCTIONS ||
		hdr->blob_size == 0 ||
		hdr->blob_size > PLV8_MAX_SNAPSHOT_BLOB_SIZE)
	{
		munmap(addr, map_len);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("plv8: snapshot_file \"%s\" header bounds out of range", path)));
	}

	size_t slots_bytes = (size_t) hdr->num_functions * sizeof(Plv8SnapshotSlot);
	size_t header_total = sizeof(Plv8SnapshotHeader) + slots_bytes;
	if (header_total > map_len || hdr->blob_size != map_len - header_total)
	{
		munmap(addr, map_len);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("plv8: snapshot_file \"%s\" size corrupt", path)));
	}

	if (strncmp(hdr->v8_version, v8::V8::GetVersion(), sizeof(hdr->v8_version) - 1) != 0)
	{
		munmap(addr, map_len);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("plv8: snapshot_file \"%s\" V8 version mismatch (snapshot=\"%.*s\", runtime=\"%s\")",
						path, (int) (sizeof(hdr->v8_version) - 1), hdr->v8_version,
						v8::V8::GetVersion())));
	}

	uint8_t digest[32];
	pg_cryptohash_ctx *hash_ctx = pg_cryptohash_create(PG_SHA256);
	if (hash_ctx == NULL ||
		pg_cryptohash_init(hash_ctx) < 0 ||
		pg_cryptohash_update(hash_ctx,
							 static_cast<const uint8_t *>(addr) + sizeof(Plv8SnapshotHeader),
							 map_len - sizeof(Plv8SnapshotHeader)) < 0 ||
		pg_cryptohash_final(hash_ctx, digest, sizeof(digest)) < 0)
	{
		if (hash_ctx != NULL)
			pg_cryptohash_free(hash_ctx);
		munmap(addr, map_len);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("plv8: failed to compute SHA-256 for snapshot_file \"%s\"", path)));
	}
	pg_cryptohash_free(hash_ctx);

	if (memcmp(digest, hdr->sha256, sizeof(digest)) != 0)
	{
		munmap(addr, map_len);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("plv8: snapshot_file \"%s\" SHA-256 checksum mismatch", path)));
	}

	mprotect(addr, map_len, PROT_READ);

	ReleaseActivePlv8Snapshot();

	const Plv8SnapshotSlot *slots_ptr = reinterpret_cast<const Plv8SnapshotSlot *>(
		static_cast<const char *>(addr) + sizeof(Plv8SnapshotHeader));
	plv8_snapshot_slots.assign(slots_ptr, slots_ptr + hdr->num_functions);
	plv8_snapshot_db_oid = hdr->db_oid;

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

	int fd = open(script_path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0)
	{
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("plv8: could not open boot_script_file \"%s\": %m",
						script_path)));
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
		st.st_size <= 0 || st.st_size > 16 * 1024 * 1024)
	{
		close(fd);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("plv8: boot_script_file \"%s\" is empty, not a regular file, or exceeds 16 MB",
						script_path)));
	}
	std::string script_src((size_t) st.st_size, '\0');
	char *dst = &script_src[0];
	size_t rem = (size_t) st.st_size;
	while (rem > 0)
	{
		ssize_t r = read(fd, dst, rem);
		if (r <= 0)
		{
			if (r < 0 && errno == EINTR)
				continue;
			close(fd);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("plv8: failed to read boot_script_file \"%s\"",
							script_path)));
		}
		dst += r;
		rem -= (size_t) r;
	}
	close(fd);

	int timeout_sec = (plv8_execution_timeout > 0 && plv8_execution_timeout <= 300)
		? plv8_execution_timeout
		: 30;

	if (!process_shared_preload_libraries_in_progress)
	{
		plv8_init_v8_engine();
		if (v8_platform)
			v8_platform->EnsureBackendPlatform();

		v8::StartupData blob = { nullptr, 0 };
		bool timed_out = false;
		bool compile_failed = false;

		{
			ScopedSignalBlockerForV8Workers sig_blocker;
			v8::ArrayBuffer::Allocator *alloc =
				v8::ArrayBuffer::Allocator::NewDefaultAllocator();
			{
				v8::Isolate::CreateParams create_params;
				create_params.array_buffer_allocator = alloc;
				create_params.external_references = plv8_external_references;
				v8::SnapshotCreator creator(create_params);
				v8::Isolate *iso = creator.GetIsolate();
				std::atomic<bool> script_done{false};
				std::atomic<bool> watchdog_fired{false};
				std::thread watchdog([iso, &script_done, &watchdog_fired, timeout_sec]() {
					for (int ms = 0; ms < timeout_sec * 1000; ms += 25)
					{
						if (script_done.load(std::memory_order_acquire))
							return;
						pg_usleep(25000L);
					}
					if (!script_done.load(std::memory_order_acquire))
					{
						watchdog_fired.store(true, std::memory_order_release);
						iso->TerminateExecution();
					}
				});
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
							if (tc.HasTerminated() || watchdog_fired.load(std::memory_order_acquire))
								timed_out = true;
							else
								compile_failed = true;
						}
					}
					script_done.store(true, std::memory_order_release);
					watchdog.join();
					creator.AddContext(ctx);
				}
				if (!timed_out && !compile_failed)
				{
					blob = creator.CreateBlob(
						v8::SnapshotCreator::FunctionCodeHandling::kKeep);
				}
			}
			delete alloc;
		}

		if (timed_out)
		{
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("plv8: timed out compiling boot_script_file \"%s\"",
							script_path)));
		}
		if (compile_failed || blob.data == nullptr || blob.raw_size <= 0 ||
			(size_t) blob.raw_size > PLV8_MAX_SNAPSHOT_BLOB_SIZE)
		{
			delete[] blob.data;
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("plv8: failed to build boot snapshot from \"%s\"",
							script_path)));
		}

		size_t map_len = (size_t) blob.raw_size;
		void *shared_mem = mmap(nullptr, map_len,
								PROT_READ | PROT_WRITE,
								MAP_SHARED | MAP_ANONYMOUS, -1, 0);
		if (shared_mem == MAP_FAILED)
		{
			delete[] blob.data;
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("plv8: anonymous mmap failed for boot_script_file \"%s\": %m",
							script_path)));
		}
		memcpy(shared_mem, blob.data, map_len);
		delete[] blob.data;
		mprotect(shared_mem, map_len, PROT_READ);

		ReleaseActivePlv8Snapshot();
		plv8_active_snapshot_blob.data = static_cast<const char *>(shared_mem);
		plv8_active_snapshot_blob.raw_size = static_cast<int>(map_len);
		plv8_snapshot_is_mmap = true;
		plv8_snapshot_mmap_addr = shared_mem;
		plv8_snapshot_mmap_len = map_len;
		snprintf(plv8_snapshot_source, sizeof(plv8_snapshot_source), "boot_script_prefork");
		return true;
	}

	/*
	 * Compile the boot script snapshot in a short-lived forked child process
	 * and write the resulting blob into a MAP_SHARED anonymous mapping.
	 * This guarantees the postmaster process itself never spawns any V8
	 * background threads before forking PostgreSQL backends.
	 */
	size_t max_shared_blob = PLV8_MAX_SNAPSHOT_BLOB_SIZE;
	void *shared_mem = mmap(nullptr, max_shared_blob,
							PROT_READ | PROT_WRITE,
							MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared_mem == MAP_FAILED)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("plv8: anonymous mmap failed for boot_script_file \"%s\": %m",
						script_path)));
	}

	pid_t pid = fork();
	if (pid < 0)
	{
		munmap(shared_mem, max_shared_blob);
		ereport(ERROR,
				(errcode(ERRCODE_SYSTEM_ERROR),
				 errmsg("plv8: fork failed while compiling boot_script_file \"%s\": %m",
						script_path)));
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
			std::atomic<bool> script_done{false};
			std::atomic<bool> watchdog_fired{false};
			std::thread watchdog([iso, &script_done, &watchdog_fired, timeout_sec]() {
				for (int ms = 0; ms < timeout_sec * 1000; ms += 25)
				{
					if (script_done.load(std::memory_order_acquire))
						return;
					pg_usleep(25000L);
				}
				if (!script_done.load(std::memory_order_acquire))
				{
					watchdog_fired.store(true, std::memory_order_release);
					iso->TerminateExecution();
				}
			});
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
						bool timed_out = tc.HasTerminated() || watchdog_fired.load(std::memory_order_acquire);
						script_done.store(true, std::memory_order_release);
						watchdog.join();
						_exit(timed_out ? 4 : 2);
					}
				}
				script_done.store(true, std::memory_order_release);
				watchdog.join();
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
	bool child_exited = false;
	int max_wait_ms = (timeout_sec + 5) * 1000;
	for (int waited_ms = 0; waited_ms < max_wait_ms; waited_ms += 10)
	{
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid)
		{
			child_exited = true;
			break;
		}
		if (w < 0 && errno != EINTR)
			break;
		pg_usleep(10000L);
	}
	if (!child_exited || (WIFEXITED(status) && WEXITSTATUS(status) == 4))
	{
		if (!child_exited)
		{
			kill(pid, SIGKILL);
			while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
		}
		munmap(shared_mem, max_shared_blob);
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("plv8: timed out compiling boot_script_file \"%s\"",
						script_path)));
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
	{
		int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		munmap(shared_mem, max_shared_blob);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("plv8: failed to build boot snapshot from \"%s\" (exit code %d)",
						script_path, code)));
	}

	uint64 blob_sz = 0;
	memcpy(&blob_sz, shared_mem, sizeof(uint64));
	if (blob_sz == 0 || sizeof(uint64) + blob_sz > max_shared_blob)
	{
		munmap(shared_mem, max_shared_blob);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("plv8: invalid boot snapshot size from \"%s\"", script_path)));
	}

	mprotect(shared_mem, max_shared_blob, PROT_READ);
	ReleaseActivePlv8Snapshot();
	plv8_active_snapshot_blob.data =
		static_cast<const char *>(shared_mem) + sizeof(uint64);
	plv8_active_snapshot_blob.raw_size = static_cast<int>(blob_sz);
	plv8_snapshot_is_mmap = true;
	plv8_snapshot_mmap_addr = shared_mem;
	plv8_snapshot_mmap_len = max_shared_blob;
	snprintf(plv8_snapshot_source, sizeof(plv8_snapshot_source), "boot_script_prefork");
	return true;
}

static void
SyncPlv8SnapshotFromGucs(void)
{
	std::string want_snap = (plv8_snapshot_file != NULL) ? plv8_snapshot_file : "";
	std::string want_boot = (plv8_boot_script_file != NULL) ? plv8_boot_script_file : "";

	if (want_snap == plv8_loaded_snapshot_file && want_boot == plv8_loaded_boot_script_file)
		return;

	if (!want_snap.empty())
	{
		LoadPlv8SnapshotFileInPreload(want_snap.c_str());
	}
	else if (!want_boot.empty())
	{
		BuildBootScriptSnapshotInPreload(want_boot.c_str());
	}
	else
	{
		ReleaseActivePlv8Snapshot();
	}

	plv8_loaded_snapshot_file = want_snap;
	plv8_loaded_boot_script_file = want_boot;
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
	current_isolate->SetHostImportModuleDynamicallyCallback(plv8_HostImportModuleDynamicallyCallback);
	current_isolate->SetHostInitializeImportMetaObjectCallback(plv8_HostInitializeImportMetaObjectCallback);
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

#define ALLOW_RAW_USER_TYPES_VAR "plv8.allow_raw_user_types"
    guc_value = plv8_find_option(ALLOW_RAW_USER_TYPES_VAR);
    if (guc_value != NULL) {
        plv8_allow_raw_user_types = plv8_bool_option(guc_value);
    } else {
        DefineCustomBoolVariable(ALLOW_RAW_USER_TYPES_VAR,
                                 gettext_noop("Allow superuser-owned language handlers to pass user-defined types as raw byte buffers."),
                                 NULL,
                                 &plv8_allow_raw_user_types,
                                 false,
                                 PGC_SUSET, 0,
                                 NULL,
                                 NULL,
                                 NULL);
    }
#undef ALLOW_RAW_USER_TYPES_VAR

#define BOOT_SCRIPT_FILE_VAR "plv8.boot_script_file"
    guc_value = plv8_find_option(BOOT_SCRIPT_FILE_VAR);
    if (guc_value != NULL) {
        plv8_boot_script_file = plv8_string_option(guc_value);
    } else {
        DefineCustomStringVariable(BOOT_SCRIPT_FILE_VAR,
                                   gettext_noop("JavaScript file compiled into a shared V8 startup snapshot during postmaster preload or superuser reload."),
                                   NULL,
                                   &plv8_boot_script_file,
                                   NULL,
                                   PGC_SUSET, GUC_SUPERUSER_ONLY,
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
                                   PGC_SUSET, GUC_SUPERUSER_ONLY,
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
                                PGC_SUSET, GUC_SUPERUSER_ONLY,
                                NULL,
                                NULL,
                                NULL);
    }
#undef WASM_CACHE_SIZE_VAR

#define WASM_CACHE_MAX_BYTES_VAR "plv8.wasm_cache_max_bytes"
    guc_value = plv8_find_option(WASM_CACHE_MAX_BYTES_VAR);
    if (guc_value != NULL) {
        plv8_wasm_cache_max_bytes = plv8_int_option(guc_value);
    } else {
        DefineCustomIntVariable(WASM_CACHE_MAX_BYTES_VAR,
                                gettext_noop("Optional fine-grained byte limit override for the WebAssembly cache (0 = use plv8.wasm_cache_size MB)."),
                                NULL,
                                &plv8_wasm_cache_max_bytes,
                                0, 0, INT_MAX,
                                PGC_SUSET, GUC_SUPERUSER_ONLY,
                                NULL,
                                NULL,
                                NULL);
    }
#undef WASM_CACHE_MAX_BYTES_VAR

#define MAX_MODULE_DEPTH_VAR "plv8.max_module_depth"
    guc_value = plv8_find_option(MAX_MODULE_DEPTH_VAR);
    if (guc_value != NULL) {
        plv8_max_module_depth = plv8_int_option(guc_value);
    } else {
        DefineCustomIntVariable(MAX_MODULE_DEPTH_VAR,
                                gettext_noop("Maximum module import/require nesting depth."),
                                NULL,
                                &plv8_max_module_depth,
                                32, 1, 256,
                                PGC_USERSET, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef MAX_MODULE_DEPTH_VAR

	if (process_shared_preload_libraries_in_progress)
	{
#if PG_VERSION_NUM >= 150000
		prev_shmem_request_hook = shmem_request_hook;
		shmem_request_hook = plv8_shmem_request;
#else
		RequestAddinShmemSpace(MAXALIGN(sizeof(Plv8SharedState)));
		RequestNamedLWLockTranche("plv8_wasm_cache", 1);
#endif
		prev_shmem_startup_hook = shmem_startup_hook;
		shmem_startup_hook = plv8_shmem_startup;

		if (plv8_snapshot_file != NULL && plv8_snapshot_file[0] != '\0')
		{
			plv8_init_v8_engine();
			LoadPlv8SnapshotFileInPreload(plv8_snapshot_file);
			plv8_loaded_snapshot_file = plv8_snapshot_file;
		}
		else if (plv8_boot_script_file != NULL && plv8_boot_script_file[0] != '\0')
		{
			BuildBootScriptSnapshotInPreload(plv8_boot_script_file);
			plv8_init_v8_engine();
			plv8_loaded_boot_script_file = plv8_boot_script_file;
		}
		else
		{
			plv8_init_v8_engine();
		}
	}

	RegisterXactCallback(plv8_xact_cb, NULL);
	RegisterSubXactCallback(plv8_subxact_cb, NULL);
	CacheRegisterRelcacheCallback(plv8_modules_inval_cb, (Datum) 0);

	EmitWarningsOnPlaceholders("plv8");

	if (!process_shared_preload_libraries_in_progress)
	{
		plv8_init_v8_engine();
	}
}

static void
plv8_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
				SubTransactionId parentSubid, void *arg)
{
	if (event == SUBXACT_EVENT_ABORT_SUB)
		plv8_catalog_gen++;

	plv8_fdw_subxact_cb(event, mySubid, parentSubid);
}

static void
plv8_xact_cb(XactEvent event, void *arg)
{
	plv8_exec_env	   *env = exec_env_head;

	plv8_cached_tle_fp_valid = false;
	if (event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT)
		plv8_catalog_gen++;

	plv8_fdw_xact_cb(event);


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

	plv8_reset_all_stamped_procs();

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
			if (valid)
			{
				PG_TRY();
				{
					if (proc->cache->tle_modules_fp != plv8_tle_modules_fingerprint())
						valid = false;
					if (valid && proc->cache->nhandlers > 0)
					{
						Oid stale_handler_oid = InvalidOid;
						if (!plv8_cache_handlers_valid(proc->cache, NULL, &stale_handler_oid))
						{
							valid = false;
							if (OidIsValid(stale_handler_oid))
								plv8_cache_function_remove(stale_handler_oid);
						}
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
			bool prev_numeric;
			PassUserTypesScope(bool val, bool numeric_val)
				: prev(plv8_pass_user_types_as_bytes),
				  prev_numeric(plv8_preserve_numeric_precision) {
				plv8_pass_user_types_as_bytes = val;
				plv8_preserve_numeric_precision = numeric_val;
			}
			~PassUserTypesScope() {
				plv8_pass_user_types_as_bytes = prev;
				plv8_preserve_numeric_precision = prev_numeric;
			}
		} user_types_scope(cache->pass_user_types_as_bytes &&
						   plv8_allow_raw_user_types &&
						   superuser_arg(cache->proowner),
						   cache->preserve_numeric_precision);

		if (!is_trigger && cache->nargs == 1 &&
			proc->argtypes[0].typid == INTERNALOID &&
			proc->rettype.typid == VOIDOID && fcinfo->nargs == 1 &&
			!PG_ARGISNULL(0))
		{
			Node *arg0 = (Node *) DatumGetPointer(PG_GETARG_DATUM(0));
			if (arg0 != NULL && IsA(arg0, InlineCodeBlock))
			{
				return plv8_inline_handler(fcinfo);
			}
		}

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

	plv8_dispose_support_hooks_for_user(ctx->user_id);

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
				cache->pass_user_types_as_bytes = false;
				cache->preserve_numeric_precision = false;
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

	for (auto &kv : ctx->tle_module_map)
		kv.second.Reset();
	ctx->tle_module_map.clear();
	ctx->tle_module_id_to_spec.clear();
	ctx->tle_module_map.~unordered_map();
	ctx->tle_module_id_to_spec.~unordered_map();

	for (auto &fn_glob : ctx->snapshot_functions)
		fn_glob.Reset();
	ctx->snapshot_functions.clear();
	ctx->snapshot_functions.~vector();

	ctx->context.Reset();
	ctx->compile_context.Reset();
	ctx->recv_templ.Reset();
	ctx->plan_ftmpl.Reset();
	ctx->plan_template.Reset();
	ctx->cursor_ftmpl.Reset();
	ctx->cursor_template.Reset();
	ctx->window_ftmpl.Reset();
	ctx->window_template.Reset();
	ctx->plv8_obj.Reset();
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
			cache->pass_user_types_as_bytes = false;
			cache->preserve_numeric_precision = false;
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

	if (superuser())
		SyncPlv8SnapshotFromGucs();

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
	plv8_fdw_reset(user_id);
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

	if (plv8_shared_state == nullptr &&
		(plv8_wasm_cache_size > 0 || plv8_wasm_cache_max_bytes > 0))
	{
		plv8_init_shared_state_internal();
	}

	uint64 wasm_bytes = 0, wasm_entries = 0, wasm_hits = 0, wasm_misses = 0, wasm_stores = 0, wasm_evictions = 0;
	bool wasm_cache_enabled = (plv8_shared_state != nullptr &&
							   (plv8_wasm_cache_size > 0 || plv8_wasm_cache_max_bytes > 0));
	if (wasm_cache_enabled)
	{
		wasm_bytes = pg_atomic_read_u64(&plv8_shared_state->bytes_used);
		wasm_entries = pg_atomic_read_u64(&plv8_shared_state->entries_count);
		wasm_hits = pg_atomic_read_u64(&plv8_shared_state->hits);
		wasm_misses = pg_atomic_read_u64(&plv8_shared_state->misses);
		wasm_stores = pg_atomic_read_u64(&plv8_shared_state->stores);
		wasm_evictions = pg_atomic_read_u64(&plv8_shared_state->evictions);
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
		appendStringInfoString(&roles_buf, "{\"user\":");
		plv8_append_json_string(&roles_buf, username ? username : "unknown");
		appendStringInfo(&roles_buf,
						 ",\"total_heap_size\":%zu,\"used_heap_size\":%zu,\"external_memory\":%zu}",
						 total_heap, used_heap, ext_mem);
	}
	appendStringInfoChar(&roles_buf, ']');

	StringInfoData src_json_buf;
	initStringInfo(&src_json_buf);
	plv8_append_json_string(&src_json_buf, plv8_snapshot_source);

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
		"\"snapshot_source\":%s,"
		"\"snapshot_blob_bytes\":%d,"
		"\"snapshot_fn_count\":%zu,"
		"\"snapshot_functions_restored\":%llu,"
		"\"snapshot\":{\"loaded\":%s,\"bytes\":%d,\"functions_count\":%zu,\"functions_restored\":%llu,\"source\":%s},"
		"\"wasm_cache\":{\"enabled\":%s,\"entries\":%llu,\"bytes_used\":%llu,\"hits\":%llu,\"misses\":%llu,\"stores\":%llu,\"evictions\":%llu}}",
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
		src_json_buf.data,
		plv8_active_snapshot_blob.raw_size,
		plv8_snapshot_slots.size(),
		(unsigned long long) plv8_snapshot_functions_restored,
		snap_loaded ? "true" : "false",
		plv8_active_snapshot_blob.raw_size,
		plv8_snapshot_slots.size(),
		(unsigned long long) plv8_snapshot_functions_restored,
		src_json_buf.data,
		wasm_cache_enabled ? "true" : "false",
		(unsigned long long) wasm_entries,
		(unsigned long long) wasm_bytes,
		(unsigned long long) wasm_hits,
		(unsigned long long) wasm_misses,
		(unsigned long long) wasm_stores,
		(unsigned long long) wasm_evictions);

	pfree(roles_buf.data);
	pfree(src_json_buf.data);
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
	struct stat initial_out_st;
	if (lstat(out_path, &initial_out_st) == 0 && S_ISLNK(initial_out_st.st_mode))
	{
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("refusing to write snapshot to symlink \"%s\"", out_path)));
	}
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

	if (target_oids.size() > PLV8_MAX_SNAPSHOT_FUNCTIONS)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot snapshot more than %d functions",
						PLV8_MAX_SNAPSHOT_FUNCTIONS)));

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
		Oid proowner = procStruct->proowner;
		bool prosecdef = procStruct->prosecdef;
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
		item.slot.proowner = proowner;
		item.slot.prolang = prolang;
		item.slot.prosecdef = prosecdef;
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
					item.slot.pass_user_types_as_bytes =
						compiled_proc->cache->pass_user_types_as_bytes;
					item.slot.preserve_numeric_precision =
						compiled_proc->cache->preserve_numeric_precision;
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

	std::string boot_script_src;
	if (plv8_boot_script_file != NULL && plv8_boot_script_file[0] != '\0')
	{
		int bfd = open(plv8_boot_script_file, O_RDONLY | O_NOFOLLOW);
		if (bfd < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open boot_script_file \"%s\": %m",
							plv8_boot_script_file)));
		struct stat bst;
		if (fstat(bfd, &bst) < 0 || !S_ISREG(bst.st_mode) ||
			bst.st_size <= 0 || bst.st_size > 16 * 1024 * 1024)
		{
			close(bfd);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("boot_script_file \"%s\" is empty, not a regular file, or exceeds 16 MB",
							plv8_boot_script_file)));
		}
		boot_script_src.resize((size_t) bst.st_size, '\0');
		char *bdst = &boot_script_src[0];
		size_t brem = (size_t) bst.st_size;
		while (brem > 0)
		{
			ssize_t r = read(bfd, bdst, brem);
			if (r <= 0)
			{
				if (r < 0 && errno == EINTR)
					continue;
				close(bfd);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("failed to read boot_script_file \"%s\"",
								plv8_boot_script_file)));
			}
			bdst += r;
			brem -= (size_t) r;
		}
		close(bfd);
	}

	std::vector<Plv8SnapshotSlot> compiled_slots;
	v8::StartupData blob = { nullptr, 0 };
	std::string snap_compile_error;
	bool snap_timed_out = false;

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
			snap_isolate->SetHostImportModuleDynamicallyCallback(plv8_HostImportModuleDynamicallyCallback);
			snap_isolate->SetHostInitializeImportMetaObjectCallback(plv8_HostInitializeImportMetaObjectCallback);

			int timeout_sec = (plv8_execution_timeout > 0 && plv8_execution_timeout <= 300)
				? plv8_execution_timeout
				: 30;
			std::atomic<bool> snap_done{false};
			std::atomic<bool> watchdog_fired{false};
			std::thread watchdog([snap_isolate, &snap_done, &watchdog_fired, timeout_sec]() {
				for (int ms = 0; ms < timeout_sec * 1000; ms += 25)
				{
					if (snap_done.load(std::memory_order_acquire))
						return;
					pg_usleep(25000L);
				}
				if (!snap_done.load(std::memory_order_acquire))
				{
					watchdog_fired.store(true, std::memory_order_release);
					snap_isolate->TerminateExecution();
				}
			});

			{
				v8::HandleScope handle_scope(snap_isolate);
				v8::Local<v8::Context> default_ctx = v8::Context::New(snap_isolate);
				creator.SetDefaultContext(default_ctx);

				v8::Local<v8::ObjectTemplate> global = GetGlobalObjectTemplate(snap_isolate);
				v8::Local<v8::Context> context = v8::Context::New(snap_isolate, nullptr, global);
				{
					v8::Context::Scope context_scope(context);

					if (!boot_script_src.empty())
					{
						v8::TryCatch tc(snap_isolate);
						v8::Local<v8::String> s = v8::String::NewFromUtf8(
							snap_isolate, boot_script_src.data(),
							v8::NewStringType::kNormal,
							(int) boot_script_src.size()).ToLocalChecked();
						v8::Local<v8::Script> sc;
						v8::Local<v8::Value> unused_res;
						if (!v8::Script::Compile(context, s).ToLocal(&sc) ||
							!sc->Run(context).ToLocal(&unused_res))
						{
							if (tc.HasTerminated() || watchdog_fired.load(std::memory_order_acquire))
							{
								snap_timed_out = true;
								snap_compile_error = "timed out compiling boot_script_file in snapshot";
							}
							else
							{
								snap_compile_error = "failed to compile or run boot_script_file in snapshot";
							}
						}
					}

					for (size_t idx = 0; snap_compile_error.empty() && idx < pending_fns.size(); idx++)
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
							snap_compile_error = "failed to compile function \"" +
								item.proname + "\" (" + std::to_string(item.slot.fn_oid) +
								") into V8 snapshot";
							break;
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
			snap_done.store(true, std::memory_order_release);
			watchdog.join();

			if (snap_compile_error.empty())
			{
				blob = creator.CreateBlob(
					v8::SnapshotCreator::FunctionCodeHandling::kKeep);
			}
		}
		delete snap_allocator;
	}

	if (!snap_compile_error.empty())
	{
		if (snap_timed_out)
		{
			ereport(ERROR,
					(errcode(ERRCODE_QUERY_CANCELED),
					 errmsg("%s", snap_compile_error.c_str())));
		}
		ereport(ERROR, (errmsg("%s", snap_compile_error.c_str())));
	}

	if (blob.data == nullptr || blob.raw_size <= 0 ||
		(size_t) blob.raw_size > PLV8_MAX_SNAPSHOT_BLOB_SIZE)
	{
		delete[] blob.data;
		ereport(ERROR, (errmsg("v8::SnapshotCreator::CreateBlob() failed or exceeded size limit")));
	}

	struct stat out_st;
	if (lstat(out_path, &out_st) == 0 && S_ISLNK(out_st.st_mode))
	{
		delete[] blob.data;
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("refusing to write snapshot to symlink \"%s\"", out_path)));
	}

	std::string tmp_path = std::string(out_path) + ".tmp." + std::to_string((long) MyProcPid);
	int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
	if (fd < 0)
	{
		delete[] blob.data;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open snapshot temporary file \"%s\": %m",
						tmp_path.c_str())));
	}

	Plv8SnapshotHeader hdr;
	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.magic, PLV8_SNAPSHOT_MAGIC, 8);
	hdr.version = PLV8_SNAPSHOT_VERSION;
	hdr.num_functions = static_cast<uint32>(compiled_slots.size());
	hdr.blob_size = static_cast<uint64>(blob.raw_size);
	hdr.db_oid = MyDatabaseId;
	strlcpy(hdr.v8_version, v8::V8::GetVersion(), sizeof(hdr.v8_version));

	size_t slots_bytes = compiled_slots.size() * sizeof(Plv8SnapshotSlot);
	pg_cryptohash_ctx *hash_ctx = pg_cryptohash_create(PG_SHA256);
	bool hash_ok = (hash_ctx != NULL && pg_cryptohash_init(hash_ctx) >= 0);
	if (hash_ok && slots_bytes > 0)
	{
		if (pg_cryptohash_update(hash_ctx,
								 reinterpret_cast<const uint8_t *>(compiled_slots.data()),
								 slots_bytes) < 0)
			hash_ok = false;
	}
	if (hash_ok)
	{
		if (pg_cryptohash_update(hash_ctx,
								 reinterpret_cast<const uint8_t *>(blob.data),
								 (size_t) blob.raw_size) < 0 ||
			pg_cryptohash_final(hash_ctx, hdr.sha256, sizeof(hdr.sha256)) < 0)
			hash_ok = false;
	}
	if (hash_ctx != NULL)
		pg_cryptohash_free(hash_ctx);
	if (!hash_ok)
	{
		delete[] blob.data;
		close(fd);
		unlink(tmp_path.c_str());
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to compute SHA-256 checksum for snapshot file")));
	}

	bool write_ok = true;
	if (write(fd, &hdr, sizeof(hdr)) != (ssize_t) sizeof(hdr))
		write_ok = false;

	if (write_ok && slots_bytes > 0)
	{
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
				if (w < 0 && errno == EINTR)
					continue;
				write_ok = false;
				break;
			}
			p += w;
			rem -= (size_t) w;
		}
	}

	if (write_ok && fsync(fd) != 0)
		write_ok = false;

	int raw_size = blob.raw_size;
	delete[] blob.data;
	if (close(fd) != 0)
		write_ok = false;

	if (!write_ok || rename(tmp_path.c_str(), out_path) != 0)
	{
		unlink(tmp_path.c_str());
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("failed to write snapshot file \"%s\"", out_path)));
	}

	StringInfoData buf;
	initStringInfo(&buf);
	appendStringInfoString(&buf, "{\"status\":\"ok\",\"path\":");
	plv8_append_json_string(&buf, out_path);
	appendStringInfo(&buf,
					 ",\"bytes\":%d,\"bytes_written\":%zu,\"functions_compiled\":%zu,\"snapshotted_js_functions\":%zu,\"prewarmed_wasm_functions\":%d,\"v8_version\":",
					 raw_size,
					 sizeof(hdr) + slots_bytes + (size_t) raw_size,
					 compiled_slots.size(),
					 compiled_slots.size(),
					 prewarmed_wasm_fns);
	plv8_append_json_string(&buf, v8::V8::GetVersion());
	appendStringInfoChar(&buf, '}');

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
		bool				preserve_numeric = false;
		char			   *source_text = plv8_transpile_src(
										codeblock->source_text,
										codeblock->langOid,
										NULL, NULL, NULL,
										&preserve_numeric);

		Local<Function>	function = CompileFunction(context,
										NULL, 0, NULL,
										source_text, false, false);
		plv8_exec_env	   *xenv = CreateExecEnv(function, context);
		struct PreserveNumericScope {
			bool prev;
			explicit PreserveNumericScope(bool val) : prev(plv8_preserve_numeric_precision) {
				plv8_preserve_numeric_precision = val;
			}
			~PreserveNumericScope() {
				plv8_preserve_numeric_precision = prev;
			}
		} numeric_scope(preserve_numeric);
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
		return;
	}
#if PG_VERSION_NUM < 150000
	enable_timeout_after(plv8_timeout_id, 25);
#endif
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
#if PG_VERSION_NUM >= 150000
			enable_timeout_every(plv8_timeout_id,
								 TimestampTzPlusMilliseconds(GetCurrentTimestamp(), 25),
								 25);
#else
			enable_timeout_after(plv8_timeout_id, 25);
#endif
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
			bool has_concrete_expected_desc =
				(rsinfo->expectedDesc != NULL &&
				 rsinfo->expectedDesc->natts > 0 &&
				 TupleDescAttr(rsinfo->expectedDesc, 0)->atttypid != RECORDOID);
			if (proc->functypclass != TYPEFUNC_COMPOSITE &&
				proc->functypclass != TYPEFUNC_COMPOSITE_DOMAIN &&
				!has_concrete_expected_desc)
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
			conv.ToDatum(Plv8GetElement(xenv->isolate, context, array, i), tupstore);
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

	Oid prolang = proc->prolang;
	bool proretset = proc->proretset;
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

	bool is_custom_lang = false;
	if (OidIsValid(prolang))
	{
		NameData plv8_lang_name = {"plv8"};
		HeapTuple plv8Tup = SearchSysCache1(LANGNAME, NameGetDatum(&plv8_lang_name));
		if (HeapTupleIsValid(plv8Tup))
		{
			Oid plv8_lang_oid = ((Form_pg_language) GETSTRUCT(plv8Tup))->oid;
			ReleaseSysCache(plv8Tup);
			if (prolang != plv8_lang_oid)
				is_custom_lang = true;
		}
	}

	if (is_custom_lang && proretset)
		plv8_ensure_proc_support(fn_oid);

	if (!check_function_bodies && !(is_custom_lang && proretset))
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
plv8_transpile_src(const char *src, Oid lang_oid, plv8_handler_dep *deps, int *ndeps, plv8_proc *proc, bool *preserve_numeric_out)
{
	Oid				plv8_lang_oid = InvalidOid;
	Oid				lang_handler_oid = InvalidOid;
	char		   *result_cstr = NULL;
	bool			is_wasm_lang = false;
	bool			all_handlers_superuser = true;

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
				if (strcmp(NameStr(langStruct->lanname), "pl/v8-wasm") == 0)
					is_wasm_lang = true;
				if (!superuser_arg(langStruct->lanowner))
					all_handlers_superuser = false;
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
				if (!superuser_arg(procStruct->proowner))
					all_handlers_superuser = false;
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
	{
		if (proc != NULL && proc->cache != NULL)
		{
			proc->cache->pass_user_types_as_bytes = false;
			proc->cache->preserve_numeric_precision = false;
		}
		if (preserve_numeric_out != NULL)
			*preserve_numeric_out = false;
		return result_cstr;
	}

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
	/*
	 * The handler always receives a meta object as arguments[1]; for inline
	 * code blocks (DO) it only carries { inline: true } so the handler can
	 * still report per-routine options such as preserve_numeric_precision.
	 */
	Local<Object> meta = Object::New(isolate);
	args[1] = meta;
	call_argc = 2;
	if (proc != NULL && proc->cache != NULL)
	{
		plv8_proc_cache *cache = proc->cache;
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
	}
	else
	{
		meta->Set(context, ToString("inline"), v8::Boolean::New(isolate, true)).Check();
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

	{
		bool explicit_pass_bytes = false;
		bool preserve_numeric = false;
		Local<v8::Value> opt_val;
		if (meta->Get(context, ToString("pass_user_types_as_bytes")).ToLocal(&opt_val) &&
			!opt_val.IsEmpty() && opt_val->BooleanValue(isolate))
		{
			explicit_pass_bytes = true;
		}
		bool owner_is_superuser = all_handlers_superuser &&
			(proc == NULL || proc->cache == NULL || superuser_arg(proc->cache->proowner));
		if (explicit_pass_bytes && !owner_is_superuser)
		{
			PG_TRY();
			{
				ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("must be superuser to enable pass_user_types_as_bytes")));
			}
			PG_CATCH();
			{
				throw pg_error();
			}
			PG_END_TRY();
		}
		bool pass_bytes = (is_wasm_lang || explicit_pass_bytes) &&
						  owner_is_superuser;
		/*
		 * preserve_numeric_precision: numeric values that cannot round-trip
		 * through a JS double are converted to their exact text form instead
		 * of a lossy Number (see ToScalarValue in plv8_type.cc).
		 */
		if (meta->Get(context, ToString("preserve_numeric_precision")).ToLocal(&opt_val) &&
			!opt_val.IsEmpty() && opt_val->BooleanValue(isolate))
		{
			preserve_numeric = true;
		}
		if (proc != NULL && proc->cache != NULL)
		{
			proc->cache->pass_user_types_as_bytes = pass_bytes;
			proc->cache->preserve_numeric_precision = preserve_numeric;
		}
		if (preserve_numeric_out != NULL)
			*preserve_numeric_out = preserve_numeric;
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

	uint64 cur_tle_fp = plv8_tle_modules_fingerprint();

	if (found)
	{
		bool	uptodate;
		Form_pg_proc curProcStruct = (Form_pg_proc) GETSTRUCT(procTup);

		/*
		 * We need to check user id and dispose it if it's different from
		 * the previous cache user id, as the V8 function is associated
		 * with the context where it was generated.  In most cases,
		 * we can expect this doesn't affect runtime performance.
		 */
		uptodate = (!cache->function.IsEmpty() &&
			cache->fn_xmin == HeapTupleHeaderGetXmin(procTup->t_data) &&
			ItemPointerEquals(&cache->fn_tid, &procTup->t_self) &&
			cache->user_id == current_context->user_id &&
			cache->proowner == curProcStruct->proowner &&
			cache->prolang == curProcStruct->prolang &&
			cache->prosecdef == curProcStruct->prosecdef &&
			cache->tle_modules_fp == cur_tle_fp);

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
			cache->pass_user_types_as_bytes = false;
			cache->preserve_numeric_precision = false;
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
		cache->pass_user_types_as_bytes = false;
		cache->preserve_numeric_precision = false;
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
		cache->proowner = procStruct->proowner;
		cache->prolang = procStruct->prolang;
		cache->prosecdef = procStruct->prosecdef;
		cache->tle_modules_fp = cur_tle_fp;
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
		plv8_snapshot_slots.empty() ||
		(OidIsValid(plv8_snapshot_db_oid) && plv8_snapshot_db_oid != MyDatabaseId))
	{
		return false;
	}

	for (size_t idx = 0; idx < plv8_snapshot_slots.size(); idx++)
	{
		const Plv8SnapshotSlot &slot = plv8_snapshot_slots[idx];
		if (slot.fn_oid != cache->fn_oid)
			continue;

		if (!TransactionIdEquals(slot.fn_xmin, cache->fn_xmin) ||
			!ItemPointerEquals(const_cast<ItemPointer>(&slot.fn_tid), &cache->fn_tid) ||
			slot.proowner != cache->proowner ||
			slot.prolang != cache->prolang ||
			slot.prosecdef != cache->prosecdef)
		{
			return false;
		}

		if (idx >= global_context->snapshot_functions.size() ||
			global_context->snapshot_functions[idx].IsEmpty())
		{
			return false;
		}

		if (slot.nhandlers > 0)
		{
			if (slot.nhandlers > PLV8_MAX_LANG_HANDLER_DEPTH)
				return false;
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
			Local<Function> fn = global_context->snapshot_functions[idx].Get(isolate);
			if (fn.IsEmpty())
			{
				cache->nhandlers = 0;
				return false;
			}
			cache->function.Reset(isolate, fn);
		}

		cache->pass_user_types_as_bytes = slot.pass_user_types_as_bytes &&
			superuser_arg(cache->proowner);
		cache->preserve_numeric_precision = slot.preserve_numeric_precision;
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
		{
			memcpy(cache->handlers, deps, sizeof(plv8_handler_dep) * ndeps);
			if (cache->retset)
			{
				PG_TRY();
				{
					plv8_ensure_proc_support(fn_oid);
					HeapTuple updTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
					if (HeapTupleIsValid(updTup))
					{
						cache->fn_xmin = HeapTupleHeaderGetXmin(updTup->t_data);
						cache->fn_tid = updTup->t_self;
						ReleaseSysCache(updTup);
					}
				}
				PG_CATCH();
				{
					throw pg_error();
				}
				PG_END_TRY();
			}
		}
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
	Local<Context> context = Local<Context>::New(isolate, global_context->context);
	Context::Scope	context_scope(context);
	StringInfoData	sig_buf;

	initStringInfo(&sig_buf);
	if (is_trigger)
	{
		if (proarglen != 0)
			throw js_error("trigger function cannot have arguments");
		// trigger function has special arguments.
		appendStringInfo(&sig_buf,
			"NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
			"TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
	}
	else
	{
		for (int i = 0; i < proarglen; i++)
		{
			if (i > 0)
				appendStringInfoChar(&sig_buf, ',');
			if (proargs && proargs[i])
				appendStringInfoString(&sig_buf, proargs[i]);
			else
				appendStringInfo(&sig_buf, "$%d", i + 1);	// unnamed argument to $N
		}
	}

	std::string hoisted_imports;
	std::string body_rest;
	if (plv8_extract_hoisted_imports(prosrc, &hoisted_imports, &body_rest))
	{
		StringInfoData mod_src;
		initStringInfo(&mod_src);
		appendStringInfo(&mod_src, "%s\nexport default (function (%s){\n%s\n});",
						 hoisted_imports.c_str(), sig_buf.data, body_rest.c_str());
		pfree(sig_buf.data);

		plv8_sync_tle_module_cache(global_context);
		Local<Object> ns = plv8_compile_and_eval_anon_module(context, proname, mod_src.data);
		pfree(mod_src.data);

		TryCatch try_catch(isolate);
		Local<v8::Value> def_val;
		if (!ns->Get(context, v8::String::NewFromUtf8Literal(isolate, "default")).ToLocal(&def_val) ||
			def_val.IsEmpty() || !def_val->IsFunction())
		{
			if (try_catch.HasCaught())
				throw js_error(try_catch);
			throw js_error("ES module function wrapper did not export a default Function");
		}
		return handle_scope.Escape(Local<Function>::Cast(def_val));
	}

	StringInfoData	src;
	initStringInfo(&src);
	appendStringInfo(&src, "(function (%s){\n%s\n})", sig_buf.data, prosrc);
	pfree(sig_buf.data);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined(isolate);
	Local<v8::String> source = ToString(src.data, src.len);
	pfree(src.data);

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

plv8_context*
GetPlv8Context() {
	Oid					user_id = GetUserId();
	unsigned int		i;
	plv8_context		*my_context = nullptr;

	if (current_context == nullptr && !plv8_bypass_snapshot_restore)
		SyncPlv8SnapshotFromGucs();

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
		new(&my_context->tle_module_map) std::unordered_map<std::string, v8::Global<v8::Module>>();
		new(&my_context->tle_module_id_to_spec) std::unordered_map<int, std::string>();
		new(&my_context->snapshot_functions) std::vector<v8::Global<v8::Function>>();
		my_context->tle_modules_fp = 0;
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
			{
				role_ctx = snap_ctx.ToLocalChecked();
				if (!plv8_snapshot_slots.empty())
				{
					Context::Scope snap_cs(role_ctx);
					Local<Object> global_obj = role_ctx->Global();
					my_context->snapshot_functions.resize(plv8_snapshot_slots.size());
					for (size_t sidx = 0; sidx < plv8_snapshot_slots.size(); sidx++)
					{
						char prop_buf[64];
						snprintf(prop_buf, sizeof(prop_buf), "__plv8_snap_fn_%zu", sidx);
						Local<v8::String> prop_name = v8::String::NewFromUtf8(
							isolate, prop_buf, v8::NewStringType::kInternalized).ToLocalChecked();
						Local<v8::Value> fn_val;
						if (global_obj->Get(role_ctx, prop_name).ToLocal(&fn_val) &&
							!fn_val.IsEmpty() && fn_val->IsFunction())
						{
							my_context->snapshot_functions[sidx].Reset(
								isolate, Local<Function>::Cast(fn_val));
						}
						(void) global_obj->Delete(role_ctx, prop_name).FromMaybe(false);
					}
				}
			}
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

		new(&my_context->plv8_obj) Persistent<Object>();
		{
			Context::Scope role_cs(role_ctx);
			char sec_buf[64];
			snprintf(sec_buf, sizeof(sec_buf), "plv8_role_%u", (unsigned int) user_id);
			Local<v8::String> sec_tok = v8::String::NewFromUtf8(
				isolate, sec_buf, NewStringType::kInternalized).ToLocalChecked();
			role_ctx->SetSecurityToken(sec_tok);
			Local<v8::Value> plv8_val;
			if (role_ctx->Global()->Get(
					role_ctx,
					v8::String::NewFromUtf8Literal(
						isolate, "plv8", NewStringType::kInternalized)).ToLocal(&plv8_val) &&
				!plv8_val.IsEmpty() && plv8_val->IsObject())
			{
				my_context->plv8_obj.Reset(isolate, Local<Object>::Cast(plv8_val));
			}
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

		new(&my_context->plan_ftmpl) Persistent<FunctionTemplate>();
		new(&my_context->plan_template) Persistent<ObjectTemplate>();
		Local<FunctionTemplate> base = FunctionTemplate::New(isolate);
		Local<v8::String> planClassName = v8::String::NewFromUtf8Literal(isolate, "PreparedPlan",
														   NewStringType::kInternalized);
		base->SetClassName(planClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, planClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupPrepFunctions(templ);
		my_context->plan_ftmpl.Reset(isolate, base);
		my_context->plan_template.Reset(isolate, templ);

		new(&my_context->cursor_ftmpl) Persistent<FunctionTemplate>();
		new(&my_context->cursor_template) Persistent<ObjectTemplate>();
		base = FunctionTemplate::New(isolate);
		Local<v8::String> cursorClassName = v8::String::NewFromUtf8Literal(isolate, "Cursor",
															 NewStringType::kInternalized);
		base->SetClassName(cursorClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, cursorClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupCursorFunctions(templ);
		my_context->cursor_ftmpl.Reset(isolate, base);
		my_context->cursor_template.Reset(isolate, templ);

		new(&my_context->window_ftmpl) Persistent<FunctionTemplate>();
		new(&my_context->window_template) Persistent<ObjectTemplate>();
		base = FunctionTemplate::New(isolate);
		Local<v8::String> windowClassName = v8::String::NewFromUtf8Literal(isolate, "WindowObject",
															 NewStringType::kInternalized);
        base->SetClassName(windowClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, windowClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupWindowFunctions(templ);
		my_context->window_ftmpl.Reset(isolate, base);
		my_context->window_template.Reset(isolate, templ);
		/*
		 * Need to register it before running any code, as the code
		 * recursively may want to the global context.
		 */
		ContextVector.push_back(my_context);
		plv8_fdw_init_context(my_context);

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
	SetupGlobalFunctions(templ);
	PropertyAttribute attrFull =
		static_cast<PropertyAttribute>(v8::ReadOnly | v8::DontEnum | v8::DontDelete);
	plv8->Set(v8::String::NewFromUtf8Literal(isolate, "version", NewStringType::kInternalized),
			  v8::String::NewFromUtf8Literal(isolate, PLV8_VERSION), attrFull);
	plv8->Set(v8::String::NewFromUtf8Literal(isolate, "v8_version", NewStringType::kInternalized),
			  v8::String::NewFromUtf8Literal(isolate, V8_VERSION_STRING), attrFull);

	templ->Set(v8::String::NewFromUtf8Literal(isolate, "plv8", NewStringType::kInternalized), plv8, attrFull);

	return handle_scope.Escape(templ);
}

/*
 * Accessor to plv8_type stored in fcinfo.
 */
plv8_type *
get_plv8_type(PG_FUNCTION_ARGS, int argno)
{
	if (fcinfo == NULL || fcinfo->flinfo == NULL || fcinfo->flinfo->fn_extra == NULL)
		throw js_error("window function api called with wrong object");
	plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	if (proc->cache == NULL || argno < 0 || argno >= proc->cache->nargs)
		throw js_error("argument number out of range");
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
	Local<Array>	numeric_cols;
	uint32_t		nnumeric = 0;

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Datum		datum;
		bool		isnull;

		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
			continue;

		datum = heap_getattr(tuple, c + 1, m_tupdesc, &isnull);

		TryCatch try_catch(isolate);
		if (!obj->CreateDataProperty(context, m_colnames[c], ::ToValue(datum, isnull, &m_coltypes[c])).FromMaybe(false))
		{
			if (try_catch.HasCaught())
				throw js_error(try_catch);
			throw js_error("failed to set tuple property");
		}

		/*
		 * In preserve_numeric_precision mode numeric columns may be delivered
		 * as exact text; record which columns are numeric in a hidden,
		 * non-enumerable property so callers can tell them from strings.
		 */
		if (plv8_preserve_numeric_precision && m_coltypes[c].typid == NUMERICOID)
		{
			if (numeric_cols.IsEmpty())
				numeric_cols = Array::New(isolate);
			numeric_cols->CreateDataProperty(context, nnumeric++, m_colnames[c]).Check();
		}
	}

	if (!numeric_cols.IsEmpty())
		obj->DefineOwnProperty(context, ToString("__numeric_columns"), numeric_cols,
							   static_cast<PropertyAttribute>(ReadOnly | DontEnum | DontDelete)).Check();

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
		if (value.IsEmpty() || !value->IsObject())
			throw js_error("argument must be an object");
		obj = Handle<Object>::Cast(value);
		if (obj.IsEmpty())
			throw js_error(try_catch);

		Handle<Array> names;
		if (!obj->GetPropertyNames(context).ToLocal(&names))
		{
			if (try_catch.HasCaught())
				throw js_error(try_catch);
			throw js_error("failed to get property names");
		}

		uint32_t nnames = names->Length();
		for (int c = 0; c < m_tupdesc->natts; c++)
		{
			if (TupleDescAttr(m_tupdesc, c)->attisdropped)
				continue;

			bool found = false;
			CString  colname(m_colnames[c]);
			for (uint32_t d = 0; d < (uint32_t) m_tupdesc->natts && d < nnames; d++)
			{
				CString fname(Plv8GetElement(isolate, context, names, d));
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

	/*
	 * Use vector<char> instead of vector<bool> because <bool> version is
	 * s specialized and different from bool[].
	 */
	Datum  *values = (Datum *) palloc(sizeof(Datum) * m_tupdesc->natts);
	bool   *nulls = (bool *) palloc(sizeof(bool) * m_tupdesc->natts);

	try
	{
		for (int c = 0; c < m_tupdesc->natts; c++)
		{
			/* Make sure dropped columns are skipped by backend code. */
			if (TupleDescAttr(m_tupdesc, c)->attisdropped)
			{
				nulls[c] = true;
				continue;
			}

			Handle<v8::Value> attr = m_is_scalar ? value : Plv8GetProperty(isolate, context, obj, m_colnames[c]);
			if (attr.IsEmpty() || attr->IsUndefined() || attr->IsNull())
				nulls[c] = true;
			else
				values[c] = ::ToDatum(attr, &nulls[c], &m_coltypes[c]);
		}
	}
	catch (...)
	{
		pfree(values);
		pfree(nulls);
		throw;
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
	: m_msg(nullptr),
	  m_code(0),
	  m_detail(nullptr),
	  m_hint(nullptr),
	  m_context(nullptr),
	  m_schema_name(nullptr),
	  m_table_name(nullptr),
	  m_column_name(nullptr),
	  m_datatype_name(nullptr),
	  m_constraint_name(nullptr)
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

static int
plv8_parse_sqlerrcode(v8::Local<v8::Context> context, v8::Local<v8::Value> val)
{
	if (val.IsEmpty() || val->IsUndefined() || val->IsNull())
		return 0;
	if (val->IsNumber())
	{
		int32_t code;
		if (val->Int32Value(context).To(&code))
			return code;
	}
	if (val->IsString())
	{
		CString str(val);
		const char *s = str.str();
		if (s != NULL && strlen(s) == 5)
		{
			bool valid = true;
			char c[5];
			for (int i = 0; i < 5; i++)
			{
				unsigned char ch = (unsigned char) s[i];
				if (ch >= '0' && ch <= '9')
					c[i] = (char) ch;
				else if (ch >= 'A' && ch <= 'Z')
					c[i] = (char) ch;
				else if (ch >= 'a' && ch <= 'z')
					c[i] = (char) (ch - 'a' + 'A');
				else
				{
					valid = false;
					break;
				}
			}
			if (valid)
				return MAKE_SQLSTATE(c[0], c[1], c[2], c[3], c[4]);
		}
	}
	return 0;
}

static char *
plv8_extract_err_string_field(v8::Isolate *isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> err,
                              const char *field_name)
{
	v8::Local<v8::Value> val;
	if (err->Get(context, v8::String::NewFromUtf8(isolate, field_name).ToLocalChecked()).ToLocal(&val))
	{
		if (!val.IsEmpty() && !val->IsUndefined() && !val->IsNull())
		{
			CString cstr(val);
			if (cstr.str() != NULL)
				return pstrdup(cstr.str());
		}
	}
	return nullptr;
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
                    m_code = plv8_parse_sqlerrcode(context, errCode);
                }
                if (m_code == 0)
                {
                    v8::Local<v8::Value> sqlErrCode;
                    if (err->Get(context,
                                 v8::String::NewFromUtf8Literal(isolate, "sqlerrcode")).ToLocal(&sqlErrCode))
                    {
                        m_code = plv8_parse_sqlerrcode(context, sqlErrCode);
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

                m_schema_name = plv8_extract_err_string_field(isolate, context, err, "schema_name");
                m_table_name = plv8_extract_err_string_field(isolate, context, err, "table_name");
                m_column_name = plv8_extract_err_string_field(isolate, context, err, "column_name");
                m_datatype_name = plv8_extract_err_string_field(isolate, context, err, "datatype_name");
                m_constraint_name = plv8_extract_err_string_field(isolate, context, err, "constraint_name");
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
	Local<v8::Value> err_val = Exception::Error(message);
	Isolate *isolate = Isolate::GetCurrent();
	if (isolate != NULL)
	{
		Local<Context> context = isolate->GetCurrentContext();
		Local<v8::Object> err;
		if (!context.IsEmpty() && err_val->ToObject(context).ToLocal(&err) && !err.IsEmpty())
		{
			if (m_code)
			{
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "code"), Int32::New(isolate, m_code)).FromMaybe(false);
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "sqlerrcode"), ToString(unpack_sql_state(m_code))).FromMaybe(false);
			}
			if (m_detail)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "detail"), ToString(m_detail)).FromMaybe(false);
			if (m_hint)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "hint"), ToString(m_hint)).FromMaybe(false);
			if (m_schema_name)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "schema_name"), ToString(m_schema_name)).FromMaybe(false);
			if (m_table_name)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "table_name"), ToString(m_table_name)).FromMaybe(false);
			if (m_column_name)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "column_name"), ToString(m_column_name)).FromMaybe(false);
			if (m_datatype_name)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "datatype_name"), ToString(m_datatype_name)).FromMaybe(false);
			if (m_constraint_name)
				(void)err->Set(context, v8::String::NewFromUtf8Literal(isolate, "constraint_name"), ToString(m_constraint_name)).FromMaybe(false);
		}
	}
	return err_val;
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
					m_context ? errcontext("%s", m_context) : 0,
					m_schema_name ? err_generic_string(PG_DIAG_SCHEMA_NAME, m_schema_name) : 0,
					m_table_name ? err_generic_string(PG_DIAG_TABLE_NAME, m_table_name) : 0,
					m_column_name ? err_generic_string(PG_DIAG_COLUMN_NAME, m_column_name) : 0,
					m_datatype_name ? err_generic_string(PG_DIAG_DATATYPE_NAME, m_datatype_name) : 0,
					m_constraint_name ? err_generic_string(PG_DIAG_CONSTRAINT_NAME, m_constraint_name) : 0
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
					m_context ? errcontext("%s", m_context) : 0,
					m_schema_name ? err_generic_string(PG_DIAG_SCHEMA_NAME, m_schema_name) : 0,
					m_table_name ? err_generic_string(PG_DIAG_TABLE_NAME, m_table_name) : 0,
					m_column_name ? err_generic_string(PG_DIAG_COLUMN_NAME, m_column_name) : 0,
					m_datatype_name ? err_generic_string(PG_DIAG_DATATYPE_NAME, m_datatype_name) : 0,
					m_constraint_name ? err_generic_string(PG_DIAG_CONSTRAINT_NAME, m_constraint_name) : 0
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

/* =========================================================================
 * pg_tle Module Loader & V8 CodeCache Support
 * ========================================================================= */

static const char PLV8_BYTECODE_MAGIC[8] = {'P', 'L', 'V', '8', 'B', 'C', '0', '1'};

static void plv8_modules_inval_cb(Datum arg, Oid relid) {
  plv8_cached_tle_fp_valid = false;
  if (!plv8_in_internal_catalog_update &&
      (relid == InvalidOid || relid != plv8_cached_holder_relid)) {
    plv8_catalog_gen++;
  }
  if (relid == InvalidOid ||
      (OidIsValid(plv8_cached_modules_relid) &&
       relid == plv8_cached_modules_relid) ||
      (OidIsValid(plv8_cached_packages_relid) &&
       relid == plv8_cached_packages_relid)) {
    for (size_t i = 0; i < ContextVector.size(); i++) {
      if (ContextVector[i] != nullptr) {
        ContextVector[i]->tle_modules_fp = 0;
      }
    }
    if (relid == InvalidOid) {
      plv8_cached_modules_relid = InvalidOid;
      plv8_cached_packages_relid = InvalidOid;
    }
  }
}

Datum plv8_modules_changed(PG_FUNCTION_ARGS) {
  if (!CALLED_AS_TRIGGER(fcinfo))
    elog(ERROR, "plv8_modules_changed: not called by trigger manager");
  TriggerData* trigdata = (TriggerData*)fcinfo->context;
  CacheInvalidateRelcache(trigdata->tg_relation);
  return PointerGetDatum(NULL);
}

static uint64 plv8_fnv1a64(uint64 h, const void* data, size_t len) {
  const unsigned char* p = (const unsigned char*)data;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64)p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

static AclResult plv8_namespace_aclcheck(Oid nspoid, Oid userid, AclMode mode) {
#if PG_VERSION_NUM >= 160000
  return object_aclcheck(NamespaceRelationId, nspoid, userid, mode);
#else
  return pg_namespace_aclcheck(nspoid, userid, mode);
#endif
}

static uint64 plv8_hash_catalog_table(Oid relid) {
  if (!OidIsValid(relid)) return 0;
  if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
    return 0;

  uint64 fp = 0;
  Relation rel = table_open(relid, AccessShareLock);
  TableScanDesc scan = table_beginscan_catalog(rel, 0, NULL);
  HeapTuple tuple;

  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
    uint64 row_hash = 14695981039346656037ULL;
    ItemPointerData tid = tuple->t_self;
    TransactionId xmin = HeapTupleHeaderGetRawXmin(tuple->t_data);
    CommandId cid = HeapTupleHeaderGetRawCommandId(tuple->t_data);

    CHECK_FOR_INTERRUPTS();

    row_hash = plv8_fnv1a64(row_hash, &tid, sizeof(tid));
    row_hash = plv8_fnv1a64(row_hash, &xmin, sizeof(xmin));
    row_hash = plv8_fnv1a64(row_hash, &cid, sizeof(cid));

    fp ^= row_hash;
  }

  table_endscan(scan);
  table_close(rel, AccessShareLock);
  return fp;
}

uint64 plv8_tle_modules_fingerprint(void) {
  if (!IsTransactionState()) return 0;

  AcceptInvalidationMessages();

  Oid user_id = GetUserId();
  TransactionId cur_xid = GetCurrentTransactionIdIfAny();
  SubTransactionId cur_subid = GetCurrentSubTransactionId();
  CommandId cur_cid = GetCurrentCommandId(false);

  if (plv8_cached_tle_fp_valid &&
      plv8_cached_tle_user_id == user_id &&
      plv8_cached_tle_xid == cur_xid &&
      plv8_cached_tle_subid == cur_subid &&
      plv8_cached_tle_cid == cur_cid) {
    return plv8_cached_tle_fp;
  }

  Oid nspoid = get_namespace_oid("plv8", true);
  if (!OidIsValid(nspoid)) {
    plv8_cached_tle_user_id = user_id;
    plv8_cached_tle_xid = cur_xid;
    plv8_cached_tle_subid = cur_subid;
    plv8_cached_tle_cid = cur_cid;
    plv8_cached_tle_fp = 0;
    plv8_cached_tle_fp_valid = true;
    return 0;
  }

  Oid mod_relid = get_relname_relid("modules", nspoid);
  Oid pkg_relid = get_relname_relid("packages", nspoid);
  if (!OidIsValid(mod_relid)) {
    plv8_cached_tle_user_id = user_id;
    plv8_cached_tle_xid = cur_xid;
    plv8_cached_tle_subid = cur_subid;
    plv8_cached_tle_cid = cur_cid;
    plv8_cached_tle_fp = 0;
    plv8_cached_tle_fp_valid = true;
    return 0;
  }

  plv8_cached_modules_relid = mod_relid;
  plv8_cached_packages_relid = pkg_relid;

  if (plv8_namespace_aclcheck(nspoid, user_id, ACL_USAGE) != ACLCHECK_OK ||
      pg_class_aclcheck(mod_relid, user_id, ACL_SELECT) != ACLCHECK_OK) {
    plv8_cached_tle_user_id = user_id;
    plv8_cached_tle_xid = cur_xid;
    plv8_cached_tle_subid = cur_subid;
    plv8_cached_tle_cid = cur_cid;
    plv8_cached_tle_fp = 0;
    plv8_cached_tle_fp_valid = true;
    return 0;
  }

  uint64 fp = 14695981039346656037ULL;
  fp ^= plv8_hash_catalog_table(mod_relid);
  if (OidIsValid(pkg_relid) &&
      pg_class_aclcheck(pkg_relid, user_id, ACL_SELECT) == ACLCHECK_OK) {
    fp ^= (plv8_hash_catalog_table(pkg_relid) * 1099511628211ULL);
  }
  if (check_enable_rls(mod_relid, InvalidOid, true) == RLS_ENABLED ||
      (OidIsValid(pkg_relid) &&
       check_enable_rls(pkg_relid, InvalidOid, true) == RLS_ENABLED)) {
    fp = plv8_fnv1a64(fp, &user_id, sizeof(user_id));
  }
  if (fp == 0) fp = 1;

  plv8_cached_tle_user_id = user_id;
  plv8_cached_tle_xid = cur_xid;
  plv8_cached_tle_subid = cur_subid;
  plv8_cached_tle_cid = cur_cid;
  plv8_cached_tle_fp = fp;
  plv8_cached_tle_fp_valid = true;
  return fp;
}

static void plv8_sync_tle_module_cache(plv8_context* ctx) {
  uint64 cur_fp;

  if (!ctx) return;

  cur_fp = plv8_tle_modules_fingerprint();
  if (cur_fp == 0 || ctx->tle_modules_fp != cur_fp) {
    for (auto& kv : ctx->tle_module_map) kv.second.Reset();
    ctx->tle_module_map.clear();
    ctx->tle_module_id_to_spec.clear();
    ctx->tle_modules_fp = cur_fp;
  }
}

static bool plv8_extract_hoisted_imports(const char* src,
                                         std::string* imports_out,
                                         std::string* body_out) {
  const char* p = src;
  bool at_line_start = true;
  int brace_depth = 0;
  bool found_esm = false;

  imports_out->clear();
  body_out->clear();

  while (*p) {
    if (at_line_start) {
      const char* ws = p;
      while (*ws == ' ' || *ws == '\t' || *ws == '\r') ws++;

      if (brace_depth == 0 && strncmp(ws, "import", 6) == 0 &&
          (ws[6] == ' ' || ws[6] == '\t' || ws[6] == '{' || ws[6] == '*' ||
           ws[6] == '"' || ws[6] == '\'')) {
        const char* stmt_start = ws;
        const char* cur = ws + 6;
        char in_quote = 0;

        while (*cur) {
          if (in_quote) {
            if (*cur == '\\' && cur[1])
              cur += 2;
            else if (*cur == in_quote) {
              in_quote = 0;
              cur++;
            } else
              cur++;
          } else {
            if (*cur == '\'' || *cur == '"' || *cur == '`') {
              in_quote = *cur;
              cur++;
            } else if (*cur == ';') {
              cur++;
              break;
            } else if (*cur == '\n') {
              const char* back = cur - 1;
              while (back > stmt_start &&
                     (*back == ' ' || *back == '\t' || *back == '\r'))
                back--;
              if (*back == '\'' || *back == '"') {
                cur++;
                break;
              }
              cur++;
            } else
              cur++;
          }
        }

        imports_out->append(stmt_start, cur - stmt_start);
        imports_out->push_back('\n');
        found_esm = true;
        p = cur;
        at_line_start = true;
        continue;
      } else if (brace_depth == 0 && strncmp(ws, "export", 6) == 0 &&
                 (ws[6] == ' ' || ws[6] == '\t')) {
        const char* after_export = ws + 6;
        while (*after_export == ' ' || *after_export == '\t') after_export++;
        if (strncmp(after_export, "default", 7) == 0 &&
            (after_export[7] == ' ' || after_export[7] == '\t')) {
          body_out->append("return ");
          found_esm = true;
          p = after_export + 7;
          at_line_start = false;
          continue;
        } else if (strncmp(after_export, "function", 8) == 0 ||
                   strncmp(after_export, "const", 5) == 0 ||
                   strncmp(after_export, "let", 3) == 0 ||
                   strncmp(after_export, "var", 3) == 0 ||
                   strncmp(after_export, "class", 5) == 0) {
          found_esm = true;
          p = after_export;
          at_line_start = false;
          continue;
        }
      }
    }

    if (*p == '/' && p[1] == '/') {
      while (*p && *p != '\n') body_out->push_back(*p++);
      if (*p == '\n') {
        body_out->push_back(*p++);
        at_line_start = true;
      }
      continue;
    }
    if (*p == '/' && p[1] == '*') {
      body_out->push_back(*p++);
      body_out->push_back(*p++);
      while (*p && !(*p == '*' && p[1] == '/')) body_out->push_back(*p++);
      if (*p) {
        body_out->push_back(*p++);
        body_out->push_back(*p++);
      }
      continue;
    }
    if (*p == '\'' || *p == '"' || *p == '`') {
      char q = *p;
      body_out->push_back(*p++);
      while (*p && *p != q) {
        if (*p == '\\' && p[1]) body_out->push_back(*p++);
        body_out->push_back(*p++);
      }
      if (*p == q) body_out->push_back(*p++);
      at_line_start = false;
      continue;
    }

    if (*p == '{')
      brace_depth++;
    else if (*p == '}' && brace_depth > 0)
      brace_depth--;

    if (*p == '\n')
      at_line_start = true;
    else if (*p != ' ' && *p != '\t' && *p != '\r')
      at_line_start = false;

    body_out->push_back(*p++);
  }

  return found_esm;
}

/* ---------- Semver Parsing & Range Evaluation ---------- */

struct Plv8Semver {
  int major;
  int minor;
  int patch;
};

static bool plv8_semver_parse(const std::string& str, Plv8Semver* out) {
  if (str.empty()) return false;
  size_t pos = 0;
  int parts[3] = {0, 0, 0};

  for (int i = 0; i < 3; i++) {
    if (pos >= str.size() || str[pos] < '0' || str[pos] > '9') return false;
    if (str[pos] == '0' && pos + 1 < str.size() && str[pos + 1] >= '0' &&
        str[pos + 1] <= '9') {
      return false;  // leading zero not allowed in strict semver
    }
    long long val = 0;
    while (pos < str.size() && str[pos] >= '0' && str[pos] <= '9') {
      val = val * 10 + (str[pos] - '0');
      if (val > 2147483647LL) return false;
      pos++;
    }
    parts[i] = (int)val;
    if (i < 2) {
      if (pos >= str.size() || str[pos] != '.') return false;
      pos++;
    }
  }

  if (pos < str.size()) {
    // Allow optional -prerelease or +build suffix for compatibility
    if (str[pos] != '-' && str[pos] != '+') return false;
  }

  out->major = parts[0];
  out->minor = parts[1];
  out->patch = parts[2];
  return true;
}

static int plv8_semver_cmp(const Plv8Semver& a, const Plv8Semver& b) {
  if (a.major != b.major) return (a.major > b.major) ? 1 : -1;
  if (a.minor != b.minor) return (a.minor > b.minor) ? 1 : -1;
  if (a.patch != b.patch) return (a.patch > b.patch) ? 1 : -1;
  return 0;
}

static int plv8_compare_semver(const std::string& a, const std::string& b) {
  size_t ia = 0, ib = 0;

  while (ia < a.size() || ib < b.size()) {
    if (ia < a.size() && ib < b.size() && a[ia] >= '0' && a[ia] <= '9' &&
        b[ib] >= '0' && b[ib] <= '9') {
      unsigned long long va = 0, vb = 0;
      while (ia < a.size() && a[ia] >= '0' && a[ia] <= '9') {
        va = va * 10ULL + (unsigned long long)(a[ia] - '0');
        ia++;
      }
      while (ib < b.size() && b[ib] >= '0' && b[ib] <= '9') {
        vb = vb * 10ULL + (unsigned long long)(b[ib] - '0');
        ib++;
      }
      if (va != vb) return (va > vb) ? 1 : -1;
    } else {
      unsigned char ca = (ia < a.size()) ? (unsigned char)a[ia] : 0;
      unsigned char cb = (ib < b.size()) ? (unsigned char)b[ib] : 0;
      if (ca != cb) return (ca > cb) ? 1 : -1;
      if (ia < a.size()) ia++;
      if (ib < b.size()) ib++;
    }
    if (ia < a.size() && a[ia] == '.') ia++;
    if (ib < b.size() && b[ib] == '.') ib++;
  }
  return 0;
}

static std::string plv8_trim(const std::string& s) {
  size_t start = 0;
  while (start < s.size() &&
         (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n'))
    start++;
  size_t end = s.size();
  while (end > start &&
         (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' ||
          s[end - 1] == '\n'))
    end--;
  return s.substr(start, end - start);
}

static bool plv8_semver_satisfies_single(const Plv8Semver& ver,
                                         const std::string& token_raw,
                                         bool* valid_range_out) {
  std::string tok = plv8_trim(token_raw);
  if (tok.empty() || tok == "*") {
    *valid_range_out = true;
    return true;
  }

  if (tok[0] == '^') {
    Plv8Semver base;
    if (!plv8_semver_parse(plv8_trim(tok.substr(1)), &base)) {
      *valid_range_out = false;
      return false;
    }
    *valid_range_out = true;
    if (plv8_semver_cmp(ver, base) < 0) return false;
    Plv8Semver upper = {0, 0, 0};
    if (base.major > 0) {
      upper.major = base.major + 1;
    } else if (base.minor > 0) {
      upper.minor = base.minor + 1;
    } else {
      upper.patch = base.patch + 1;
    }
    return plv8_semver_cmp(ver, upper) < 0;
  }

  if (tok[0] == '~') {
    Plv8Semver base;
    if (!plv8_semver_parse(plv8_trim(tok.substr(1)), &base)) {
      *valid_range_out = false;
      return false;
    }
    *valid_range_out = true;
    if (plv8_semver_cmp(ver, base) < 0) return false;
    Plv8Semver upper = {base.major, base.minor + 1, 0};
    return plv8_semver_cmp(ver, upper) < 0;
  }

  if (tok.rfind(">=", 0) == 0) {
    Plv8Semver base;
    if (!plv8_semver_parse(plv8_trim(tok.substr(2)), &base)) {
      *valid_range_out = false;
      return false;
    }
    *valid_range_out = true;
    return plv8_semver_cmp(ver, base) >= 0;
  }

  if (tok.rfind("<=", 0) == 0) {
    Plv8Semver base;
    if (!plv8_semver_parse(plv8_trim(tok.substr(2)), &base)) {
      *valid_range_out = false;
      return false;
    }
    *valid_range_out = true;
    return plv8_semver_cmp(ver, base) <= 0;
  }

  if (tok[0] == '>') {
    Plv8Semver base;
    if (!plv8_semver_parse(plv8_trim(tok.substr(1)), &base)) {
      *valid_range_out = false;
      return false;
    }
    *valid_range_out = true;
    return plv8_semver_cmp(ver, base) > 0;
  }

  if (tok[0] == '<') {
    Plv8Semver base;
    if (!plv8_semver_parse(plv8_trim(tok.substr(1)), &base)) {
      *valid_range_out = false;
      return false;
    }
    *valid_range_out = true;
    return plv8_semver_cmp(ver, base) < 0;
  }

  if (tok[0] == '=') {
    tok = plv8_trim(tok.substr(1));
  }

  // Check wildcard forms: 1.x, 1.*, 1.2.x, 1.2.*
  size_t dot1 = tok.find('.');
  if (dot1 != std::string::npos) {
    std::string s_maj = tok.substr(0, dot1);
    std::string rest = tok.substr(dot1 + 1);
    size_t dot2 = rest.find('.');
    std::string s_min =
        (dot2 == std::string::npos) ? rest : rest.substr(0, dot2);
    std::string s_pat =
        (dot2 == std::string::npos) ? "x" : rest.substr(dot2 + 1);

    auto is_wild = [](const std::string& s) {
      return s == "x" || s == "X" || s == "*";
    };
    auto parse_int = [](const std::string& s, int* v) {
      if (s.empty()) return false;
      long long n = 0;
      for (char c : s) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
        if (n > 2147483647LL) return false;
      }
      *v = (int)n;
      return true;
    };

    if (is_wild(s_min) || is_wild(s_pat)) {
      int maj = 0;
      if (!parse_int(s_maj, &maj)) {
        *valid_range_out = false;
        return false;
      }
      if (is_wild(s_min)) {
        *valid_range_out = true;
        return ver.major == maj;
      }
      int min = 0;
      if (!parse_int(s_min, &min) || !is_wild(s_pat)) {
        *valid_range_out = false;
        return false;
      }
      *valid_range_out = true;
      return ver.major == maj && ver.minor == min;
    }
  }

  Plv8Semver exact;
  if (!plv8_semver_parse(tok, &exact)) {
    *valid_range_out = false;
    return false;
  }
  *valid_range_out = true;
  return plv8_semver_cmp(ver, exact) == 0;
}

static bool plv8_semver_satisfies(const std::string& ver_str,
                                  const std::string& range_str) {
  if (range_str.empty()) return true;
  if (ver_str == range_str) return true;

  Plv8Semver ver;
  if (!plv8_semver_parse(ver_str, &ver)) {
    return ver_str == range_str;
  }

  // Split by "||"
  size_t pos = 0;
  bool any_branch_matched = false;
  while (pos <= range_str.size()) {
    size_t next_or = range_str.find("||", pos);
    std::string branch = (next_or == std::string::npos)
                             ? range_str.substr(pos)
                             : range_str.substr(pos, next_or - pos);
    branch = plv8_trim(branch);

    // Check hyphen range: "1.0.0 - 2.0.0"
    size_t hyph = branch.find(" - ");
    bool branch_ok = true;
    if (hyph != std::string::npos) {
      Plv8Semver low, high;
      if (!plv8_semver_parse(plv8_trim(branch.substr(0, hyph)), &low) ||
          !plv8_semver_parse(plv8_trim(branch.substr(hyph + 3)), &high)) {
        return false;
      }
      branch_ok =
          (plv8_semver_cmp(ver, low) >= 0 && plv8_semver_cmp(ver, high) <= 0);
    } else if (!branch.empty()) {
      // Tokenize space-separated comparators (joining operator + version if separated by space)
      std::vector<std::string> raw_tokens;
      size_t i = 0;
      while (i < branch.size()) {
        while (i < branch.size() && (branch[i] == ' ' || branch[i] == '\t'))
          i++;
        if (i >= branch.size()) break;
        size_t j = i;
        while (j < branch.size() && branch[j] != ' ' && branch[j] != '\t') j++;
        raw_tokens.push_back(branch.substr(i, j - i));
        i = j;
      }

      for (size_t t = 0; t < raw_tokens.size(); t++) {
        std::string tok = raw_tokens[t];
        if ((tok == ">=" || tok == "<=" || tok == ">" || tok == "<" ||
             tok == "=" || tok == "^" || tok == "~") &&
            t + 1 < raw_tokens.size()) {
          tok += raw_tokens[++t];
        }
        bool valid_tok = false;
        bool sat = plv8_semver_satisfies_single(ver, tok, &valid_tok);
        if (!valid_tok) return false;
        if (!sat) branch_ok = false;
      }
    }

    if (branch_ok) any_branch_matched = true;
    if (next_or == std::string::npos) break;
    pos = next_or + 2;
  }

  return any_branch_matched;
}

/* ---------- Specifier Normalization & Module Resolution ---------- */

struct Plv8ModuleId {
  bool is_package = false;
  std::string package_name;
  std::string package_version;
  std::string path;

  std::string canonical_key() const {
    if (is_package) {
      return "pkg:" + package_name + "@" + package_version + "/" + path;
    }
    return "mod:" + path;
  }

  std::string meta_url() const {
    if (is_package) {
      return "plv8:" + package_name + "@" + package_version + "/" + path;
    }
    return "plv8:" + path;
  }
};

static std::string plv8_strip_spec_prefix(const std::string& raw) {
  std::string s = raw;
  if (s.rfind("plv8-pkg:", 0) == 0)
    s = s.substr(9);
  else if (s.rfind("plv8:", 0) == 0)
    s = s.substr(5);
  else if (s.rfind("pgtle:", 0) == 0)
    s = s.substr(6);
  else if (s.rfind("tle://", 0) == 0)
    s = s.substr(6);
  else if (s.rfind("tle:", 0) == 0)
    s = s.substr(4);
  while (!s.empty() && s[0] == '/') s = s.substr(1);
  return s;
}

static bool plv8_parse_canonical_key(const std::string& key,
                                     Plv8ModuleId* out) {
  if (key.rfind("pkg:", 0) == 0) {
    std::string rest = key.substr(4);
    size_t search_start = (!rest.empty() && rest[0] == '@') ? 1 : 0;
    size_t at_pos = rest.find('@', search_start);
    if (at_pos == std::string::npos) return false;
    size_t slash_pos = rest.find('/', at_pos + 1);
    if (slash_pos == std::string::npos) return false;
    out->is_package = true;
    out->package_name = rest.substr(0, at_pos);
    out->package_version = rest.substr(at_pos + 1, slash_pos - at_pos - 1);
    out->path = rest.substr(slash_pos + 1);
    return true;
  }
  if (key.rfind("mod:", 0) == 0) {
    out->is_package = false;
    out->package_name.clear();
    out->package_version.clear();
    out->path = key.substr(4);
    return true;
  }
  return false;
}

static bool plv8_is_relative_spec(const std::string& spec) {
  return spec == "." || spec == ".." || spec.rfind("./", 0) == 0 ||
         spec.rfind("../", 0) == 0;
}

static bool plv8_join_relative_path(const std::string& base_path,
                                    const std::string& rel_spec,
                                    std::string* out_path) {
  std::string base_dir;
  size_t last_slash = base_path.rfind('/');
  if (last_slash != std::string::npos) {
    base_dir = base_path.substr(0, last_slash);
  }
  std::string combined =
      base_dir.empty() ? rel_spec : (base_dir + "/" + rel_spec);

  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= combined.size()) {
    size_t end = combined.find('/', start);
    std::string seg = (end == std::string::npos)
                          ? combined.substr(start)
                          : combined.substr(start, end - start);
    if (seg.empty() || seg == ".") {
      // skip
    } else if (seg == "..") {
      if (parts.empty()) {
        return false;  // escaped root
      }
      parts.pop_back();
    } else {
      parts.push_back(seg);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }

  if (parts.empty()) return false;

  std::string joined;
  for (size_t i = 0; i < parts.size(); i++) {
    if (i > 0) joined += "/";
    joined += parts[i];
  }
  *out_path = joined;
  return true;
}

static void plv8_parse_spec_parts(const std::string& clean_spec,
                                  std::string* pkg_name_out,
                                  std::string* sub_path_out,
                                  std::string* inline_range_out) {
  pkg_name_out->clear();
  sub_path_out->clear();
  inline_range_out->clear();

  size_t slash_search = (!clean_spec.empty() && clean_spec[0] == '@') ? 1 : 0;
  size_t slash_pos = clean_spec.find('/', slash_search);
  if (slash_pos != std::string::npos && !clean_spec.empty() &&
      clean_spec[0] == '@') {
    slash_pos = clean_spec.find('/', slash_pos + 1);
  }

  std::string head = (slash_pos == std::string::npos)
                         ? clean_spec
                         : clean_spec.substr(0, slash_pos);
  std::string tail = (slash_pos == std::string::npos)
                         ? ""
                         : clean_spec.substr(slash_pos + 1);

  size_t at_search = (!head.empty() && head[0] == '@') ? 1 : 0;
  size_t at_pos = head.find('@', at_search);
  if (at_pos != std::string::npos) {
    *pkg_name_out = head.substr(0, at_pos);
    *inline_range_out = head.substr(at_pos + 1);
    *sub_path_out = tail;
  } else {
    *pkg_name_out = head;
    if (!tail.empty()) {
      size_t tail_at = tail.rfind('@');
      if (tail_at != std::string::npos && tail_at > 0) {
        *sub_path_out = tail.substr(0, tail_at);
        *inline_range_out = tail.substr(tail_at + 1);
      } else {
        *sub_path_out = tail;
      }
    }
  }
}

struct Plv8FetchedModule {
  Plv8ModuleId id;
  std::string source;
  std::vector<uint8_t> bytecode;
  bool sloppy = false;
  bool is_trusted = true;
};

static bool plv8_fetch_tle_module(const std::string& raw_spec,
                                  const std::string& base_spec,
                                  Plv8FetchedModule* mod_out,
                                  std::string& err_msg) {
  if (!IsTransactionState()) {
    err_msg = "cannot load plv8 module outside a transaction";
    return false;
  }

  Oid nspoid = get_namespace_oid("plv8", true);
  if (!OidIsValid(nspoid)) {
    err_msg = "schema \"plv8\" does not exist";
    return false;
  }

  Oid mod_relid = get_relname_relid("modules", nspoid);
  Oid pkg_relid = get_relname_relid("packages", nspoid);
  if (!OidIsValid(mod_relid)) {
    err_msg = "table \"plv8.modules\" does not exist";
    return false;
  }

  Oid user_id = GetUserId();
  if (plv8_namespace_aclcheck(nspoid, user_id, ACL_USAGE) != ACLCHECK_OK) {
    err_msg = "permission denied for schema plv8";
    return false;
  }
  if (pg_class_aclcheck(mod_relid, user_id, ACL_SELECT) != ACLCHECK_OK) {
    err_msg = "permission denied for table modules";
    return false;
  }

  Plv8ModuleId importer;
  bool has_importer = false;
  if (!base_spec.empty()) {
    if (!plv8_parse_canonical_key(base_spec, &importer)) {
      std::string clean_base = plv8_strip_spec_prefix(base_spec);
      if (!clean_base.empty()) {
        has_importer = true;
        importer.is_package = false;
        importer.path = clean_base;
      }
    } else {
      has_importer = true;
    }
  }

  std::string clean_spec = plv8_strip_spec_prefix(raw_spec);
  if (clean_spec.empty()) {
    err_msg = "could not load module \"\"";
    return false;
  }

  bool is_rel = plv8_is_relative_spec(clean_spec);
  std::string rel_joined_path;
  if (is_rel) {
    if (!has_importer) {
      err_msg = "could not load relative module \"" + raw_spec +
                "\" without an importing module";
      return false;
    }
    if (!plv8_join_relative_path(importer.path, clean_spec, &rel_joined_path)) {
      err_msg = "relative module specifier \"" + raw_spec +
                "\" traverses above root";
      return false;
    }
  }

  bool caller_is_super = false;
  bool found = false;
  bool imp_has_requires = false;
  std::vector<std::string> imp_requires;

  Relation volatile open_rel = NULL;
  TableScanDesc volatile open_scan = NULL;
  volatile bool spi_connected = false;

  PG_TRY();
  {
    caller_is_super = superuser();

    struct PkgEntry {
      std::string name;
      std::string version;
      std::string entry_point;
      std::unordered_map<std::string, std::string> deps;
    };
    std::vector<PkgEntry> packages;

    auto extract_pkg_tuple = [&](HeapTuple pkg_tup, TupleDesc pkg_desc) {
      bool n_name = false, n_ver = false, n_ep = false, n_deps = false;
      Datum d_name = heap_getattr(pkg_tup, 1, pkg_desc, &n_name);
      Datum d_ver = heap_getattr(pkg_tup, 2, pkg_desc, &n_ver);
      Datum d_ep = heap_getattr(pkg_tup, 3, pkg_desc, &n_ep);
      if (n_name || n_ver) return;

      PkgEntry pe;
      char* c_name = TextDatumGetCString(d_name);
      char* c_ver = TextDatumGetCString(d_ver);
      pe.name.assign(c_name);
      pe.version.assign(c_ver);
      pfree(c_name);
      pfree(c_ver);

      if (!n_ep) {
        char* c_ep = TextDatumGetCString(d_ep);
        pe.entry_point.assign(c_ep);
        pfree(c_ep);
      } else {
        pe.entry_point = "index.js";
      }

      if (pkg_desc->natts >= 4) {
        Datum d_deps = heap_getattr(pkg_tup, 4, pkg_desc, &n_deps);
        if (!n_deps) {
          Jsonb* jb = DatumGetJsonbP(d_deps);
          JsonbIterator* it = JsonbIteratorInit(&jb->root);
          JsonbValue v;
          JsonbIteratorToken r;
          std::string cur_key;
          while ((r = JsonbIteratorNext(&it, &v, true)) != WJB_DONE) {
            if (r == WJB_KEY && v.type == jbvString) {
              cur_key.assign(v.val.string.val, v.val.string.len);
            } else if (r == WJB_VALUE && v.type == jbvString &&
                       !cur_key.empty()) {
              pe.deps[cur_key] =
                  std::string(v.val.string.val, v.val.string.len);
              cur_key.clear();
            }
          }
          if ((Pointer)jb != DatumGetPointer(d_deps)) pfree(jb);
        }
      }
      packages.push_back(std::move(pe));
    };

    if (OidIsValid(pkg_relid) &&
        pg_class_aclcheck(pkg_relid, user_id, ACL_SELECT) == ACLCHECK_OK) {
      if (check_enable_rls(pkg_relid, InvalidOid, true) == RLS_ENABLED) {
        if (SPI_connect() == SPI_OK_CONNECT) {
          spi_connected = true;
          if (SPI_execute("SELECT * FROM plv8.packages", true, 0) ==
                  SPI_OK_SELECT &&
              SPI_tuptable != NULL) {
            TupleDesc spi_desc = SPI_tuptable->tupdesc;
            for (uint64 r = 0; r < SPI_processed; r++) {
              CHECK_FOR_INTERRUPTS();
              extract_pkg_tuple(SPI_tuptable->vals[r], spi_desc);
            }
          }
          SPI_finish();
          spi_connected = false;
        }
      } else {
        open_rel = table_open(pkg_relid, AccessShareLock);
        TupleDesc pkg_desc = RelationGetDescr(open_rel);
        open_scan = table_beginscan_catalog(open_rel, 0, NULL);
        HeapTuple pkg_tup;

        while ((pkg_tup = heap_getnext(open_scan, ForwardScanDirection)) !=
               NULL) {
          CHECK_FOR_INTERRUPTS();
          extract_pkg_tuple(pkg_tup, pkg_desc);
        }

        table_endscan(open_scan);
        open_scan = NULL;
        table_close(open_rel, AccessShareLock);
        open_rel = NULL;
      }
    }

    // 2. Scan plv8.modules
    struct ModRow {
      std::string path;
      std::string source;
      std::string pkg_name;
      std::string pkg_ver;
      bool is_pkg = false;
      bool sloppy = false;
      bool is_trusted = true;
      bool has_requires = false;
      std::vector<std::string> requires_list;
      std::vector<uint8_t> bytecode;
    };
    std::vector<ModRow> modules;

    auto extract_mod_tuple = [&](HeapTuple mod_tup, TupleDesc mod_desc) {
      bool n_path = false, n_src = false, n_pkg = false, n_ver = false;
      bool n_sloppy = false, n_bc = false, n_req = false, n_trust = false;

      Datum d_path = heap_getattr(mod_tup, 1, mod_desc, &n_path);
      if (n_path) return;

      ModRow mr;
      char* c_path = TextDatumGetCString(d_path);
      mr.path.assign(c_path);
      pfree(c_path);

      if (mod_desc->natts >= 2) {
        Datum d_src = heap_getattr(mod_tup, 2, mod_desc, &n_src);
        if (!n_src) {
          char* c_src = TextDatumGetCString(d_src);
          mr.source.assign(c_src);
          pfree(c_src);
        }
      }

      if (mod_desc->natts >= 4) {
        Datum d_pkg = heap_getattr(mod_tup, 3, mod_desc, &n_pkg);
        Datum d_ver = heap_getattr(mod_tup, 4, mod_desc, &n_ver);
        if (!n_pkg) {
          char* c_pkg = TextDatumGetCString(d_pkg);
          mr.pkg_name.assign(c_pkg);
          pfree(c_pkg);
          mr.is_pkg = true;
        }
        if (!n_ver) {
          char* c_ver = TextDatumGetCString(d_ver);
          mr.pkg_ver.assign(c_ver);
          pfree(c_ver);
        }
      }

      if (mod_desc->natts >= 7) {
        Datum d_sloppy = heap_getattr(mod_tup, 7, mod_desc, &n_sloppy);
        if (!n_sloppy) mr.sloppy = DatumGetBool(d_sloppy);
      }

      if (mod_desc->natts >= 8) {
        Datum d_bc = heap_getattr(mod_tup, 8, mod_desc, &n_bc);
        if (!n_bc) {
          bytea* bc = DatumGetByteaPP(d_bc);
          const uint8_t* bc_ptr = (const uint8_t*)VARDATA_ANY(bc);
          size_t bc_len = VARSIZE_ANY_EXHDR(bc);
          mr.bytecode.assign(bc_ptr, bc_ptr + bc_len);
          if ((Pointer)bc != DatumGetPointer(d_bc)) pfree(bc);
        }
      }

      if (mod_desc->natts >= 9) {
        Datum d_req = heap_getattr(mod_tup, 9, mod_desc, &n_req);
        if (!n_req) {
          mr.has_requires = true;
          ArrayType* arr = DatumGetArrayTypeP(d_req);
          Datum* elems = nullptr;
          bool* nulls = nullptr;
          int nelems = 0;
          deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, &nulls,
                            &nelems);
          for (int i = 0; i < nelems; i++) {
            if (!nulls[i]) {
              char* req_c = TextDatumGetCString(elems[i]);
              mr.requires_list.emplace_back(req_c);
              pfree(req_c);
            }
          }
          if (elems) pfree(elems);
          if (nulls) pfree(nulls);
          if ((Pointer)arr != DatumGetPointer(d_req)) pfree(arr);
        }
      }

      if (mod_desc->natts >= 10) {
        Datum d_trust = heap_getattr(mod_tup, 10, mod_desc, &n_trust);
        if (!n_trust) mr.is_trusted = DatumGetBool(d_trust);
      }

      if (mr.source.empty() && mr.bytecode.empty()) return;

      if (has_importer && mr.is_pkg == importer.is_package &&
          mr.path == importer.path &&
          (!mr.is_pkg || (mr.pkg_name == importer.package_name &&
                          mr.pkg_ver == importer.package_version))) {
        imp_has_requires = mr.has_requires;
        imp_requires = mr.requires_list;
      }

      modules.push_back(std::move(mr));
    };

    if (check_enable_rls(mod_relid, InvalidOid, true) == RLS_ENABLED) {
      if (SPI_connect() == SPI_OK_CONNECT) {
        spi_connected = true;
        if (SPI_execute("SELECT * FROM plv8.modules", true, 0) ==
                SPI_OK_SELECT &&
            SPI_tuptable != NULL) {
          TupleDesc spi_desc = SPI_tuptable->tupdesc;
          for (uint64 r = 0; r < SPI_processed; r++) {
            CHECK_FOR_INTERRUPTS();
            extract_mod_tuple(SPI_tuptable->vals[r], spi_desc);
          }
        }
        SPI_finish();
        spi_connected = false;
      }
    } else {
      open_rel = table_open(mod_relid, AccessShareLock);
      TupleDesc mod_desc = RelationGetDescr(open_rel);
      open_scan = table_beginscan_catalog(open_rel, 0, NULL);
      HeapTuple mod_tup;

      while ((mod_tup = heap_getnext(open_scan, ForwardScanDirection)) !=
             NULL) {
        CHECK_FOR_INTERRUPTS();
        extract_mod_tuple(mod_tup, mod_desc);
      }

      table_endscan(open_scan);
      open_scan = NULL;
      table_close(open_rel, AccessShareLock);
      open_rel = NULL;
    }

    auto populate_from_row = [&](const ModRow& mr) {
      mod_out->id.is_package = mr.is_pkg;
      mod_out->id.package_name = mr.pkg_name;
      mod_out->id.package_version = mr.pkg_ver;
      mod_out->id.path = mr.path;
      mod_out->source = mr.source;
      mod_out->bytecode = mr.bytecode;
      mod_out->sloppy = mr.sloppy;
      mod_out->is_trusted = mr.is_trusted;
      found = true;
    };

    if (is_rel) {
      for (const auto& mr : modules) {
        if (mr.is_pkg == importer.is_package && mr.path == rel_joined_path &&
            (!mr.is_pkg || (mr.pkg_name == importer.package_name &&
                            mr.pkg_ver == importer.package_version))) {
          populate_from_row(mr);
          break;
        }
      }
    } else {
      std::string pkg_name, sub_path, inline_range;
      plv8_parse_spec_parts(clean_spec, &pkg_name, &sub_path, &inline_range);

      std::string range = inline_range;
      bool is_declared = false;
      if (range.empty() && has_importer && importer.is_package) {
        if (importer.package_name == pkg_name) {
          is_declared = true;
          range = importer.package_version;
        } else {
          for (const auto& pe : packages) {
            if (pe.name == importer.package_name &&
                pe.version == importer.package_version) {
              auto it = pe.deps.find(pkg_name);
              if (it != pe.deps.end()) {
                range = it->second;
                is_declared = true;
              }
              break;
            }
          }
        }
      }

      // 1. Standalone module match (takes priority unless declared as a package dependency or inline @version)
      if (!is_declared && inline_range.empty()) {
        for (const auto& mr : modules) {
          if (!mr.is_pkg && mr.path == clean_spec) {
            populate_from_row(mr);
            break;
          }
        }
      }

      // 2. Package match in plv8.packages
      if (!found) {
        std::string best_ver;
        std::string best_ep;
        bool pkg_matched = false;

        for (const auto& pe : packages) {
          if (pe.name != pkg_name) continue;
          if (!range.empty() && !plv8_semver_satisfies(pe.version, range))
            continue;
          if (!pkg_matched || plv8_compare_semver(pe.version, best_ver) > 0) {
            best_ver = pe.version;
            best_ep = pe.entry_point;
            pkg_matched = true;
          }
        }

        if (pkg_matched) {
          std::string target_path = sub_path.empty() ? best_ep : sub_path;
          for (const auto& mr : modules) {
            if (mr.is_pkg && mr.pkg_name == pkg_name &&
                mr.pkg_ver == best_ver && mr.path == target_path) {
              populate_from_row(mr);
              break;
            }
          }
        }
      }

      // 3. Fallback for standalone modules referenced with inline @version suffix
      if (!found && !is_declared && !inline_range.empty()) {
        size_t last_at = clean_spec.rfind('@');
        std::string stripped = (last_at != std::string::npos && last_at > 0)
                                   ? clean_spec.substr(0, last_at)
                                   : clean_spec;
        for (const auto& mr : modules) {
          if (!mr.is_pkg && (mr.path == clean_spec || mr.path == stripped)) {
            populate_from_row(mr);
            break;
          }
        }
      }
    }
  }
  PG_CATCH();
  {
    ErrorData* edata = CopyErrorData();
    if (edata && edata->message)
      err_msg = edata->message;
    else
      err_msg = "failed to scan plv8.modules catalog";
    if (edata) FreeErrorData(edata);
    FlushErrorState();
    if (open_scan != NULL) {
      table_endscan(open_scan);
      open_scan = NULL;
    }
    if (open_rel != NULL) {
      table_close(open_rel, AccessShareLock);
      open_rel = NULL;
    }
    if (spi_connected) {
      SPI_finish();
      spi_connected = false;
    }
    return false;
  }
  PG_END_TRY();

  if (!found) {
    err_msg = "module \"" + clean_spec + "\" not found in plv8.modules";
    return false;
  }

  std::string target_label =
      mod_out->id.is_package
          ? (mod_out->id.package_name + "@" + mod_out->id.package_version)
          : mod_out->id.path;

  if (!mod_out->is_trusted && !caller_is_super) {
    err_msg = "permission denied for untrusted module \"" + target_label + "\"";
    return false;
  }

  if (imp_has_requires) {
    bool allowed = false;
    for (size_t i = 0; i < imp_requires.size(); i++) {
      std::string entry = plv8_strip_spec_prefix(imp_requires[i]);
      std::string req_pkg, req_sub, req_ver;
      plv8_parse_spec_parts(entry, &req_pkg, &req_sub, &req_ver);
      std::string req_full =
          req_sub.empty() ? req_pkg : (req_pkg + "/" + req_sub);

      if (mod_out->id.is_package) {
        std::string pkg_sub =
            mod_out->id.package_name + "/" + mod_out->id.path;
        if ((req_full == mod_out->id.package_name || req_full == pkg_sub ||
             req_full == mod_out->id.path) &&
            (req_ver.empty() ||
             plv8_semver_satisfies(mod_out->id.package_version, req_ver))) {
          allowed = true;
          break;
        }
      } else {
        if (req_full == mod_out->id.path || entry == mod_out->id.path) {
          allowed = true;
          break;
        }
      }
    }
    if (!allowed) {
      std::string imp_label =
          importer.is_package ? importer.package_name : importer.path;
      err_msg = "plv8 module \"" + target_label +
                "\" is not listed in requires of \"" + imp_label + "\"";
      return false;
    }
  }

  return true;
}

static MaybeLocal<Module> plv8_load_tle_module(Local<Context> context,
                                               const std::string& raw_spec,
                                               const std::string& base_spec) {
  Isolate* isolate = Isolate::GetCurrent();

  if (plv8_current_module_depth >= plv8_max_module_depth) {
    std::string msg = "module import depth exceeded (" +
                      std::to_string(plv8_max_module_depth) +
                      "): possible circular dependency";
    isolate->ThrowException(Exception::Error(ToString(msg.c_str())));
    return MaybeLocal<Module>();
  }

  if (current_context && base_spec.empty()) {
    plv8_sync_tle_module_cache(current_context);
  }

  Plv8FetchedModule fetched;
  std::string err_msg;
  if (!plv8_fetch_tle_module(raw_spec, base_spec, &fetched, err_msg)) {
    isolate->ThrowException(Exception::Error(ToString(err_msg.c_str())));
    return MaybeLocal<Module>();
  }

  std::string canonical_key = fetched.id.canonical_key();
  if (current_context) {
    auto it = current_context->tle_module_map.find(canonical_key);
    if (it != current_context->tle_module_map.end() && !it->second.IsEmpty()) {
      return it->second.Get(isolate);
    }
  }

  std::string origin_url = fetched.id.meta_url();
  Local<v8::String> resource_name = ToString(origin_url.c_str());
  ScriptOrigin origin(resource_name, 0, 0, false, -1, Local<v8::Value>(), false,
                      false, true /* is_module */);

  std::string actual_source = fetched.source;
  const std::vector<uint8_t>& bytecode = fetched.bytecode;
  ScriptCompiler::CachedData* cached_data = nullptr;
  ScriptCompiler::CompileOptions compile_options =
      ScriptCompiler::kNoCompileOptions;

  if (bytecode.size() >= 16 &&
      memcmp(bytecode.data(), PLV8_BYTECODE_MAGIC, 8) == 0) {
    uint32_t src_len = 0, cache_len = 0;
    memcpy(&src_len, bytecode.data() + 8, sizeof(uint32_t));
    memcpy(&cache_len, bytecode.data() + 12, sizeof(uint32_t));
    if (16ULL + src_len + cache_len == bytecode.size() && cache_len > 0) {
      actual_source.assign((const char*)(bytecode.data() + 16), src_len);
      uint8_t* cd_buf = new uint8_t[cache_len];
      memcpy(cd_buf, bytecode.data() + 16 + src_len, cache_len);
      cached_data = new ScriptCompiler::CachedData(
          cd_buf, (int)cache_len, ScriptCompiler::CachedData::BufferOwned);
      compile_options = ScriptCompiler::kConsumeCodeCache;
    }
  } else if (bytecode.size() >= 8 && bytecode[0] == 0x00 &&
             bytecode[1] == 0x61 && bytecode[2] == 0x73 &&
             bytecode[3] == 0x6d && bytecode[4] == 0x01 &&
             bytecode[5] == 0x00 && bytecode[6] == 0x00 &&
             bytecode[7] == 0x00) {
    std::ostringstream wasm_js;
    wasm_js << "const __wasm_bytes = new Uint8Array([";
    for (size_t i = 0; i < bytecode.size(); i++) {
      if (i > 0) wasm_js << ",";
      wasm_js << (unsigned int)bytecode[i];
    }
    wasm_js << "]);\n";
    wasm_js << "const __mod = (typeof plv8.compile_wasm_cached === 'function') "
            << "? plv8.compile_wasm_cached(__wasm_bytes) "
            << ": new WebAssembly.Module(__wasm_bytes);\n";
    wasm_js << "if (typeof globalThis.__plv8_wasm_polyfills_loaded === 'undefined') {\n"
            << "  try { plv8.find_function('public.plv8_wasm_bootstrap_polyfills')(); } catch (_e) {}\n"
            << "}\n";
    wasm_js << "let __inst = null;\n";
    wasm_js << "const __get_inst = function() { return __inst; };\n";
    wasm_js << "const __wasi = (typeof globalThis.__plv8_wasm_create_wasi === 'function') "
            << "? globalThis.__plv8_wasm_create_wasi(__get_inst) : {};\n";
    wasm_js << "const __pg_host = (typeof globalThis.__plv8_wasm_create_postgres === 'function') "
            << "? globalThis.__plv8_wasm_create_postgres(__get_inst) : (globalThis.postgres || {});\n";
    wasm_js << "const __imports = {\n"
            << "  wasi_snapshot_preview1: __wasi,\n"
            << "  wasi_unstable: __wasi,\n"
            << "  env: {\n"
            << "    abort: function() { throw new Error('WASM execution "
               "aborted'); },\n"
            << "    __wasi_fd_write: __wasi.fd_write || function() { return 0; },\n"
            << "    'syscall.seek': function() { return 0; },\n"
            << "    plv8_elog_notice: function(ptr, len) {\n"
            << "      const b = new Uint8Array(__inst.exports.memory.buffer, "
               "ptr >>> 0, len >>> 0);\n"
            << "      let s = ''; for (let i = 0; i < b.length; i++) s += "
               "String.fromCharCode(b[i]);\n"
            << "      plv8.elog(NOTICE, s);\n"
            << "    },\n"
            << "    plv8_elog_error: function(ptr, len) {\n"
            << "      const b = new Uint8Array(__inst.exports.memory.buffer, "
               "ptr >>> 0, len >>> 0);\n"
            << "      let s = ''; for (let i = 0; i < b.length; i++) s += "
               "String.fromCharCode(b[i]);\n"
            << "      throw new Error(s);\n"
            << "    }\n"
            << "  },\n"
            << "  postgres: __pg_host\n"
            << "};\n";
    wasm_js << "__inst = new WebAssembly.Instance(__mod, __imports);\n";
    wasm_js << "if (typeof __inst.exports._initialize === 'function') { __inst.exports._initialize(); }\n";
    wasm_js << "export const module = __mod;\n";
    wasm_js << "export const instance = __inst;\n";
    wasm_js << "export const exports = __inst.exports;\n";

    bool exported_memory = false;
    size_t pos = 8;
    auto read_uleb128 = [&](size_t& p, uint32_t& out) -> bool {
      out = 0;
      unsigned shift = 0;
      while (p < bytecode.size() && shift < 35) {
        uint8_t byte = bytecode[p++];
        out |= (uint32_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
      }
      return false;
    };
    auto is_valid_js_ident = [](const std::string& s) -> bool {
      if (s.empty() || s == "default" || s == "module" || s == "instance" ||
          s == "exports")
        return false;
      char c0 = s[0];
      if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_' ||
            c0 == '$'))
        return false;
      for (size_t i = 1; i < s.size(); i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '$'))
          return false;
      }
      return true;
    };

    while (pos < bytecode.size()) {
      uint8_t sec_id = bytecode[pos++];
      uint32_t sec_len = 0;
      if (!read_uleb128(pos, sec_len) || pos + sec_len > bytecode.size()) break;
      size_t sec_end = pos + sec_len;
      if (sec_id == 7) {
        uint32_t count = 0;
        if (read_uleb128(pos, count)) {
          for (uint32_t i = 0; i < count && pos < sec_end; i++) {
            uint32_t name_len = 0;
            if (!read_uleb128(pos, name_len) || pos + name_len > sec_end) break;
            std::string exp_name((const char*)(bytecode.data() + pos),
                                 name_len);
            pos += name_len;
            if (pos >= sec_end) break;
            pos++;
            uint32_t exp_idx = 0;
            if (!read_uleb128(pos, exp_idx)) break;
            if (is_valid_js_ident(exp_name)) {
              if (exp_name == "memory") exported_memory = true;
              wasm_js << "export const " << exp_name << " = __inst.exports[\""
                      << exp_name << "\"];\n";
            }
          }
        }
        break;
      }
      pos = sec_end;
    }
    if (!exported_memory)
      wasm_js << "export const memory = __inst.exports.memory;\n";
    wasm_js << "export default __inst.exports;\n";
    actual_source = wasm_js.str();
  } else if (!bytecode.empty()) {
    uint8_t* cd_buf = new uint8_t[bytecode.size()];
    memcpy(cd_buf, bytecode.data(), bytecode.size());
    cached_data = new ScriptCompiler::CachedData(
        cd_buf, (int)bytecode.size(), ScriptCompiler::CachedData::BufferOwned);
    compile_options = ScriptCompiler::kConsumeCodeCache;
  } else {
    bool is_cjs =
        fetched.sloppy ||
        (actual_source.find("export ") == std::string::npos &&
         actual_source.find("export{") == std::string::npos &&
         actual_source.find("export\t") == std::string::npos &&
         (actual_source.find("exports") != std::string::npos ||
          actual_source.find("require(") != std::string::npos));
    if (is_cjs) {
      std::string cjs_imports, cjs_body;
      plv8_extract_hoisted_imports(actual_source.c_str(), &cjs_imports,
                                   &cjs_body);
      if (fetched.sloppy) {
        std::ostringstream escaped;
        escaped << "\"";
        for (unsigned char c : cjs_body) {
          if (c == '\\')
            escaped << "\\\\";
          else if (c == '"')
            escaped << "\\\"";
          else if (c == '\n')
            escaped << "\\n";
          else if (c == '\r')
            escaped << "\\r";
          else if (c == '\t')
            escaped << "\\t";
          else
            escaped << (char)c;
        }
        escaped << "\"";
        actual_source =
            cjs_imports +
            "\nconst module = { exports: {} };\nconst exports = "
            "module.exports;\nconst require = (spec) => plv8.require(spec, \"" +
            canonical_key + "\");\n" +
            "(new Function('exports', 'require', 'module', " + escaped.str() +
            ")).call(module.exports, module.exports, require, module);\n"
            "export default module.exports;\n";
      } else {
        actual_source = cjs_imports +
                        "\nconst module = { exports: {} };\nconst exports = "
                        "module.exports;\n" +
                        "const require = (spec) => plv8.require(spec, \"" +
                        canonical_key + "\");\n" + cjs_body +
                        "\nexport default module.exports;\n";
      }
    }
  }

  Local<v8::String> source_str =
      ToString(actual_source.data(), actual_source.size());
  ScriptCompiler::Source script_source(source_str, origin, cached_data);

  MaybeLocal<Module> maybe_mod =
      ScriptCompiler::CompileModule(isolate, &script_source, compile_options);

  if (maybe_mod.IsEmpty() &&
      compile_options == ScriptCompiler::kConsumeCodeCache) {
    ScriptCompiler::Source fallback_source(source_str, origin);
    maybe_mod = ScriptCompiler::CompileModule(isolate, &fallback_source);
  }

  Local<Module> mod;
  if (!maybe_mod.ToLocal(&mod)) return MaybeLocal<Module>();

  if (current_context) {
    current_context->tle_module_map[canonical_key].Reset(isolate, mod);
    current_context->tle_module_id_to_spec[mod->GetIdentityHash()] =
        canonical_key;
  }

  return mod;
}

MaybeLocal<Module> plv8_ResolveModuleCallback(
    Local<Context> context, Local<v8::String> specifier,
    Local<FixedArray> import_attributes, Local<Module> referrer) {
  Isolate* isolate = Isolate::GetCurrent();
  v8::String::Utf8Value spec_utf8(isolate, specifier);
  std::string raw_spec(*spec_utf8 ? *spec_utf8 : "");

  std::string base_spec;
  if (!referrer.IsEmpty() && current_context) {
    auto it = current_context->tle_module_id_to_spec.find(
        referrer->GetIdentityHash());
    if (it != current_context->tle_module_id_to_spec.end())
      base_spec = it->second;
  }

  Plv8ModuleDepthScope depth_scope;
  return plv8_load_tle_module(context, raw_spec, base_spec);
}

MaybeLocal<Promise> plv8_HostImportModuleDynamicallyCallback(
    Local<Context> context, Local<Data> host_defined_options,
    Local<v8::Value> resource_name, Local<v8::String> specifier,
    Local<FixedArray> import_attributes) {
  Isolate* isolate = Isolate::GetCurrent();
  Local<Promise::Resolver> resolver;
  if (!Promise::Resolver::New(context).ToLocal(&resolver))
    return MaybeLocal<Promise>();

  TryCatch try_catch(isolate);
  v8::String::Utf8Value spec_utf8(isolate, specifier);
  std::string raw_spec(*spec_utf8 ? *spec_utf8 : "");

  std::string base_spec;
  if (!resource_name.IsEmpty() && resource_name->IsString()) {
    v8::String::Utf8Value res_utf8(isolate, resource_name);
    if (*res_utf8) {
      if (strncmp(*res_utf8, "plv8:", 5) == 0 ||
          strncmp(*res_utf8, "pgtle:", 6) == 0 ||
          strncmp(*res_utf8, "tle:", 4) == 0) {
        base_spec = *res_utf8;
      }
    }
  }

  MaybeLocal<Module> maybe_mod =
      plv8_load_tle_module(context, raw_spec, base_spec);
  Local<Module> mod;
  if (!maybe_mod.ToLocal(&mod)) {
    Local<v8::Value> exc =
        try_catch.HasCaught()
            ? try_catch.Exception()
            : Exception::Error(ToString("failed to load module"));
    resolver->Reject(context, exc).FromMaybe(false);
    return resolver->GetPromise();
  }

  if (mod->GetStatus() == Module::kUninstantiated) {
    Maybe<bool> inst =
        mod->InstantiateModule(context, plv8_ResolveModuleCallback);
    if (inst.IsNothing() || !inst.FromJust()) {
      Local<v8::Value> exc =
          try_catch.HasCaught()
              ? try_catch.Exception()
              : Exception::Error(ToString("failed to instantiate module"));
      resolver->Reject(context, exc).FromMaybe(false);
      return resolver->GetPromise();
    }
  }

  if (mod->GetStatus() == Module::kInstantiated) {
    bool prev_ignore =
        current_context ? current_context->ignore_unhandled_promises : false;
    if (current_context) current_context->ignore_unhandled_promises = true;
    MaybeLocal<v8::Value> eval_res = mod->Evaluate(context);
    if (current_context)
      current_context->ignore_unhandled_promises = prev_ignore;

    Local<v8::Value> val;
    if (!eval_res.ToLocal(&val)) {
      Local<v8::Value> exc =
          try_catch.HasCaught()
              ? try_catch.Exception()
              : Exception::Error(ToString("failed to evaluate module"));
      resolver->Reject(context, exc).FromMaybe(false);
      return resolver->GetPromise();
    }
    if (val->IsPromise()) {
      Local<Promise> prom = val.As<Promise>();
      if (prom->State() == Promise::kRejected) {
        resolver->Reject(context, prom->Result()).FromMaybe(false);
        return resolver->GetPromise();
      }
    }
  }

  if (mod->GetStatus() == Module::kErrored) {
    resolver->Reject(context, mod->GetException()).FromMaybe(false);
    return resolver->GetPromise();
  }

  resolver->Resolve(context, mod->GetModuleNamespace()).FromMaybe(false);
  return resolver->GetPromise();
}

void plv8_HostInitializeImportMetaObjectCallback(Local<Context> context,
                                                 Local<Module> module,
                                                 Local<Object> meta) {
  std::string url = "plv8:anonymous";

  if (current_context) {
    auto it =
        current_context->tle_module_id_to_spec.find(module->GetIdentityHash());
    if (it != current_context->tle_module_id_to_spec.end()) {
      Plv8ModuleId id;
      if (plv8_parse_canonical_key(it->second, &id)) {
        url = id.meta_url();
      } else {
        url = "plv8:" + it->second;
      }
    }
  }

  meta->CreateDataProperty(context, ToString("url"), ToString(url.c_str()))
      .FromMaybe(false);
}

static Local<Object> plv8_compile_and_eval_anon_module(Local<Context> context,
                                                       const char* name,
                                                       const char* mod_src) {
  Isolate* isolate = Isolate::GetCurrent();
  EscapableHandleScope handle_scope(isolate);
  TryCatch try_catch(isolate);

  Local<v8::String> sname = ToString(name ? name : "plv8:anonymous");
  Local<v8::String> source = ToString(mod_src);
  ScriptOrigin origin(sname, 0, 0, false, -1, Local<v8::Value>(), false, false,
                      true /* is_module */);
  ScriptCompiler::Source script_source(source, origin);

  MaybeLocal<Module> maybe_mod =
      ScriptCompiler::CompileModule(isolate, &script_source);
  Local<Module> mod;
  if (!maybe_mod.ToLocal(&mod)) throw js_error(try_catch);

  Maybe<bool> inst =
      mod->InstantiateModule(context, plv8_ResolveModuleCallback);
  if (inst.IsNothing() || !inst.FromJust()) throw js_error(try_catch);

  bool prev_ignore =
      current_context ? current_context->ignore_unhandled_promises : false;
  if (current_context) current_context->ignore_unhandled_promises = true;
  MaybeLocal<v8::Value> eval_res = mod->Evaluate(context);
  if (current_context) current_context->ignore_unhandled_promises = prev_ignore;

  Local<v8::Value> val;
  if (!eval_res.ToLocal(&val)) throw js_error(try_catch);

  if (val->IsPromise()) {
    Local<Promise> prom = val.As<Promise>();
    if (prom->State() == Promise::kRejected) {
      isolate->ThrowException(prom->Result());
      throw js_error(try_catch);
    }
  }

  Local<v8::Value> ns_val = mod->GetModuleNamespace();
  return handle_scope.Escape(ns_val.As<Object>());
}

static void plv8_load_module_sync_impl(
    const FunctionCallbackInfo<v8::Value>& args, bool unwrap_default) {
  Isolate* isolate = args.GetIsolate();
  HandleScope handle_scope(isolate);
  Local<Context> context = isolate->GetCurrentContext();

  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(Exception::TypeError(
        ToString(unwrap_default ? "require() expects a module name string"
                                : "import() expects a module name string")));
    return;
  }

  v8::String::Utf8Value spec_utf8(isolate, args[0]);
  std::string raw_spec(*spec_utf8 ? *spec_utf8 : "");

  std::string base_spec;
  if (args.Length() >= 2 && args[1]->IsString()) {
    v8::String::Utf8Value base_utf8(isolate, args[1]);
    if (*base_utf8) base_spec.assign(*base_utf8);
  } else {
    Local<StackTrace> st =
        StackTrace::CurrentStackTrace(isolate, 4, StackTrace::kScriptName);
    if (!st.IsEmpty()) {
      for (int i = 0; i < st->GetFrameCount(); i++) {
        Local<StackFrame> frame = st->GetFrame(isolate, i);
        if (!frame.IsEmpty() && !frame->GetScriptName().IsEmpty()) {
          v8::String::Utf8Value sname(isolate, frame->GetScriptName());
          if (*sname && (strncmp(*sname, "plv8:", 5) == 0 ||
                         strncmp(*sname, "pgtle:", 6) == 0 ||
                         strncmp(*sname, "tle:", 4) == 0)) {
            std::string url(*sname);
            std::string body = plv8_strip_spec_prefix(url);
            // Check if this matches a known canonical key in tle_module_map
            if (current_context) {
              for (const auto& kv : current_context->tle_module_id_to_spec) {
                Plv8ModuleId cid;
                if (plv8_parse_canonical_key(kv.second, &cid) &&
                    cid.meta_url() == url) {
                  base_spec = kv.second;
                  break;
                }
              }
            }
            if (base_spec.empty()) base_spec = url;
            break;
          }
        }
      }
    }
  }

  Plv8ModuleDepthScope depth_scope;
  TryCatch try_catch(isolate);
  MaybeLocal<Module> maybe_mod =
      plv8_load_tle_module(context, raw_spec, base_spec);
  Local<Module> mod;
  if (!maybe_mod.ToLocal(&mod)) {
    try_catch.ReThrow();
    return;
  }

  if (mod->GetStatus() == Module::kUninstantiated) {
    Maybe<bool> inst =
        mod->InstantiateModule(context, plv8_ResolveModuleCallback);
    if (inst.IsNothing() || !inst.FromJust()) {
      try_catch.ReThrow();
      return;
    }
  }

  if (mod->GetStatus() == Module::kInstantiated) {
    bool prev_ignore =
        current_context ? current_context->ignore_unhandled_promises : false;
    if (current_context) current_context->ignore_unhandled_promises = true;
    MaybeLocal<v8::Value> eval_res = mod->Evaluate(context);
    if (current_context)
      current_context->ignore_unhandled_promises = prev_ignore;

    Local<v8::Value> val;
    if (!eval_res.ToLocal(&val)) {
      try_catch.ReThrow();
      return;
    }
    if (val->IsPromise()) {
      Local<Promise> prom = val.As<Promise>();
      if (prom->State() == Promise::kRejected) {
        isolate->ThrowException(prom->Result());
        return;
      }
    }
  }

  if (mod->GetStatus() == Module::kErrored) {
    isolate->ThrowException(mod->GetException());
    return;
  }

  Local<v8::Value> ns_val = mod->GetModuleNamespace();
  if (unwrap_default &&
      (ns_val->IsModuleNamespaceObject() || ns_val->IsObject())) {
    Local<Object> ns_obj = ns_val.As<Object>();
    Local<Array> keys;
    if (ns_obj->GetOwnPropertyNames(context).ToLocal(&keys) &&
        keys->Length() == 1) {
      Local<v8::Value> k0;
      if (keys->Get(context, 0).ToLocal(&k0) && k0->IsString()) {
        v8::String::Utf8Value k_utf8(isolate, k0);
        if (*k_utf8 && strcmp(*k_utf8, "default") == 0) {
          Local<v8::Value> def_val;
          if (ns_obj->Get(context, k0).ToLocal(&def_val)) {
            args.GetReturnValue().Set(def_val);
            return;
          }
        }
      }
    }
  }

  args.GetReturnValue().Set(ns_val);
}

void plv8_Require(const FunctionCallbackInfo<v8::Value>& args) {
  plv8_load_module_sync_impl(args, true);
}

void plv8_Import(const FunctionCallbackInfo<v8::Value>& args) {
  plv8_load_module_sync_impl(args, false);
}

void plv8_CompileBytecode(const FunctionCallbackInfo<v8::Value>& args) {
  Isolate* isolate = args.GetIsolate();
  HandleScope handle_scope(isolate);

  if (args.Length() < 1 || !args[0]->IsString()) {
    isolate->ThrowException(Exception::TypeError(
        ToString("plv8.compile_bytecode(source[, module_name]) expects a "
                 "source string")));
    return;
  }

  Local<v8::String> source_str = args[0].As<v8::String>();
  v8::String::Utf8Value src_utf8(isolate, source_str);
  const char* src_bytes = *src_utf8 ? *src_utf8 : "";
  uint32_t src_len = (uint32_t)src_utf8.length();

  std::string origin_url = "plv8:bytecode";
  std::string canonical_key;
  if (args.Length() >= 2 && args[1]->IsString()) {
    v8::String::Utf8Value name_utf8(isolate, args[1]);
    if (*name_utf8 && (*name_utf8)[0] != '\0') {
      std::string n = plv8_strip_spec_prefix(*name_utf8);
      origin_url = "plv8:" + n;
      size_t search_start = (!n.empty() && n[0] == '@') ? 1 : 0;
      size_t at_pos = n.find('@', search_start);
      if (at_pos != std::string::npos) {
        if (n.find('/', at_pos + 1) != std::string::npos) {
          canonical_key = "pkg:" + n;
        } else {
          canonical_key = "pkg:" + n + "/index.js";
        }
      } else {
        canonical_key = "mod:" + n;
      }
    }
  }

  std::string actual_source(src_bytes, src_len);
  bool is_cjs =
      (actual_source.find("export ") == std::string::npos &&
       actual_source.find("export{") == std::string::npos &&
       actual_source.find("export\t") == std::string::npos &&
       (actual_source.find("exports") != std::string::npos ||
        actual_source.find("require(") != std::string::npos));
  if (is_cjs) {
    std::string cjs_imports, cjs_body;
    plv8_extract_hoisted_imports(actual_source.c_str(), &cjs_imports,
                                 &cjs_body);
    std::string req_call =
        canonical_key.empty()
            ? "plv8.require(spec)"
            : ("plv8.require(spec, \"" + canonical_key + "\")");
    actual_source = cjs_imports +
                    "\nconst module = { exports: {} };\nconst exports = "
                    "module.exports;\nconst require = (spec) => " +
                    req_call + ";\n" + cjs_body +
                    "\nexport default module.exports;\n";
    source_str = ToString(actual_source.data(), actual_source.size());
    src_bytes = actual_source.data();
    src_len = (uint32_t)actual_source.size();
  }

  ScriptOrigin origin(ToString(origin_url.c_str()), 0, 0, false, -1,
                      Local<v8::Value>(), false, false, true /* is_module */);
  ScriptCompiler::Source script_source(source_str, origin);

  TryCatch try_catch(isolate);
  MaybeLocal<Module> maybe_mod =
      ScriptCompiler::CompileModule(isolate, &script_source);
  Local<Module> mod;
  if (!maybe_mod.ToLocal(&mod)) {
    try_catch.ReThrow();
    return;
  }

  ScriptCompiler::CachedData* cd =
      ScriptCompiler::CreateCodeCache(mod->GetUnboundModuleScript());
  if (!cd || !cd->data || cd->length <= 0) {
    if (cd) delete cd;
    isolate->ThrowException(
        Exception::Error(ToString("V8 CreateCodeCache returned empty data")));
    return;
  }

  uint32_t cache_len = (uint32_t)cd->length;
  size_t total_len = 16 + (size_t)src_len + (size_t)cache_len;

  std::unique_ptr<v8::BackingStore> bs =
      v8::ArrayBuffer::NewBackingStore(isolate, total_len);
  uint8_t* dst = static_cast<uint8_t*>(bs->Data());

  memcpy(dst, PLV8_BYTECODE_MAGIC, 8);
  memcpy(dst + 8, &src_len, sizeof(uint32_t));
  memcpy(dst + 12, &cache_len, sizeof(uint32_t));
  if (src_len > 0) memcpy(dst + 16, src_bytes, src_len);
  memcpy(dst + 16 + src_len, cd->data, cache_len);

  delete cd;

  Local<ArrayBuffer> ab = v8::ArrayBuffer::New(isolate, std::move(bs));
  Local<Uint8Array> u8 = v8::Uint8Array::New(ab, 0, total_len);
  args.GetReturnValue().Set(u8);
}

/*
 * Append a C string as a JSON string literal.
 */
static void
plv8_append_json_string(StringInfo buf, const char *str)
{
	if (str == NULL)
	{
		appendStringInfoString(buf, "null");
		return;
	}

	appendStringInfoChar(buf, '"');
	for (const unsigned char *p = (const unsigned char *) str; *p != '\0'; p++)
	{
		unsigned char c = *p;

		CHECK_FOR_INTERRUPTS();
		switch (c)
		{
			case '"':
				appendStringInfoString(buf, "\\\"");
				break;
			case '\\':
				appendStringInfoString(buf, "\\\\");
				break;
			case '\b':
				appendStringInfoString(buf, "\\b");
				break;
			case '\f':
				appendStringInfoString(buf, "\\f");
				break;
			case '\n':
				appendStringInfoString(buf, "\\n");
				break;
			case '\r':
				appendStringInfoString(buf, "\\r");
				break;
			case '\t':
				appendStringInfoString(buf, "\\t");
				break;
			default:
				if (c < 0x20)
					appendStringInfo(buf, "\\u%04x", (unsigned int) c);
				else
					appendStringInfoChar(buf, (char) c);
				break;
		}
	}
	appendStringInfoChar(buf, '"');
}

/* =========================================================================
 * PL/v8 Dynamic Table Function & Planner Support Subsystem
 *
 * Provides:
 *   1. `plv8_source_in` / `plv8_source_out` — parse-time TupleDesc inference
 *      for functions taking `plv8_source` (or a domain over `plv8_source` such
 *      as `pgq_pattern`) and returning `SETOF record`, enabling calls in FROM
 *      both WITH and WITHOUT an explicit `AS (...)` column definition list.
 *   2. `plv8_call_support` — `prosupport` planner hook (`SUPPORT plv8_call_support`)
 *      that delegates `SupportRequestSimplify`, `SupportRequestCost`, and
 *      `SupportRequestRows` to JavaScript callbacks (`plv8.register_support`)
 *      or SQL/PLv8 support functions (`<func>_support` / `plv8.support_hooks`),
 *      inlining `FROM`-clause calls into native `RTE_SUBQUERY` nodes with
 *      full outer `LATERAL` `Var` correlation via `plv8_inline_helper_0..7`.
 * =========================================================================
 */

#define PLV8_NUM_INLINE_HELPERS 8
#define PLV8_MAX_INFERRED_CACHE 16
#define PLV8_MAX_HOOK_ENTRIES   32
#define PLV8_MAX_STAMPED_OIDS   64

typedef struct Plv8InferredEntry
{
	char	   *func_name;
	char	   *source_str;
	char	   *first_arg;
	int			ncols;
	Plv8ColMeta *cols;
} Plv8InferredEntry;

typedef struct Plv8SupportHookEntry
{
	bool					in_use;
	char					func_name[NAMEDATALEN];
	char					infer_sql_func[NAMEDATALEN];
	char					support_sql_func[NAMEDATALEN];
	Oid						user_id;
	v8::Persistent<v8::Function> infer_js_cb;
	v8::Persistent<v8::Function> support_js_cb;
} Plv8SupportHookEntry;

static Plv8InferredEntry plv8_inferred_cache[PLV8_MAX_INFERRED_CACHE];
static int plv8_inferred_cache_next = 0;

static Plv8SupportHookEntry plv8_hook_registry[PLV8_MAX_HOOK_ENTRIES];
static bool plv8_hook_registry_initialized = false;

static Oid plv8_stamped_oids[PLV8_MAX_STAMPED_OIDS];
static int plv8_num_stamped_oids = 0;

static bool plv8_in_support_internal_call = false;

static void
plv8_ensure_hook_registry_init(void)
{
	if (!plv8_hook_registry_initialized)
	{
		for (int i = 0; i < PLV8_MAX_HOOK_ENTRIES; i++)
		{
			plv8_hook_registry[i].in_use = false;
			plv8_hook_registry[i].func_name[0] = '\0';
			plv8_hook_registry[i].infer_sql_func[0] = '\0';
			plv8_hook_registry[i].support_sql_func[0] = '\0';
			plv8_hook_registry[i].user_id = InvalidOid;
			new(&plv8_hook_registry[i].infer_js_cb) v8::Persistent<v8::Function>();
			new(&plv8_hook_registry[i].support_js_cb) v8::Persistent<v8::Function>();
		}
		plv8_hook_registry_initialized = true;
	}
}

static void
plv8_normalize_func_name(const char *raw, char *out, size_t out_sz)
{
	if (raw == NULL || out_sz == 0)
	{
		if (out_sz > 0)
			out[0] = '\0';
		return;
	}

	const char *dot = strrchr(raw, '.');
	const char *p = dot ? (dot + 1) : raw;
	size_t idx = 0;

	while (*p != '\0' && idx + 1 < out_sz)
	{
		unsigned char c = (unsigned char) *p++;
		if (c == '"')
			continue;
		if (c >= 'A' && c <= 'Z')
			c = (unsigned char) (c - 'A' + 'a');
		out[idx++] = (char) c;
	}
	out[idx] = '\0';
}

static Plv8SupportHookEntry *
plv8_find_or_create_hook_entry(const char *func_name, bool create)
{
	plv8_ensure_hook_registry_init();
	char norm[NAMEDATALEN];
	plv8_normalize_func_name(func_name, norm, sizeof(norm));
	if (norm[0] == '\0')
		return NULL;

	int free_slot = -1;
	for (int i = 0; i < PLV8_MAX_HOOK_ENTRIES; i++)
	{
		if (plv8_hook_registry[i].in_use &&
			strcmp(plv8_hook_registry[i].func_name, norm) == 0)
		{
			return &plv8_hook_registry[i];
		}
		if (!plv8_hook_registry[i].in_use && free_slot < 0)
			free_slot = i;
	}

	if (!create || free_slot < 0)
		return NULL;

	Plv8SupportHookEntry *e = &plv8_hook_registry[free_slot];
	e->in_use = true;
	strlcpy(e->func_name, norm, sizeof(e->func_name));
	e->infer_sql_func[0] = '\0';
	e->support_sql_func[0] = '\0';
	e->user_id = current_context ? current_context->user_id : GetUserId();
	return e;
}

void
plv8_register_column_inference_hook(const char *func_name,
									const char *sql_func_name,
									v8::Local<v8::Function> js_cb)
{
	Plv8SupportHookEntry *e = plv8_find_or_create_hook_entry(func_name, true);
	if (e == NULL)
		return;

	e->infer_js_cb.Reset();
	e->infer_sql_func[0] = '\0';

	if (!js_cb.IsEmpty() && current_isolate != nullptr)
	{
		e->infer_js_cb.Reset(current_isolate, js_cb);
		e->user_id = current_context ? current_context->user_id : GetUserId();
	}
	else if (sql_func_name != NULL && sql_func_name[0] != '\0')
	{
		strlcpy(e->infer_sql_func, sql_func_name, sizeof(e->infer_sql_func));
	}
}

void
plv8_register_planner_support_hook(const char *func_name,
								   const char *sql_func_name,
								   v8::Local<v8::Function> js_cb)
{
	Plv8SupportHookEntry *e = plv8_find_or_create_hook_entry(func_name, true);
	if (e == NULL)
		return;

	e->support_js_cb.Reset();
	e->support_sql_func[0] = '\0';

	if (!js_cb.IsEmpty() && current_isolate != nullptr)
	{
		e->support_js_cb.Reset(current_isolate, js_cb);
		e->user_id = current_context ? current_context->user_id : GetUserId();
	}
	else if (sql_func_name != NULL && sql_func_name[0] != '\0')
	{
		strlcpy(e->support_sql_func, sql_func_name, sizeof(e->support_sql_func));
	}
}

static void
plv8_dispose_support_hooks_for_user(Oid user_id)
{
	if (!plv8_hook_registry_initialized)
		return;

	for (int i = 0; i < PLV8_MAX_HOOK_ENTRIES; i++)
	{
		if (plv8_hook_registry[i].in_use &&
			plv8_hook_registry[i].user_id == user_id)
		{
			plv8_hook_registry[i].infer_js_cb.Reset();
			plv8_hook_registry[i].support_js_cb.Reset();
		}
	}
}

/*
 * Save inferred columns in TopMemoryContext ring buffer.
 */
static void
plv8_save_inferred_columns(const char *func_name, const char *source_str,
						   const char *first_arg, int ncols,
						   const Plv8ColMeta *cols)
{
	if (ncols <= 0 || source_str == NULL)
		return;

	MemoryContext oldcxt = MemoryContextSwitchTo(TopMemoryContext);
	Plv8InferredEntry *slot = &plv8_inferred_cache[plv8_inferred_cache_next];
	plv8_inferred_cache_next = (plv8_inferred_cache_next + 1) % PLV8_MAX_INFERRED_CACHE;

	if (slot->func_name != NULL)
		pfree(slot->func_name);
	if (slot->source_str != NULL)
		pfree(slot->source_str);
	if (slot->first_arg != NULL)
		pfree(slot->first_arg);
	if (slot->cols != NULL)
	{
		for (int i = 0; i < slot->ncols; i++)
		{
			if (slot->cols[i].name != NULL)
				pfree(slot->cols[i].name);
		}
		pfree(slot->cols);
	}

	slot->func_name = pstrdup(func_name ? func_name : "");
	slot->source_str = pstrdup(source_str);
	slot->first_arg = first_arg ? pstrdup(first_arg) : NULL;
	slot->ncols = ncols;
	slot->cols = (Plv8ColMeta *) palloc0(sizeof(Plv8ColMeta) * ncols);
	for (int i = 0; i < ncols; i++)
	{
		CHECK_FOR_INTERRUPTS();
		slot->cols[i].name = pstrdup(cols[i].name ? cols[i].name : "?column?");
		slot->cols[i].type_oid = cols[i].type_oid;
		slot->cols[i].typmod = cols[i].typmod;
		slot->cols[i].collation_oid = cols[i].collation_oid;
	}

	MemoryContextSwitchTo(oldcxt);
}

static Plv8InferredEntry *
plv8_find_inferred_columns(const char *source_str)
{
	if (source_str == NULL)
		return NULL;

	for (int i = 0; i < PLV8_MAX_INFERRED_CACHE; i++)
	{
		int idx = (plv8_inferred_cache_next - 1 - i + PLV8_MAX_INFERRED_CACHE) % PLV8_MAX_INFERRED_CACHE;

		CHECK_FOR_INTERRUPTS();
		if (plv8_inferred_cache[idx].source_str != NULL &&
			strcmp(plv8_inferred_cache[idx].source_str, source_str) == 0)
			return &plv8_inferred_cache[idx];
	}
	return NULL;
}

/*
 * Look up a function by name and argument count in the syscache.
 */
static Oid
plv8_lookup_func_oid_by_name_nargs(const char *funcname, int nargs)
{
	if (funcname == NULL || funcname[0] == '\0')
		return InvalidOid;

	Oid result = InvalidOid;
	CatCList *catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(funcname));
	for (int i = 0; i < catlist->n_members; i++)
	{
		HeapTuple proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);

		CHECK_FOR_INTERRUPTS();
		if (nargs < 0 || procform->pronargs == nargs)
		{
			result = procform->oid;
			break;
		}
	}
	ReleaseSysCacheList(catlist);
	return result;
}

static Oid
plv8_find_infer_sql_func_oid(const char *func_name)
{
	Plv8SupportHookEntry *e = plv8_find_or_create_hook_entry(func_name, false);
	if (e != NULL && e->infer_sql_func[0] != '\0')
	{
		Oid oid = plv8_lookup_func_oid_by_name_nargs(e->infer_sql_func, 2);
		if (OidIsValid(oid))
			return oid;
	}

	char candidate[NAMEDATALEN * 2];
	snprintf(candidate, sizeof(candidate), "%s_infer_columns",
			 (func_name && func_name[0] != '\0') ? func_name : "graph_table");
	Oid oid = plv8_lookup_func_oid_by_name_nargs(candidate, 2);
	if (OidIsValid(oid))
		return oid;

	if (func_name == NULL || func_name[0] == '\0' || strcmp(func_name, "graph_table") == 0)
	{
		oid = plv8_lookup_func_oid_by_name_nargs("pgq_infer_columns_tsv", 2);
		if (OidIsValid(oid))
			return oid;
	}

	return InvalidOid;
}

static Oid
plv8_find_support_sql_func_oid(const char *func_name)
{
	Plv8SupportHookEntry *e = plv8_find_or_create_hook_entry(func_name, false);
	if (e != NULL && e->support_sql_func[0] != '\0')
	{
		Oid oid = plv8_lookup_func_oid_by_name_nargs(e->support_sql_func, 5);
		if (OidIsValid(oid))
			return oid;
	}

	char candidate[NAMEDATALEN * 2];
	snprintf(candidate, sizeof(candidate), "%s_support",
			 (func_name && func_name[0] != '\0') ? func_name : "graph_table");
	Oid oid = plv8_lookup_func_oid_by_name_nargs(candidate, 5);
	if (OidIsValid(oid))
		return oid;

	if (func_name == NULL || func_name[0] == '\0' || strcmp(func_name, "graph_table") == 0)
	{
		oid = plv8_lookup_func_oid_by_name_nargs("pgq_compile_graph_table_tsv", 5);
		if (OidIsValid(oid))
			return oid;
	}

	return InvalidOid;
}

/*
 * Find the 1-column composite holder type (`plv8_single_col_holder` or
 * `plpgq_single_col_holder`) and update its single attribute metadata.
 */
static Oid
plv8_update_single_col_holder_type(const Plv8ColMeta *col)
{
	static const char *holder_names[] = {
		"plv8_single_col_holder",
		"plpgq_single_col_holder",
		NULL
	};
	Oid holder_type_oid = InvalidOid;
	Oid holder_relid = InvalidOid;

	if (col == NULL || XactReadOnly)
		return InvalidOid;

	for (int h = 0; holder_names[h] != NULL && !OidIsValid(holder_type_oid); h++)
	{
		CatCList *catlist = SearchSysCacheList1(TYPENAMENSP,
												CStringGetDatum(holder_names[h]));
		for (int i = 0; i < catlist->n_members; i++)
		{
			HeapTuple ttup = &catlist->members[i]->tuple;
			Form_pg_type tform = (Form_pg_type) GETSTRUCT(ttup);

			CHECK_FOR_INTERRUPTS();
			if (tform->typtype == TYPTYPE_COMPOSITE && OidIsValid(tform->typrelid))
			{
				holder_type_oid = tform->oid;
				holder_relid = tform->typrelid;
				break;
			}
		}
		ReleaseSysCacheList(catlist);
	}

	if (!OidIsValid(holder_type_oid) || !OidIsValid(holder_relid))
		return InvalidOid;

	plv8_cached_holder_relid = holder_relid;

	HeapTuple typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(col->type_oid));
	if (!HeapTupleIsValid(typtup))
		return InvalidOid;
	Form_pg_type typform = (Form_pg_type) GETSTRUCT(typtup);
	int16 typ_len = typform->typlen;
	bool typ_byval = typform->typbyval;
	char typ_align = typform->typalign;
	char typ_storage = typform->typstorage;
	Oid typ_collation = typform->typcollation;
	char typ_category = typform->typcategory;
	ReleaseSysCache(typtup);

	const char *desired_name = (col->name && col->name[0] != '\0') ? col->name : "?column?";
	Oid desired_coll = OidIsValid(col->collation_oid)
		? col->collation_oid
		: typ_collation;
	int16 desired_ndims = (typ_category == TYPCATEGORY_ARRAY) ? 1 : 0;

	HeapTuple cur_atttup = SearchSysCache2(ATTNUM,
										   ObjectIdGetDatum(holder_relid),
										   Int16GetDatum(1));
	if (HeapTupleIsValid(cur_atttup))
	{
		Form_pg_attribute cur_att = (Form_pg_attribute) GETSTRUCT(cur_atttup);
		bool same = (strncmp(NameStr(cur_att->attname), desired_name, NAMEDATALEN - 1) == 0 &&
					 cur_att->atttypid == col->type_oid &&
					 cur_att->atttypmod == col->typmod &&
					 cur_att->attcollation == desired_coll &&
					 cur_att->attndims == desired_ndims);
		ReleaseSysCache(cur_atttup);
		if (same)
			return holder_type_oid;
	}

	Relation attrel = table_open(AttributeRelationId, ShareRowExclusiveLock);
	HeapTuple atttup = SearchSysCacheCopy2(ATTNUM,
										   ObjectIdGetDatum(holder_relid),
										   Int16GetDatum(1));
	if (!HeapTupleIsValid(atttup))
	{
		table_close(attrel, NoLock);
		return InvalidOid;
	}

	Form_pg_attribute attform = (Form_pg_attribute) GETSTRUCT(atttup);
	if (strncmp(NameStr(attform->attname), desired_name, NAMEDATALEN - 1) == 0 &&
		attform->atttypid == col->type_oid &&
		attform->atttypmod == col->typmod &&
		attform->attcollation == desired_coll &&
		attform->attndims == desired_ndims)
	{
		heap_freetuple(atttup);
		table_close(attrel, NoLock);
		return holder_type_oid;
	}

	namestrcpy(&attform->attname, desired_name);
	attform->atttypid = col->type_oid;
	attform->attlen = typ_len;
	attform->attbyval = typ_byval;
	attform->attalign = typ_align;
	attform->attstorage = typ_storage;
#if PG_VERSION_NUM >= 140000
	attform->attcompression = '\0';
#endif
	attform->atttypmod = col->typmod;
	attform->attndims = desired_ndims;
	attform->attcollation = desired_coll;

	CatalogTupleUpdate(attrel, &atttup->t_self, atttup);
	heap_freetuple(atttup);
	table_close(attrel, NoLock);

	plv8_in_internal_catalog_update = true;
	CacheInvalidateRelcacheByRelid(holder_relid);
	plv8_in_internal_catalog_update = false;
	CommandCounterIncrement();

	return holder_type_oid;
}

static bool
plv8_is_single_col_holder_type(Oid typid)
{
	if (!OidIsValid(typid) || typid == RECORDOID)
		return false;

	HeapTuple typtup = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(typtup))
		return false;
	Form_pg_type tform = (Form_pg_type) GETSTRUCT(typtup);
	bool match = (strcmp(NameStr(tform->typname), "plv8_single_col_holder") == 0 ||
				  strcmp(NameStr(tform->typname), "plpgq_single_col_holder") == 0);
	ReleaseSysCache(typtup);
	return match;
}

static void
plv8_record_stamped_oid(Oid funcid)
{
	if (!OidIsValid(funcid))
		return;
	for (int i = 0; i < plv8_num_stamped_oids; i++)
	{
		if (plv8_stamped_oids[i] == funcid)
			return;
	}
	if (plv8_num_stamped_oids < PLV8_MAX_STAMPED_OIDS)
		plv8_stamped_oids[plv8_num_stamped_oids++] = funcid;
}

static bool
plv8_varlena_attr_equals(HeapTuple tup, TupleDesc tupdesc, int attnum,
						 bool new_isnull, Datum new_val)
{
	bool old_isnull = false;
	Datum old_val = heap_getattr(tup, attnum, tupdesc, &old_isnull);
	if (old_isnull != new_isnull)
		return false;
	if (new_isnull)
		return true;
	struct varlena *old_v = PG_DETOAST_DATUM_PACKED(old_val);
	struct varlena *new_v = PG_DETOAST_DATUM_PACKED(new_val);
	size_t old_len = VARSIZE_ANY_EXHDR(old_v);
	size_t new_len = VARSIZE_ANY_EXHDR(new_v);
	bool eq = (old_len == new_len &&
			   memcmp(VARDATA_ANY(old_v), VARDATA_ANY(new_v), old_len) == 0);
	if ((Pointer) old_v != DatumGetPointer(old_val))
		pfree(old_v);
	if ((Pointer) new_v != DatumGetPointer(new_val))
		pfree(new_v);
	return eq;
}

/*
 * Update `proallargtypes`, `proargmodes`, and `proargnames` on a `pg_proc`
 * tuple (and optionally `prosrc`, `proargtypes`, `pronargs`, `prorettype` for inline helpers).
 */
static void
plv8_update_proc_table_columns(Oid funcid,
							   int num_out_cols,
							   const Plv8ColMeta *out_cols,
							   const char *new_prosrc,
							   int new_num_in_args,
							   const Oid *new_in_arg_oids,
							   Oid target_rettype = InvalidOid)
{
	if (!OidIsValid(funcid) || XactReadOnly)
		return;

	Relation proc_rel = table_open(ProcedureRelationId, ShareRowExclusiveLock);

	Oid single_holder_oid = InvalidOid;
	if (num_out_cols == 1 && out_cols != NULL &&
		(!OidIsValid(target_rettype) || plv8_is_single_col_holder_type(target_rettype)))
	{
		single_holder_oid = plv8_update_single_col_holder_type(&out_cols[0]);
	}

	HeapTuple tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tup))
	{
		table_close(proc_rel, NoLock);
		return;
	}

	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(tup);
	TupleDesc proc_tupdesc = RelationGetDescr(proc_rel);

	Oid desired_base_rettype = OidIsValid(target_rettype) ? target_rettype : RECORDOID;

	if (num_out_cols <= 0 && new_prosrc == NULL && new_num_in_args < 0 &&
		procform->prorettype == desired_base_rettype &&
		heap_attisnull(tup, Anum_pg_proc_proallargtypes, proc_tupdesc))
	{
		heap_freetuple(tup);
		table_close(proc_rel, NoLock);
		return;
	}

	Datum values[Natts_pg_proc];
	bool nulls[Natts_pg_proc];
	bool replaces[Natts_pg_proc];
	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));
	MemSet(replaces, false, sizeof(replaces));

	int ninargs;
	const Oid *inargoids;

	if (new_num_in_args >= 0)
	{
		ninargs = new_num_in_args;
		inargoids = new_in_arg_oids;
		values[Anum_pg_proc_pronargs - 1] = Int16GetDatum((int16) ninargs);
		replaces[Anum_pg_proc_pronargs - 1] = true;
		values[Anum_pg_proc_proargtypes - 1] =
			PointerGetDatum(buildoidvector(inargoids, ninargs));
		replaces[Anum_pg_proc_proargtypes - 1] = true;
	}
	else
	{
		ninargs = procform->pronargs;
		inargoids = procform->proargtypes.values;
	}

	if (new_prosrc != NULL)
	{
		values[Anum_pg_proc_prosrc - 1] = CStringGetTextDatum(new_prosrc);
		replaces[Anum_pg_proc_prosrc - 1] = true;
	}

	Datum *existing_names = NULL;
	bool *existing_nulls = NULL;
	int n_existing_names = 0;
	if (new_num_in_args < 0 && ninargs > 0)
	{
		bool argnames_isnull = true;
		Datum argnames_datum = heap_getattr(tup, Anum_pg_proc_proargnames,
											proc_tupdesc, &argnames_isnull);
		if (!argnames_isnull)
		{
#if PG_VERSION_NUM >= 160000
			deconstruct_array_builtin(DatumGetArrayTypeP(argnames_datum), TEXTOID,
									  &existing_names, &existing_nulls,
									  &n_existing_names);
#else
			deconstruct_array(DatumGetArrayTypeP(argnames_datum), TEXTOID, -1, false,
							  TYPALIGN_INT, &existing_names, &existing_nulls,
							  &n_existing_names);
#endif
		}
	}

	bool has_real_in_argnames = false;
	if (existing_names != NULL && n_existing_names >= ninargs && ninargs > 0)
	{
		for (int i = 0; i < ninargs; i++)
		{
			if (!existing_nulls[i])
			{
				char *nm = TextDatumGetCString(existing_names[i]);
				if (nm[0] != '\0' && strncmp(nm, "__in_", 5) != 0)
					has_real_in_argnames = true;
				pfree(nm);
			}
			else
			{
				existing_names[i] = CStringGetTextDatum("");
			}
		}
	}

	if (num_out_cols <= 0)
	{
		values[Anum_pg_proc_prorettype - 1] = ObjectIdGetDatum(desired_base_rettype);
		replaces[Anum_pg_proc_prorettype - 1] = true;

		nulls[Anum_pg_proc_proallargtypes - 1] = true;
		replaces[Anum_pg_proc_proallargtypes - 1] = true;

		nulls[Anum_pg_proc_proargmodes - 1] = true;
		replaces[Anum_pg_proc_proargmodes - 1] = true;

		if (has_real_in_argnames)
		{
#if PG_VERSION_NUM >= 160000
			values[Anum_pg_proc_proargnames - 1] = PointerGetDatum(
				construct_array_builtin(existing_names, ninargs, TEXTOID));
#else
			values[Anum_pg_proc_proargnames - 1] = PointerGetDatum(
				construct_array(existing_names, ninargs, TEXTOID, -1, false, TYPALIGN_INT));
#endif
		}
		else
		{
			nulls[Anum_pg_proc_proargnames - 1] = true;
		}
		replaces[Anum_pg_proc_proargnames - 1] = true;
	}
	else if (num_out_cols == 1 && OidIsValid(single_holder_oid) &&
			 (!OidIsValid(target_rettype) || plv8_is_single_col_holder_type(target_rettype)))
	{
		values[Anum_pg_proc_prorettype - 1] = ObjectIdGetDatum(single_holder_oid);
		replaces[Anum_pg_proc_prorettype - 1] = true;

		nulls[Anum_pg_proc_proallargtypes - 1] = true;
		replaces[Anum_pg_proc_proallargtypes - 1] = true;

		nulls[Anum_pg_proc_proargmodes - 1] = true;
		replaces[Anum_pg_proc_proargmodes - 1] = true;

		if (has_real_in_argnames)
		{
#if PG_VERSION_NUM >= 160000
			values[Anum_pg_proc_proargnames - 1] = PointerGetDatum(
				construct_array_builtin(existing_names, ninargs, TEXTOID));
#else
			values[Anum_pg_proc_proargnames - 1] = PointerGetDatum(
				construct_array(existing_names, ninargs, TEXTOID, -1, false, TYPALIGN_INT));
#endif
		}
		else
		{
			nulls[Anum_pg_proc_proargnames - 1] = true;
		}
		replaces[Anum_pg_proc_proargnames - 1] = true;
	}
	else
	{
		int total_args = ninargs + num_out_cols;
		Datum *all_types = (Datum *) palloc(sizeof(Datum) * total_args);
		Datum *all_modes = (Datum *) palloc(sizeof(Datum) * total_args);
		Datum *all_names = (Datum *) palloc(sizeof(Datum) * total_args);

		values[Anum_pg_proc_prorettype - 1] = ObjectIdGetDatum(RECORDOID);
		replaces[Anum_pg_proc_prorettype - 1] = true;

		for (int i = 0; i < ninargs; i++)
		{
			char argbuf[32];

			CHECK_FOR_INTERRUPTS();
			all_types[i] = ObjectIdGetDatum(inargoids[i]);
			all_modes[i] = CharGetDatum(PROARGMODE_IN);
			if (existing_names != NULL && i < n_existing_names && !existing_nulls[i])
			{
				all_names[i] = existing_names[i];
			}
			else
			{
				snprintf(argbuf, sizeof(argbuf), "__in_%d", i + 1);
				all_names[i] = CStringGetTextDatum(argbuf);
			}
		}
		for (int i = 0; i < num_out_cols; i++)
		{
			CHECK_FOR_INTERRUPTS();
			all_types[ninargs + i] = ObjectIdGetDatum(out_cols[i].type_oid);
			all_modes[ninargs + i] = CharGetDatum(PROARGMODE_TABLE);
			all_names[ninargs + i] = CStringGetTextDatum(
				out_cols[i].name ? out_cols[i].name : "?column?");
		}

#if PG_VERSION_NUM >= 160000
		values[Anum_pg_proc_proallargtypes - 1] = PointerGetDatum(
			construct_array_builtin(all_types, total_args, OIDOID));
		replaces[Anum_pg_proc_proallargtypes - 1] = true;

		values[Anum_pg_proc_proargmodes - 1] = PointerGetDatum(
			construct_array_builtin(all_modes, total_args, CHAROID));
		replaces[Anum_pg_proc_proargmodes - 1] = true;

		values[Anum_pg_proc_proargnames - 1] = PointerGetDatum(
			construct_array_builtin(all_names, total_args, TEXTOID));
		replaces[Anum_pg_proc_proargnames - 1] = true;
#else
		values[Anum_pg_proc_proallargtypes - 1] = PointerGetDatum(
			construct_array(all_types, total_args, OIDOID, sizeof(Oid), true, TYPALIGN_INT));
		replaces[Anum_pg_proc_proallargtypes - 1] = true;

		values[Anum_pg_proc_proargmodes - 1] = PointerGetDatum(
			construct_array(all_modes, total_args, CHAROID, 1, true, TYPALIGN_CHAR));
		replaces[Anum_pg_proc_proargmodes - 1] = true;

		values[Anum_pg_proc_proargnames - 1] = PointerGetDatum(
			construct_array(all_names, total_args, TEXTOID, -1, false, TYPALIGN_INT));
		replaces[Anum_pg_proc_proargnames - 1] = true;
#endif
	}

	bool already_matches = true;
	if (replaces[Anum_pg_proc_pronargs - 1])
	{
		if (procform->pronargs != (int16) ninargs ||
			(ninargs > 0 &&
			 memcmp(procform->proargtypes.values, inargoids, sizeof(Oid) * ninargs) != 0))
		{
			already_matches = false;
		}
	}
	if (already_matches && replaces[Anum_pg_proc_prorettype - 1])
	{
		if (procform->prorettype != DatumGetObjectId(values[Anum_pg_proc_prorettype - 1]))
			already_matches = false;
	}
	if (already_matches && replaces[Anum_pg_proc_prosrc - 1])
	{
		if (!plv8_varlena_attr_equals(tup, proc_tupdesc, Anum_pg_proc_prosrc,
									  nulls[Anum_pg_proc_prosrc - 1],
									  values[Anum_pg_proc_prosrc - 1]))
			already_matches = false;
	}
	if (already_matches && replaces[Anum_pg_proc_proallargtypes - 1])
	{
		if (!plv8_varlena_attr_equals(tup, proc_tupdesc, Anum_pg_proc_proallargtypes,
									  nulls[Anum_pg_proc_proallargtypes - 1],
									  values[Anum_pg_proc_proallargtypes - 1]))
			already_matches = false;
	}
	if (already_matches && replaces[Anum_pg_proc_proargmodes - 1])
	{
		if (!plv8_varlena_attr_equals(tup, proc_tupdesc, Anum_pg_proc_proargmodes,
									  nulls[Anum_pg_proc_proargmodes - 1],
									  values[Anum_pg_proc_proargmodes - 1]))
			already_matches = false;
	}
	if (already_matches && replaces[Anum_pg_proc_proargnames - 1])
	{
		if (!plv8_varlena_attr_equals(tup, proc_tupdesc, Anum_pg_proc_proargnames,
									  nulls[Anum_pg_proc_proargnames - 1],
									  values[Anum_pg_proc_proargnames - 1]))
			already_matches = false;
	}

	if (already_matches)
	{
		heap_freetuple(tup);
		table_close(proc_rel, NoLock);
		if (num_out_cols > 0 && new_prosrc == NULL)
			plv8_record_stamped_oid(funcid);
		return;
	}

	HeapTuple newtup = heap_modify_tuple(tup, proc_tupdesc,
										 values, nulls, replaces);
	CatalogTupleUpdate(proc_rel, &newtup->t_self, newtup);

	/*
	 * If this function is already in `plv8_proc_cache_hash` and we only
	 * toggled its TABLE return columns (not `prosrc` or `proargtypes`), keep
	 * its `fn_xmin` / `fn_tid` and `rettype = RECORDOID` synchronized so the
	 * compiled V8 function cache stays warm.
	 */
	if (new_prosrc == NULL && new_num_in_args < 0 && plv8_proc_cache_hash != NULL)
	{
		bool found = false;
		plv8_proc_cache *cache = (plv8_proc_cache *)
			hash_search(plv8_proc_cache_hash, &funcid, HASH_FIND, &found);
		if (found && cache != NULL && !cache->function.IsEmpty())
		{
			cache->fn_xmin = HeapTupleHeaderGetXmin(newtup->t_data);
			cache->fn_tid = newtup->t_self;
			cache->rettype = RECORDOID;
		}
	}

	heap_freetuple(newtup);
	heap_freetuple(tup);
	table_close(proc_rel, NoLock);

	CommandCounterIncrement();

	if (num_out_cols > 0 && new_prosrc == NULL)
		plv8_record_stamped_oid(funcid);
}

/*
 * Apply `plv8_update_proc_table_columns` to all `RETURNS SETOF record`
 * overloads of `func_name`.
 */
static void
plv8_update_func_overloads_columns(const char *func_name,
								   int num_out_cols,
								   const Plv8ColMeta *out_cols)
{
	if (XactReadOnly)
		return;

	char norm[NAMEDATALEN];
	plv8_normalize_func_name(func_name ? func_name : "graph_table", norm, sizeof(norm));
	if (norm[0] == '\0')
		strlcpy(norm, "graph_table", sizeof(norm));

	List *oids = NIL;
	CatCList *catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(norm));
	for (int i = 0; i < catlist->n_members; i++)
	{
		HeapTuple proctup = &catlist->members[i]->tuple;
		Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(proctup);

		CHECK_FOR_INTERRUPTS();
		if (procform->proretset &&
			(procform->prorettype == RECORDOID ||
			 plv8_is_single_col_holder_type(procform->prorettype)))
		{
			if (num_out_cols <= 0 && procform->prorettype == RECORDOID)
			{
				bool att_isnull = true;
				(void) SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_proallargtypes, &att_isnull);
				if (att_isnull)
					continue;
			}
			oids = lappend_oid(oids, procform->oid);
		}
	}
	ReleaseSysCacheList(catlist);

	ListCell *lc;
	foreach(lc, oids)
	{
		Oid foid = lfirst_oid(lc);

		CHECK_FOR_INTERRUPTS();
		plv8_update_proc_table_columns(foid, num_out_cols, out_cols, NULL, -1, NULL);
	}
	list_free(oids);
}

static void
plv8_reset_all_stamped_procs(void)
{
	if (plv8_in_support_internal_call || plv8_num_stamped_oids <= 0 || XactReadOnly)
		return;

	int count = plv8_num_stamped_oids;
	Oid local_oids[PLV8_MAX_STAMPED_OIDS];
	for (int i = 0; i < count; i++)
		local_oids[i] = plv8_stamped_oids[i];
	plv8_num_stamped_oids = 0;

	PG_TRY();
	{
		for (int i = 0; i < count; i++)
		{
			CHECK_FOR_INTERRUPTS();
			plv8_update_proc_table_columns(local_oids[i], 0, NULL, NULL, -1, NULL);
		}
	}
	PG_CATCH();
	{
		FlushErrorState();
	}
	PG_END_TRY();
}

void
plv8_set_function_columns_js(const char *func_name,
							 int ncols,
							 const Plv8ColMeta *cols)
{
	PG_TRY();
	{
		plv8_update_func_overloads_columns(func_name, ncols, cols);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();
}

void
plv8_ensure_proc_support(Oid fn_oid)
{
	if (!OidIsValid(fn_oid) || XactReadOnly)
		return;

	Oid support_oid = plv8_lookup_func_oid_by_name_nargs("plv8_call_support", 1);
	if (!OidIsValid(support_oid))
		return;

	HeapTuple check_tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(check_tup))
		return;
	Form_pg_proc check_form = (Form_pg_proc) GETSTRUCT(check_tup);
	bool needs_update = (check_form->proretset && !OidIsValid(check_form->prosupport));
	ReleaseSysCache(check_tup);

	if (!needs_update)
		return;

	Relation proc_rel = table_open(ProcedureRelationId, ShareRowExclusiveLock);
	HeapTuple tup = SearchSysCacheCopy1(PROCOID, ObjectIdGetDatum(fn_oid));
	if (!HeapTupleIsValid(tup))
	{
		table_close(proc_rel, NoLock);
		return;
	}

	Form_pg_proc procform = (Form_pg_proc) GETSTRUCT(tup);
	if (procform->proretset && !OidIsValid(procform->prosupport))
	{
		procform->prosupport = support_oid;
		plv8_in_internal_catalog_update = true;
		CatalogTupleUpdate(proc_rel, &tup->t_self, tup);
		plv8_in_internal_catalog_update = false;

		if (plv8_proc_cache_hash != NULL)
		{
			bool found = false;
			plv8_proc_cache *cache = (plv8_proc_cache *)
				hash_search(plv8_proc_cache_hash, &fn_oid, HASH_FIND, &found);
			if (found && cache != NULL && !cache->function.IsEmpty())
			{
				cache->fn_xmin = HeapTupleHeaderGetXmin(tup->t_data);
				cache->fn_tid = tup->t_self;
			}
		}

		heap_freetuple(tup);
		table_close(proc_rel, NoLock);
		CommandCounterIncrement();
		return;
	}

	heap_freetuple(tup);
	table_close(proc_rel, NoLock);
}

/*
 * Inspect `query_ctx` around `source_str` to determine:
 *   - `func_name_out`: unqualified lowercase name of the enclosing function call
 *   - `first_arg_out`: raw SQL text of the 1st argument (if `source_str` is arg 2+)
 *   - `has_coldeflist_out`: true if the call is followed by `AS (...)` or `alias(...)`
 */
static void
plv8_extract_enclosing_call(const char *query_ctx,
							const char *source_str,
							char **func_name_out,
							char **first_arg_out,
							bool *has_coldeflist_out)
{
	*func_name_out = pstrdup("graph_table");
	*first_arg_out = pstrdup("");
	*has_coldeflist_out = false;

	if (query_ctx == NULL || *query_ctx == '\0' || source_str == NULL || *source_str == '\0')
		return;

	size_t src_len = strlen(source_str);
	size_t match_len = src_len > 32 ? 32 : src_len;
	char *needle = pnstrdup(source_str, match_len);
	const char *pos = strstr(query_ctx, needle);
	pfree(needle);

	if (pos == NULL)
		return;

	/* Scan backwards from `pos` to find the enclosing `func_name(` */
	int depth = 0;
	const char *open_paren = NULL;
	const char *first_comma_at_zero = NULL;
	const char *quote_start = pos;

	/* Step back over the opening quote (`'` or `$tag$`) */
	if (quote_start > query_ctx && quote_start[-1] == '\'')
		quote_start--;
	else if (quote_start > query_ctx && quote_start[-1] == '$')
	{
		quote_start--;
		while (quote_start > query_ctx &&
			   (isalnum((unsigned char) quote_start[-1]) || quote_start[-1] == '_'))
			quote_start--;
		if (quote_start > query_ctx && quote_start[-1] == '$')
			quote_start--;
	}

	for (const char *p = quote_start - 1; p >= query_ctx; p--)
	{
		char c = *p;
		if (c == ')')
			depth++;
		else if (c == '(')
		{
			if (depth == 0)
			{
				open_paren = p;
				break;
			}
			depth--;
		}
		else if (c == ',' && depth == 0)
		{
			first_comma_at_zero = p;
		}
	}

	if (open_paren != NULL)
	{
		/* Extract function name preceding `open_paren` */
		const char *fn_end = open_paren;
		while (fn_end > query_ctx && isspace((unsigned char) fn_end[-1]))
			fn_end--;
		const char *fn_start = fn_end;
		while (fn_start > query_ctx &&
			   (isalnum((unsigned char) fn_start[-1]) ||
				fn_start[-1] == '_' || fn_start[-1] == '.' || fn_start[-1] == '"'))
			fn_start--;
		if (fn_end > fn_start)
		{
			char *raw_fn = pnstrdup(fn_start, fn_end - fn_start);
			char norm[NAMEDATALEN];
			plv8_normalize_func_name(raw_fn, norm, sizeof(norm));
			pfree(raw_fn);
			if (norm[0] != '\0')
			{
				pfree(*func_name_out);
				*func_name_out = pstrdup(norm);
			}
		}

		/* Extract 1st argument if `source_str` is the 2nd+ argument */
		if (first_comma_at_zero != NULL && first_comma_at_zero > open_paren + 1)
		{
			const char *a_start = open_paren + 1;
			const char *a_end = first_comma_at_zero;
			while (a_start < a_end && isspace((unsigned char) *a_start))
				a_start++;
			while (a_end > a_start && isspace((unsigned char) a_end[-1]))
				a_end--;
			pfree(*first_arg_out);
			*first_arg_out = pnstrdup(a_start, a_end - a_start);
		}

		/* Scan forward from `open_paren` to find the matching `)` and check for `AS (...)` */
		int fdepth = 0;
		bool in_squote = false;
		const char *close_paren = NULL;
		for (const char *p = open_paren; *p != '\0'; p++)
		{
			char c = *p;
			if (in_squote)
			{
				if (c == '\'')
				{
					if (p[1] == '\'')
						p++;
					else
						in_squote = false;
				}
				continue;
			}
			if (c == '\'')
			{
				in_squote = true;
				continue;
			}
			if (c == '(')
				fdepth++;
			else if (c == ')')
			{
				fdepth--;
				if (fdepth == 0)
				{
					close_paren = p;
					break;
				}
			}
		}

		if (close_paren != NULL)
		{
			const char *after = close_paren + 1;
			while (*after != '\0' && isspace((unsigned char) *after))
				after++;

			/* Check optional `AS` keyword */
			if (pg_strncasecmp(after, "AS", 2) == 0 &&
				(isspace((unsigned char) after[2]) || after[2] == '('))
			{
				after += 2;
				while (*after != '\0' && isspace((unsigned char) *after))
					after++;
				if (*after == '(')
				{
					*has_coldeflist_out = true;
					return;
				}
			}

			/* Check optional alias identifier followed by `(` */
			if (isalpha((unsigned char) *after) || *after == '_' || *after == '"')
			{
				const char *tok_start = after;
				if (*after == '"')
				{
					after++;
					while (*after != '\0' && *after != '"')
						after++;
					if (*after == '"')
						after++;
				}
				else
				{
					while (isalnum((unsigned char) *after) || *after == '_')
						after++;
				}
				size_t tok_len = (size_t) (after - tok_start);
				/* Ignore SQL clause keywords that cannot be followed by a coldeflist */
				if (!(tok_len == 5 && pg_strncasecmp(tok_start, "WHERE", 5) == 0) &&
					!(tok_len == 4 && pg_strncasecmp(tok_start, "JOIN", 4) == 0) &&
					!(tok_len == 2 && pg_strncasecmp(tok_start, "ON", 2) == 0) &&
					!(tok_len == 5 && pg_strncasecmp(tok_start, "ORDER", 5) == 0) &&
					!(tok_len == 5 && pg_strncasecmp(tok_start, "GROUP", 5) == 0) &&
					!(tok_len == 5 && pg_strncasecmp(tok_start, "LIMIT", 5) == 0) &&
					!(tok_len == 5 && pg_strncasecmp(tok_start, "CROSS", 5) == 0) &&
					!(tok_len == 4 && pg_strncasecmp(tok_start, "LEFT", 4) == 0) &&
					!(tok_len == 5 && pg_strncasecmp(tok_start, "INNER", 5) == 0))
				{
					while (*after != '\0' && isspace((unsigned char) *after))
						after++;
					if (*after == '(')
						*has_coldeflist_out = true;
				}
			}
		}
	}
}

/*
 * Parse the tab-separated output from a `<func>_infer_columns(query_ctx, source_str)`
 * SQL hook:
 *   Line 0: "COLDEF\t1" or "COLDEF\t0\t<first_arg>"
 *   Line k: "<col_name>\t<type_oid>\t<typmod>\t<collation_oid>"
 */
static void
plv8_parse_and_apply_inferred_tsv(const char *func_name,
								  const char *source_str,
								  const char *fallback_first_arg,
								  bool fallback_has_coldef,
								  const char *tsv)
{
	if (tsv == NULL || *tsv == '\0')
		return;

	bool has_coldef = fallback_has_coldef;
	char *first_arg = fallback_first_arg ? pstrdup(fallback_first_arg) : NULL;
	char *copy = pstrdup(tsv);
	char *saveptr = NULL;
	Plv8ColMeta cols[FUNC_MAX_ARGS];
	int ncols = 0;
	int line_idx = 0;

	for (char *line = strtok_r(copy, "\n", &saveptr);
		 line != NULL;
		 line = strtok_r(NULL, "\n", &saveptr))
	{
		CHECK_FOR_INTERRUPTS();
		if (line_idx == 0 && strncmp(line, "COLDEF\t", 7) == 0)
		{
			if (strncmp(line, "COLDEF\t1", 8) == 0)
				has_coldef = true;
			else if (strncmp(line, "COLDEF\t0", 8) == 0)
			{
				has_coldef = false;
				if (line[8] == '\t' && line[9] != '\0')
					first_arg = pstrdup(line + 9);
			}
			line_idx++;
			continue;
		}
		if (ncols < FUNC_MAX_ARGS)
		{
			char *tab1 = strchr(line, '\t');
			if (tab1 != NULL)
			{
				*tab1 = '\0';
				char *tab2 = strchr(tab1 + 1, '\t');
				if (tab2 != NULL)
				{
					*tab2 = '\0';
					char *tab3 = strchr(tab2 + 1, '\t');
					if (tab3 != NULL)
						*tab3 = '\0';
					cols[ncols].name = pstrdup(line);
					cols[ncols].type_oid = (Oid) atooid(tab1 + 1);
					cols[ncols].typmod = atoi(tab2 + 1);
					cols[ncols].collation_oid = tab3 ? (Oid) atooid(tab3 + 1) : InvalidOid;
					ncols++;
				}
			}
		}
		line_idx++;
	}

	if (ncols > 0)
		plv8_save_inferred_columns(func_name, source_str, first_arg, ncols, cols);

	if (has_coldef)
		plv8_update_func_overloads_columns(func_name, 0, NULL);
	else if (ncols > 0)
		plv8_update_func_overloads_columns(func_name, ncols, cols);
}

/*
 * Helper to extract an array of `Plv8ColMeta` from a V8 Array of column objects.
 */
static int
plv8_extract_cols_from_v8_array(Isolate *isolate,
								Local<Context> context,
								Local<Array> arr,
								Plv8ColMeta *out_cols)
{
	uint32_t len = arr->Length();
	if (len > FUNC_MAX_ARGS)
		len = FUNC_MAX_ARGS;
	int ncols = 0;

	for (uint32_t i = 0; i < len; i++)
	{
		CHECK_FOR_INTERRUPTS();
		Local<v8::Value> elem;
		if (!arr->Get(context, i).ToLocal(&elem) || !elem->IsObject())
			continue;
		Local<Object> obj = Local<Object>::Cast(elem);

		const char *cname = "?column?";
		Local<v8::Value> name_val;
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

		out_cols[ncols].name = (char *) cname;
		out_cols[ncols].type_oid = type_oid;
		out_cols[ncols].typmod = typmod;
		out_cols[ncols].collation_oid = coll_oid;
		ncols++;
	}
	return ncols;
}

/*
 * plv8_source_in(cstring) -> plv8_source
 */
Datum
plv8_source_in(PG_FUNCTION_ARGS)
{
	char *str = PG_GETARG_CSTRING(0);
	const char *query_ctx = debug_query_string ? debug_query_string : "";

	if (!plv8_in_support_internal_call && *str != '\0')
	{
		char *func_name = NULL;
		char *first_arg = NULL;
		bool has_coldeflist = false;

		plv8_extract_enclosing_call(query_ctx, str, &func_name, &first_arg, &has_coldeflist);

		Plv8SupportHookEntry *hook = plv8_find_or_create_hook_entry(func_name, false);
		if (hook != NULL && !hook->infer_js_cb.IsEmpty())
		{
			char *inferred_tsv = NULL;
			Plv8ColMeta inferred_cols[FUNC_MAX_ARGS];
			int inferred_ncols = -1;

			plv8_in_support_internal_call = true;
			try
			{
				plv8_context *ctx = GetPlv8Context();
				CurrentContextScope context_scope(ctx);
				Isolate *isolate = ctx->isolate;
				Isolate::Scope iscope(isolate);
				HandleScope handle_scope(isolate);
				Local<Context> context = ctx->localContext();
				Context::Scope cscope(context);
				HandlerExecutionScope hscope(context);

				Local<Function> fn = Local<Function>::New(isolate, hook->infer_js_cb);
				Local<Object> req = Object::New(isolate);
				req->Set(context, ToString("func_name"), ToString(func_name)).Check();
				req->Set(context, ToString("query_string"), ToString(query_ctx)).Check();
				req->Set(context, ToString("source_arg"), ToString(str)).Check();
				req->Set(context, ToString("first_arg"), ToString(first_arg ? first_arg : "")).Check();
				req->Set(context, ToString("has_coldeflist"), v8::Boolean::New(isolate, has_coldeflist)).Check();

				Local<v8::Value> call_args[1] = { req };
				Local<v8::Value> res = DoCall(context, fn, context->Global(), 1, call_args, false);

				if (!res.IsEmpty() && res->IsString())
				{
					CString tsv_cs(res);
					inferred_tsv = pstrdup(tsv_cs.str());
				}
				else if (!res.IsEmpty() && res->IsArray())
				{
					inferred_ncols = plv8_extract_cols_from_v8_array(isolate, context, Local<Array>::Cast(res), inferred_cols);
				}
				else if (!res.IsEmpty() && res->IsObject())
				{
					Local<Object> robj = Local<Object>::Cast(res);
					Local<v8::Value> cd_val;
					if (robj->Get(context, ToString("has_coldeflist")).ToLocal(&cd_val) && cd_val->IsBoolean())
						has_coldeflist = cd_val->BooleanValue(isolate);
					Local<v8::Value> fa_val;
					if (robj->Get(context, ToString("first_arg")).ToLocal(&fa_val) && fa_val->IsString())
					{
						CString facs(fa_val);
						first_arg = pstrdup(facs.str(""));
					}
					Local<v8::Value> cols_val;
					if (robj->Get(context, ToString("columns")).ToLocal(&cols_val) && cols_val->IsArray())
					{
						inferred_ncols = plv8_extract_cols_from_v8_array(isolate, context, Local<Array>::Cast(cols_val), inferred_cols);
					}
				}
				plv8_in_support_internal_call = false;
			}
			catch (js_error& e)
			{
				plv8_in_support_internal_call = false;
				e.rethrow();
			}
			catch (pg_error& e)
			{
				plv8_in_support_internal_call = false;
				e.rethrow();
			}

			if (inferred_tsv != NULL)
			{
				plv8_parse_and_apply_inferred_tsv(func_name, str, first_arg, has_coldeflist, inferred_tsv);
			}
			else if (inferred_ncols >= 0)
			{
				if (inferred_ncols > 0)
					plv8_save_inferred_columns(func_name, str, first_arg, inferred_ncols, inferred_cols);
				if (has_coldeflist)
					plv8_update_func_overloads_columns(func_name, 0, NULL);
				else if (inferred_ncols > 0)
					plv8_update_func_overloads_columns(func_name, inferred_ncols, inferred_cols);
			}
		}
		else
		{
			Oid infer_oid = plv8_find_infer_sql_func_oid(func_name);
			if (OidIsValid(infer_oid))
			{
				char *tsv = NULL;
				plv8_in_support_internal_call = true;
				PG_TRY();
				{
					Datum res_datum = OidFunctionCall2(
						infer_oid,
						CStringGetTextDatum(query_ctx),
						CStringGetTextDatum(str));
					tsv = TextDatumGetCString(res_datum);
				}
				PG_FINALLY();
				{
					plv8_in_support_internal_call = false;
				}
				PG_END_TRY();
				if (tsv != NULL)
					plv8_parse_and_apply_inferred_tsv(func_name, str, first_arg, has_coldeflist, tsv);
			}
		}
	}

	PG_RETURN_TEXT_P(cstring_to_text(str));
}

/*
 * plv8_source_out(plv8_source) -> cstring
 */
Datum
plv8_source_out(PG_FUNCTION_ARGS)
{
	Datum txt = PG_GETARG_DATUM(0);

	PG_RETURN_CSTRING(TextDatumGetCString(txt));
}

/*
 * Extract a C string from a Const node (or RelabelType/CoerceViaIO/CoerceToDomain wrapping a Const).
 */
static char *
plv8_extract_const_string(Node *node)
{
	if (node == NULL)
		return NULL;
	while (IsA(node, RelabelType) || IsA(node, CoerceViaIO) || IsA(node, CoerceToDomain))
	{
		CHECK_FOR_INTERRUPTS();
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		else if (IsA(node, CoerceViaIO))
			node = (Node *) ((CoerceViaIO *) node)->arg;
		else
			node = (Node *) ((CoerceToDomain *) node)->arg;
	}
	if (IsA(node, Const))
	{
		Const *c = (Const *) node;
		Oid typoutput;
		bool typIsVarlena;

		if (c->constisnull)
			return NULL;
		getTypeOutputInfo(c->consttype, &typoutput, &typIsVarlena);
		return OidOutputFunctionCall(typoutput, c->constvalue);
	}
	return NULL;
}

static bool
plv8_is_source_type_node(Node *node)
{
	if (node == NULL)
		return false;
	Oid typid = getBaseType(exprType(node));
	if (!OidIsValid(typid))
		return false;
	HeapTuple tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typid));
	if (!HeapTupleIsValid(tp))
		return false;
	Form_pg_type tform = (Form_pg_type) GETSTRUCT(tp);
	bool match = (strcmp(NameStr(tform->typname), "plv8_source") == 0 ||
				  strcmp(NameStr(tform->typname), "pgq_pattern") == 0);
	ReleaseSysCache(tp);
	return match;
}

static bool
plv8_is_null_const_node(Node *node)
{
	while (node != NULL && (IsA(node, RelabelType) || IsA(node, CoerceViaIO) || IsA(node, CoerceToDomain)))
	{
		CHECK_FOR_INTERRUPTS();
		if (IsA(node, RelabelType))
			node = (Node *) ((RelabelType *) node)->arg;
		else if (IsA(node, CoerceViaIO))
			node = (Node *) ((CoerceViaIO *) node)->arg;
		else
			node = (Node *) ((CoerceToDomain *) node)->arg;
	}
	return (node != NULL && IsA(node, Const) && ((Const *) node)->constisnull);
}

/*
 * Extract the 1st argument string (constant text/JSONB or function call `fn()`)
 * from the 1st argument of a supported function call.
 */
static char *
plv8_extract_first_arg_string(Node *arg1)
{
	char *const_str = plv8_extract_const_string(arg1);
	if (const_str != NULL)
		return const_str;

	while (arg1 != NULL && (IsA(arg1, RelabelType) || IsA(arg1, CoerceViaIO) || IsA(arg1, CoerceToDomain)))
	{
		CHECK_FOR_INTERRUPTS();
		if (IsA(arg1, RelabelType))
			arg1 = (Node *) ((RelabelType *) arg1)->arg;
		else if (IsA(arg1, CoerceViaIO))
			arg1 = (Node *) ((CoerceViaIO *) arg1)->arg;
		else
			arg1 = (Node *) ((CoerceToDomain *) arg1)->arg;
	}

	if (arg1 != NULL && IsA(arg1, FuncExpr))
	{
		FuncExpr *fe = (FuncExpr *) arg1;
		char *funcname = get_func_name(fe->funcid);
		Oid nspid = get_func_namespace(fe->funcid);
		char *nspname = OidIsValid(nspid) ? get_namespace_name(nspid) : NULL;

		if (funcname != NULL)
		{
			if (nspname != NULL)
				return psprintf("%s.%s()", quote_identifier(nspname), quote_identifier(funcname));
			return psprintf("%s()", quote_identifier(funcname));
		}
	}

	return NULL;
}

static inline const char *
plv8_node_strval(void *v)
{
#if PG_VERSION_NUM >= 150000
	return ((PG_Node_String *) v)->sval;
#else
	return ((::Value *) v)->val.str;
#endif
}

/*
 * Append RTE column descriptors from `rtable` into `lat_buf` JSON array.
 */
static void
plv8_append_rtable_to_lateral_json(StringInfo lat_buf, List *rtable,
								   RangeTblEntry *skip_rte, int sublevels_up,
								   bool *first_rte)
{
	ListCell *lc;
	int rtindex = 0;

	foreach(lc, rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);
		List *colnames = NIL;
		List *colvars = NIL;
		ListCell *name_lc;
		ListCell *var_lc;
		bool first_col = true;

		CHECK_FOR_INTERRUPTS();
		rtindex++;

		if (rte == skip_rte || rte->eref == NULL || rte->eref->aliasname == NULL)
			continue;
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery == NULL)
			continue;
		if (rte->rtekind == RTE_JOIN &&
			(rte->joinaliasvars == NIL ||
			 list_length(rte->eref->colnames) != list_length(rte->joinaliasvars)))
			continue;
		if (rte->rtekind == RTE_RESULT
#if PG_VERSION_NUM >= 180000
			|| rte->rtekind == RTE_GROUP
#endif
			)
			continue;

		if (rte->rtekind == RTE_FUNCTION)
		{
			bool can_expand = true;
			ListCell *flc;

			foreach(flc, rte->functions)
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) lfirst(flc);
				if (rtfunc->funccolnames == NIL)
				{
					Oid funcrettype = InvalidOid;
					TupleDesc tupdesc = NULL;
					TypeFuncClass functypclass =
						get_expr_result_type(rtfunc->funcexpr, &funcrettype, &tupdesc);

					if (functypclass == TYPEFUNC_COMPOSITE ||
						functypclass == TYPEFUNC_COMPOSITE_DOMAIN)
					{
						if (tupdesc == NULL || rtfunc->funccolcount > tupdesc->natts)
						{
							can_expand = false;
							break;
						}
					}
					else if (functypclass == TYPEFUNC_RECORD)
					{
						if (list_length(rtfunc->funccoltypes) != rtfunc->funccolcount)
						{
							can_expand = false;
							break;
						}
					}
					else if (functypclass != TYPEFUNC_SCALAR)
					{
						can_expand = false;
						break;
					}
				}
			}

			if (!can_expand)
			{
				if (list_length(rte->functions) == 1)
				{
					RangeTblFunction *rtfunc = (RangeTblFunction *) linitial(rte->functions);
					if (rtfunc->funcexpr != NULL && IsA(rtfunc->funcexpr, FuncExpr))
					{
						FuncExpr *fe = (FuncExpr *) rtfunc->funcexpr;
						int fnargs = list_length(fe->args);
						if (fnargs >= 1)
						{
							bool src_first = (fnargs == 1 ||
											  plv8_is_source_type_node((Node *) linitial(fe->args)));
							Node *snode = src_first
								? (Node *) linitial(fe->args)
								: (fnargs >= 2 ? (Node *) lsecond(fe->args) : NULL);
							char *rsrc = plv8_extract_const_string(snode);
							Plv8InferredEntry *inf = plv8_find_inferred_columns(rsrc);
							if (inf != NULL &&
								inf->ncols == list_length(rte->eref->colnames))
							{
								ListCell *clc;
								int cidx = 0;
								foreach(clc, rte->eref->colnames)
								{
									const char *cn = plv8_node_strval(lfirst(clc));
									colnames = lappend(colnames, makeString(pstrdup(cn)));
									colvars = lappend(colvars,
													  makeVar(rtindex,
															  cidx + 1,
															  inf->cols[cidx].type_oid,
															  inf->cols[cidx].typmod,
															  inf->cols[cidx].collation_oid,
															  sublevels_up));
									cidx++;
								}
							}
						}
					}
				}
				if (colnames == NIL || colvars == NIL)
					continue;
			}
		}

		if (colnames == NIL || colvars == NIL)
		{
			PG_TRY();
			{
#if PG_VERSION_NUM >= 180000
				expandRTE(rte, rtindex, sublevels_up, VAR_RETURNING_DEFAULT, -1,
						  false, &colnames, &colvars);
#else
				expandRTE(rte, rtindex, sublevels_up, -1,
						  false, &colnames, &colvars);
#endif
			}
			PG_CATCH();
			{
				FlushErrorState();
				colnames = NIL;
				colvars = NIL;
			}
			PG_END_TRY();
		}

		if (colnames == NIL || colvars == NIL)
			continue;

		if (!(*first_rte))
			appendStringInfoChar(lat_buf, ',');
		*first_rte = false;

		appendStringInfoString(lat_buf, "{\"alias\":");
		plv8_append_json_string(lat_buf, rte->eref->aliasname);
		appendStringInfo(lat_buf, ",\"rtindex\":%d,\"columns\":[",
						 sublevels_up > 0 ? -rtindex : rtindex);

		forboth(name_lc, colnames, var_lc, colvars)
		{
			const char *colname = plv8_node_strval(lfirst(name_lc));
			Node *varnode = (Node *) lfirst(var_lc);

			CHECK_FOR_INTERRUPTS();
			if (colname == NULL || *colname == '\0' || varnode == NULL || !IsA(varnode, Var))
				continue;

			Var *var = (Var *) varnode;
			if (!OidIsValid(var->vartype) || var->varattno <= 0)
				continue;

			if (!first_col)
				appendStringInfoChar(lat_buf, ',');
			first_col = false;

			appendStringInfoString(lat_buf, "{\"name\":");
			plv8_append_json_string(lat_buf, colname);
			appendStringInfo(lat_buf,
							 ",\"attno\":%d,\"type_oid\":%u,\"typmod\":%d,\"collation_oid\":%u,\"type_sql\":",
							 (int) var->varattno, var->vartype, var->vartypmod, var->varcollid);
			plv8_append_json_string(lat_buf, format_type_with_typemod(var->vartype, var->vartypmod));
			appendStringInfoChar(lat_buf, '}');
		}
		appendStringInfoString(lat_buf, "]}");
	}
}

/*
 * plv8_call_support(internal) -> internal
 *
 * General-purpose planner support hook (`SUPPORT plv8_call_support`) for
 * PL/v8 and custom `pl/<lang>` functions.
 */
Datum
plv8_call_support(PG_FUNCTION_ARGS)
{
	Node *rawreq = (Node *) PG_GETARG_POINTER(0);

	if (rawreq == NULL)
		PG_RETURN_POINTER(NULL);

	if (IsA(rawreq, SupportRequestCost))
	{
		SupportRequestCost *req = (SupportRequestCost *) rawreq;

		req->startup = 10.0;
		req->per_tuple = 0.01;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestRows))
	{
		SupportRequestRows *req = (SupportRequestRows *) rawreq;

		req->rows = 100.0;
		PG_RETURN_POINTER(req);
	}

	if (IsA(rawreq, SupportRequestSimplify))
	{
		SupportRequestSimplify *req = (SupportRequestSimplify *) rawreq;
		PlannerInfo *root = req->root;
		FuncExpr *fexpr = req->fcall;

		if (plv8_in_support_internal_call || root == NULL || root->parse == NULL ||
			fexpr == NULL)
			PG_RETURN_POINTER(NULL);

		char *raw_func_name = get_func_name(fexpr->funcid);
		char func_name[NAMEDATALEN];
		plv8_normalize_func_name(raw_func_name ? raw_func_name : "graph_table",
								 func_name, sizeof(func_name));

		Oid prolang_oid = InvalidOid;
		char lang_name[NAMEDATALEN];
		lang_name[0] = '\0';
		bool is_custom_lang_srf = false;
		char *fn_prosrc = NULL;

		{
			HeapTuple ftup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fexpr->funcid));
			if (HeapTupleIsValid(ftup))
			{
				Form_pg_proc fform = (Form_pg_proc) GETSTRUCT(ftup);
				prolang_oid = fform->prolang;
				bool src_isnull = false;
				Datum src_d = SysCacheGetAttr(PROCOID, ftup, Anum_pg_proc_prosrc, &src_isnull);
				if (!src_isnull)
					fn_prosrc = TextDatumGetCString(src_d);
				ReleaseSysCache(ftup);
			}
			if (OidIsValid(prolang_oid))
			{
				HeapTuple ltup = SearchSysCache1(LANGOID, ObjectIdGetDatum(prolang_oid));
				if (HeapTupleIsValid(ltup))
				{
					Form_pg_language lform = (Form_pg_language) GETSTRUCT(ltup);
					plv8_normalize_func_name(NameStr(lform->lanname), lang_name, sizeof(lang_name));
					if (strcmp(lang_name, "plv8") != 0 && strcmp(lang_name, "pljs") != 0)
						is_custom_lang_srf = true;
					ReleaseSysCache(ltup);
				}
			}
		}

		int nargs = list_length(fexpr->args);
		char *first_arg_str = NULL;
		char *source_str = NULL;
		char *params_str = NULL;
		Node *params_arg_node = NULL;
		bool source_is_first_arg = (nargs == 1 ||
									(nargs >= 1 && plv8_is_source_type_node((Node *) linitial(fexpr->args))));

		if (nargs >= 1)
		{
			if (source_is_first_arg)
			{
				source_str = plv8_extract_const_string((Node *) linitial(fexpr->args));
				first_arg_str = pstrdup("");
				if (nargs >= 2)
					params_arg_node = (Node *) lsecond(fexpr->args);
			}
			else if (nargs >= 2)
			{
				source_str = plv8_extract_const_string((Node *) lsecond(fexpr->args));
				first_arg_str = plv8_extract_first_arg_string((Node *) linitial(fexpr->args));
				if (nargs >= 3)
					params_arg_node = (Node *) lthird(fexpr->args);
			}
		}

		/*
		 * Locate the RTE_FUNCTION in `root->parse->rtable` that corresponds to
		 * this `SupportRequestSimplify` call.
		 */
		RangeTblEntry *target_rte = NULL;
		RangeTblFunction *target_rtfunc = NULL;
		ListCell *lc;

		foreach(lc, root->parse->rtable)
		{
			RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

			CHECK_FOR_INTERRUPTS();
			if (rte->rtekind == RTE_FUNCTION && list_length(rte->functions) == 1)
			{
				RangeTblFunction *rtfunc = (RangeTblFunction *) linitial(rte->functions);

				if (rtfunc->funcexpr != NULL && IsA(rtfunc->funcexpr, FuncExpr))
				{
					FuncExpr *rte_fe = (FuncExpr *) rtfunc->funcexpr;

					if (rte_fe == fexpr)
					{
						target_rte = rte;
						target_rtfunc = rtfunc;
						break;
					}
					if (rte_fe->funcid == fexpr->funcid &&
						list_length(rte_fe->args) == nargs)
					{
						if (source_str != NULL &&
							list_length(rte_fe->args) >= (source_is_first_arg ? 1 : 2))
						{
							Node *src_node = source_is_first_arg
								? (Node *) linitial(rte_fe->args)
								: (Node *) lsecond(rte_fe->args);
							char *rte_src = plv8_extract_const_string(src_node);

							if (rte_src != NULL && strcmp(rte_src, source_str) == 0)
							{
								target_rte = rte;
								target_rtfunc = rtfunc;
								break;
							}
						}
						if (target_rte == NULL && equal(rte_fe, fexpr))
						{
							target_rte = rte;
							target_rtfunc = rtfunc;
							break;
						}
						if (target_rte == NULL)
						{
							target_rte = rte;
							target_rtfunc = rtfunc;
						}
					}
				}
			}
		}

		if (target_rte == NULL)
			PG_RETURN_POINTER(NULL);

		if (!is_custom_lang_srf)
		{
			if (source_str == NULL)
				PG_RETURN_POINTER(NULL);

			if (first_arg_str == NULL)
			{
				Plv8InferredEntry *cached = plv8_find_inferred_columns(source_str);
				if (cached != NULL && cached->first_arg != NULL)
					first_arg_str = pstrdup(cached->first_arg);
			}
			if (first_arg_str == NULL)
				PG_RETURN_POINTER(NULL);

			if (params_arg_node != NULL && !plv8_is_null_const_node(params_arg_node))
			{
				params_str = plv8_extract_const_string(params_arg_node);
				if (params_str == NULL)
				{
					/*
					 * Non-constant runtime params argument: fall back to runtime SRF.
					 * Note: if `target_rtfunc->funccolnames != NIL`, reset stamped
					 * columns now; otherwise leave them until `plv8_call_handler`
					 * so `ExecInitFunctionScan()` can build `rsinfo->expectedDesc`!
					 */
					if (target_rtfunc->funccolnames != NIL)
						plv8_update_func_overloads_columns(func_name, 0, NULL);
					PG_RETURN_POINTER(NULL);
				}
			}
		}
		else
		{
			if (source_str == NULL)
				source_str = fn_prosrc ? fn_prosrc : pstrdup("");
			if (first_arg_str == NULL)
				first_arg_str = pstrdup("");
		}

		/*
		 * Build expected output columns JSON and `expected_cols` array.
		 */
		StringInfoData expected_cols_buf;
		initStringInfo(&expected_cols_buf);
		appendStringInfoChar(&expected_cols_buf, '[');

		int expected_ncols = 0;
		Plv8ColMeta expected_cols[FUNC_MAX_ARGS];

		if (target_rtfunc->funccolnames != NIL)
		{
			ListCell *cn_lc;
			ListCell *ct_lc;
			ListCell *cm_lc;
			ListCell *cc_lc;
			bool first = true;

			forfour(cn_lc, target_rtfunc->funccolnames,
					ct_lc, target_rtfunc->funccoltypes,
					cm_lc, target_rtfunc->funccoltypmods,
					cc_lc, target_rtfunc->funccolcollations)
			{
				const char *cname = plv8_node_strval(lfirst(cn_lc));
				Oid ctype = lfirst_oid(ct_lc);
				int32 ctypmod = lfirst_int(cm_lc);
				Oid ccoll = lfirst_oid(cc_lc);

				CHECK_FOR_INTERRUPTS();
				if (!first)
					appendStringInfoChar(&expected_cols_buf, ',');
				first = false;

				appendStringInfoString(&expected_cols_buf, "{\"name\":");
				plv8_append_json_string(&expected_cols_buf, cname);
				appendStringInfo(&expected_cols_buf,
								 ",\"type_oid\":%u,\"typmod\":%d,\"collation_oid\":%u,\"type_sql\":",
								 ctype, ctypmod, ccoll);
				plv8_append_json_string(&expected_cols_buf,
										format_type_with_typemod(ctype, ctypmod));
				appendStringInfoChar(&expected_cols_buf, '}');

				if (expected_ncols < FUNC_MAX_ARGS)
				{
					expected_cols[expected_ncols].name = pstrdup(cname);
					expected_cols[expected_ncols].type_oid = ctype;
					expected_cols[expected_ncols].typmod = ctypmod;
					expected_cols[expected_ncols].collation_oid = ccoll;
					expected_ncols++;
				}
			}
		}
		else
		{
			Plv8InferredEntry *cached = (!is_custom_lang_srf && source_str != NULL)
				? plv8_find_inferred_columns(source_str)
				: NULL;
			if (cached != NULL && cached->ncols > 0)
			{
				for (int i = 0; i < cached->ncols; i++)
				{
					CHECK_FOR_INTERRUPTS();
					if (i > 0)
						appendStringInfoChar(&expected_cols_buf, ',');
					appendStringInfoString(&expected_cols_buf, "{\"name\":");
					plv8_append_json_string(&expected_cols_buf, cached->cols[i].name);
					appendStringInfo(&expected_cols_buf,
									 ",\"type_oid\":%u,\"typmod\":%d,\"collation_oid\":%u,\"type_sql\":",
									 cached->cols[i].type_oid,
									 cached->cols[i].typmod,
									 cached->cols[i].collation_oid);
					plv8_append_json_string(&expected_cols_buf,
											format_type_with_typemod(cached->cols[i].type_oid,
																	 cached->cols[i].typmod));
					appendStringInfoChar(&expected_cols_buf, '}');

					if (expected_ncols < FUNC_MAX_ARGS)
						expected_cols[expected_ncols++] = cached->cols[i];
				}
			}
			else
			{
				Oid funcrettype = InvalidOid;
				TupleDesc tupdesc = NULL;
				TypeFuncClass functypclass =
					get_expr_result_type((Node *) fexpr, &funcrettype, &tupdesc);
				if ((functypclass == TYPEFUNC_COMPOSITE ||
					 functypclass == TYPEFUNC_COMPOSITE_DOMAIN) &&
					tupdesc != NULL)
				{
					bool first = true;
					for (int a = 0; a < tupdesc->natts && expected_ncols < FUNC_MAX_ARGS; a++)
					{
						Form_pg_attribute attr = TupleDescAttr(tupdesc, a);
						if (attr->attisdropped)
							continue;
						if (!first)
							appendStringInfoChar(&expected_cols_buf, ',');
						first = false;
						const char *cname = NameStr(attr->attname);
						appendStringInfoString(&expected_cols_buf, "{\"name\":");
						plv8_append_json_string(&expected_cols_buf, cname);
						appendStringInfo(&expected_cols_buf,
										 ",\"type_oid\":%u,\"typmod\":%d,\"collation_oid\":%u,\"type_sql\":",
										 attr->atttypid, attr->atttypmod, attr->attcollation);
						plv8_append_json_string(&expected_cols_buf,
												format_type_with_typemod(attr->atttypid, attr->atttypmod));
						appendStringInfoChar(&expected_cols_buf, '}');

						expected_cols[expected_ncols].name = pstrdup(cname);
						expected_cols[expected_ncols].type_oid = attr->atttypid;
						expected_cols[expected_ncols].typmod = attr->atttypmod;
						expected_cols[expected_ncols].collation_oid = attr->attcollation;
						expected_ncols++;
					}
				}
				else if (functypclass == TYPEFUNC_SCALAR && OidIsValid(funcrettype))
				{
					const char *cname = (target_rte->eref && target_rte->eref->colnames != NIL)
						? plv8_node_strval(linitial(target_rte->eref->colnames))
						: func_name;
					int32 ctypmod = exprTypmod((Node *) fexpr);
					Oid ccoll = exprCollation((Node *) fexpr);
					appendStringInfoString(&expected_cols_buf, "{\"name\":");
					plv8_append_json_string(&expected_cols_buf, cname);
					appendStringInfo(&expected_cols_buf,
									 ",\"type_oid\":%u,\"typmod\":%d,\"collation_oid\":%u,\"type_sql\":",
									 funcrettype, ctypmod, ccoll);
					plv8_append_json_string(&expected_cols_buf,
											format_type_with_typemod(funcrettype, ctypmod));
					appendStringInfoChar(&expected_cols_buf, '}');

					expected_cols[0].name = pstrdup(cname);
					expected_cols[0].type_oid = funcrettype;
					expected_cols[0].typmod = ctypmod;
					expected_cols[0].collation_oid = ccoll;
					expected_ncols = 1;
				}
			}
		}
		appendStringInfoChar(&expected_cols_buf, ']');

		/*
		 * Build `call_args` JSON array describing the arguments passed to `fexpr`.
		 */
		StringInfoData call_args_buf;
		initStringInfo(&call_args_buf);
		appendStringInfoChar(&call_args_buf, '[');
		{
			ListCell *alc;
			int aidx = 0;
			foreach(alc, fexpr->args)
			{
				Node *anode = (Node *) lfirst(alc);
				Oid atype = exprType(anode);
				int32 atypmod = exprTypmod(anode);
				bool ais_null = plv8_is_null_const_node(anode);
				char *aconst = ais_null ? NULL : plv8_extract_const_string(anode);
				bool ais_const = (ais_null || aconst != NULL);

				if (aidx > 0)
					appendStringInfoChar(&call_args_buf, ',');
				appendStringInfo(&call_args_buf,
								 "{\"index\":%d,\"type_oid\":%u,\"typmod\":%d,\"type_sql\":",
								 aidx + 1, atype, atypmod);
				plv8_append_json_string(&call_args_buf, format_type_with_typemod(atype, atypmod));
				appendStringInfo(&call_args_buf,
								 ",\"is_const\":%s,\"is_null\":%s,\"const_val\":",
								 ais_const ? "true" : "false",
								 ais_null ? "true" : "false");
				if (aconst != NULL)
					plv8_append_json_string(&call_args_buf, aconst);
				else
					appendStringInfoString(&call_args_buf, "null");
				appendStringInfoChar(&call_args_buf, '}');
				aidx++;
			}
		}
		appendStringInfoChar(&call_args_buf, ']');

		/*
		 * Build `lateral_env` JSON from `root->parse->rtable` (and `parent_root`).
		 */
		StringInfoData lateral_env_buf;
		bool first_rte = true;
		initStringInfo(&lateral_env_buf);
		appendStringInfoChar(&lateral_env_buf, '[');
		plv8_append_rtable_to_lateral_json(&lateral_env_buf, root->parse->rtable,
										   target_rte, 0, &first_rte);

		if (root->parent_root != NULL && root->parent_root->parse != NULL)
		{
			plv8_append_rtable_to_lateral_json(&lateral_env_buf,
											   root->parent_root->parse->rtable,
											   NULL, 1, &first_rte);
		}
		appendStringInfoChar(&lateral_env_buf, ']');

		char *compiled_tsv = NULL;
		char *sql_str = NULL;
		List *lateral_args = NIL;
		Oid lateral_arg_oids[FUNC_MAX_ARGS];
		int nlateral = 0;
		bool pass_call_args = false;

		Plv8SupportHookEntry *hook = plv8_find_or_create_hook_entry(func_name, false);
		if ((hook == NULL || hook->support_js_cb.IsEmpty()) && lang_name[0] != '\0')
			hook = plv8_find_or_create_hook_entry(lang_name, false);

		if ((hook == NULL || hook->support_js_cb.IsEmpty()) && is_custom_lang_srf)
		{
			plv8_in_support_internal_call = true;
			try
			{
				plv8_context *ctx = GetPlv8Context();
				CurrentContextScope context_scope(ctx);
				Isolate::Scope iscope(ctx->isolate);
				HandleScope handle_scope(ctx->isolate);
				(void) Compile(fexpr->funcid, NULL, false, false);
			}
			catch (...)
			{
			}
			plv8_in_support_internal_call = false;

			hook = plv8_find_or_create_hook_entry(func_name, false);
			if ((hook == NULL || hook->support_js_cb.IsEmpty()) && lang_name[0] != '\0')
				hook = plv8_find_or_create_hook_entry(lang_name, false);
		}

		if (hook != NULL && !hook->support_js_cb.IsEmpty())
		{
			plv8_in_support_internal_call = true;
			try
			{
				plv8_context *ctx = GetPlv8Context();
				CurrentContextScope context_scope(ctx);
				Isolate *isolate = ctx->isolate;
				Isolate::Scope iscope(isolate);
				HandleScope handle_scope(isolate);
				Local<Context> context = ctx->localContext();
				Context::Scope cscope(context);
				HandlerExecutionScope hscope(context);
				JSONObject json_helper;

				Local<Function> fn = Local<Function>::New(isolate, hook->support_js_cb);
				Local<Object> req_obj = Object::New(isolate);
				req_obj->Set(context, ToString("request_type"), ToString("simplify")).Check();
				req_obj->Set(context, ToString("func_oid"), Uint32::NewFromUnsigned(isolate, fexpr->funcid)).Check();
				req_obj->Set(context, ToString("func_name"), ToString(func_name)).Check();
				req_obj->Set(context, ToString("lang_name"), ToString(lang_name)).Check();
				req_obj->Set(context, ToString("is_from_clause"), v8::Boolean::New(isolate, true)).Check();
				req_obj->Set(context, ToString("has_coldeflist"),
							 v8::Boolean::New(isolate, target_rtfunc->funccolnames != NIL)).Check();
				req_obj->Set(context, ToString("first_arg"), ToString(first_arg_str ? first_arg_str : "")).Check();
				req_obj->Set(context, ToString("source_arg"), ToString(source_str ? source_str : "")).Check();
				if (params_str != NULL)
					req_obj->Set(context, ToString("params"), json_helper.Parse(ToString(params_str))).Check();
				else
					req_obj->Set(context, ToString("params"), Null(isolate)).Check();
				req_obj->Set(context, ToString("call_args"),
							 json_helper.Parse(ToString(call_args_buf.data))).Check();
				req_obj->Set(context, ToString("expected_columns"),
							 json_helper.Parse(ToString(expected_cols_buf.data))).Check();
				req_obj->Set(context, ToString("lateral_env"),
							 json_helper.Parse(ToString(lateral_env_buf.data))).Check();

				Local<v8::Value> call_args[1] = { req_obj };
				Local<v8::Value> res = DoCall(context, fn, context->Global(), 1, call_args, false);

				if (!res.IsEmpty() && res->IsString())
				{
					CString cs(res);
					compiled_tsv = pstrdup(cs.str());
				}
				else if (!res.IsEmpty() && res->IsObject())
				{
					Local<Object> robj = Local<Object>::Cast(res);
					Local<v8::Value> sql_val;
					if (robj->Get(context, ToString("sql")).ToLocal(&sql_val) && sql_val->IsString())
					{
						CString scs(sql_val);
						sql_str = pstrdup(scs.str());
					}
					Local<v8::Value> pca_val;
					if (robj->Get(context, ToString("pass_call_args")).ToLocal(&pca_val) && pca_val->IsBoolean())
					{
						pass_call_args = pca_val->BooleanValue(isolate);
					}
					Local<v8::Value> cols_val;
					if (robj->Get(context, ToString("columns")).ToLocal(&cols_val) && cols_val->IsArray())
					{
						Plv8ColMeta ccols[FUNC_MAX_ARGS];
						int nccols = plv8_extract_cols_from_v8_array(isolate, context, Local<Array>::Cast(cols_val), ccols);
						if (expected_ncols == 0 && nccols > 0)
						{
							for (int i = 0; i < nccols; i++)
								expected_cols[i] = ccols[i];
							expected_ncols = nccols;
						}
					}
					Local<v8::Value> lvars_val;
					if (robj->Get(context, ToString("lateral_vars")).ToLocal(&lvars_val) && lvars_val->IsArray())
					{
						Local<Array> larr = Local<Array>::Cast(lvars_val);
						uint32_t llen = larr->Length();
						for (uint32_t i = 0; i < llen && nlateral < FUNC_MAX_ARGS; i++)
						{
							Local<v8::Value> lelem;
							if (!larr->Get(context, i).ToLocal(&lelem) || !lelem->IsObject())
								continue;
							Local<Object> lobj = Local<Object>::Cast(lelem);
							int raw_rtindex = 0;
							int varlevelsup = 0;
							AttrNumber attno = 0;
							Oid vtype = InvalidOid;
							int32 vtypmod = -1;
							Oid vcoll = InvalidOid;

							Local<v8::Value> v;
							if (lobj->Get(context, ToString("rtindex")).ToLocal(&v) && v->IsNumber())
								raw_rtindex = v->Int32Value(context).FromMaybe(0);
							if (raw_rtindex < 0)
							{
								raw_rtindex = -raw_rtindex;
								varlevelsup = 1;
							}
							if (lobj->Get(context, ToString("attno")).ToLocal(&v) && v->IsNumber())
								attno = (AttrNumber) v->Int32Value(context).FromMaybe(0);
							if (lobj->Get(context, ToString("type_oid")).ToLocal(&v) && v->IsNumber())
								vtype = (Oid) v->Uint32Value(context).FromMaybe(0);
							if (lobj->Get(context, ToString("typmod")).ToLocal(&v) && v->IsNumber())
								vtypmod = v->Int32Value(context).FromMaybe(-1);
							if (lobj->Get(context, ToString("collation_oid")).ToLocal(&v) && v->IsNumber())
								vcoll = (Oid) v->Uint32Value(context).FromMaybe(0);

							if (raw_rtindex > 0 && attno > 0 && OidIsValid(vtype))
							{
								Var *vnode = makeVar((Index) raw_rtindex, attno, vtype, vtypmod, vcoll, (Index) varlevelsup);
								lateral_args = lappend(lateral_args, vnode);
								lateral_arg_oids[nlateral++] = vtype;
							}
						}
					}
				}
				plv8_in_support_internal_call = false;
			}
			catch (js_error& e)
			{
				plv8_in_support_internal_call = false;
				e.rethrow();
			}
			catch (pg_error& e)
			{
				plv8_in_support_internal_call = false;
				e.rethrow();
			}
		}
		else
		{
			if (is_custom_lang_srf)
				PG_RETURN_POINTER(NULL);

			Oid compile_oid = plv8_find_support_sql_func_oid(func_name);
			if (!OidIsValid(compile_oid))
				PG_RETURN_POINTER(NULL);

			plv8_in_support_internal_call = true;
			PG_TRY();
			{
				Datum compiled_datum = OidFunctionCall5(
					compile_oid,
					CStringGetTextDatum(first_arg_str ? first_arg_str : ""),
					CStringGetTextDatum(source_str ? source_str : ""),
					CStringGetTextDatum(expected_cols_buf.data),
					CStringGetTextDatum(lateral_env_buf.data),
					CStringGetTextDatum(params_str ? params_str : ""));
				compiled_tsv = TextDatumGetCString(compiled_datum);
			}
			PG_FINALLY();
			{
				plv8_in_support_internal_call = false;
			}
			PG_END_TRY();
		}

		if (compiled_tsv != NULL)
		{
			char *copy = pstrdup(compiled_tsv);
			char *saveptr = NULL;
			int line_idx = 0;
			Plv8ColMeta compiled_cols[FUNC_MAX_ARGS];
			int ncompiled_cols = 0;

			for (char *line = strtok_r(copy, "\n", &saveptr);
				 line != NULL;
				 line = strtok_r(NULL, "\n", &saveptr))
			{
				CHECK_FOR_INTERRUPTS();
				if (line_idx == 0 && strncmp(line, "SQL\t", 4) == 0)
				{
					sql_str = pstrdup(line + 4);
					line_idx++;
					continue;
				}
				if (strncmp(line, "COL\t", 4) == 0 && ncompiled_cols < FUNC_MAX_ARGS)
				{
					char *p1 = line + 4;
					char *t1 = strchr(p1, '\t');
					if (t1 != NULL)
					{
						*t1 = '\0';
						char *t2 = strchr(t1 + 1, '\t');
						if (t2 != NULL)
						{
							*t2 = '\0';
							char *t3 = strchr(t2 + 1, '\t');
							if (t3 != NULL)
								*t3 = '\0';
							compiled_cols[ncompiled_cols].name = pstrdup(p1);
							compiled_cols[ncompiled_cols].type_oid = (Oid) atooid(t1 + 1);
							compiled_cols[ncompiled_cols].typmod = atoi(t2 + 1);
							compiled_cols[ncompiled_cols].collation_oid =
								t3 ? (Oid) atooid(t3 + 1) : InvalidOid;
							ncompiled_cols++;
						}
					}
				}
				else if (strncmp(line, "LAT\t", 4) == 0 && nlateral < FUNC_MAX_ARGS)
				{
					char *p1 = line + 4;
					char *t1 = strchr(p1, '\t');
					if (t1 != NULL)
					{
						*t1 = '\0';
						char *t2 = strchr(t1 + 1, '\t');
						if (t2 != NULL)
						{
							*t2 = '\0';
							char *t3 = strchr(t2 + 1, '\t');
							if (t3 != NULL)
							{
								*t3 = '\0';
								char *t4 = strchr(t3 + 1, '\t');
								if (t4 != NULL)
									*t4 = '\0';

								int raw_rtindex = atoi(p1);
								int varlevelsup = 0;
								if (raw_rtindex < 0)
								{
									raw_rtindex = -raw_rtindex;
									varlevelsup = 1;
								}
								AttrNumber attno = (AttrNumber) atoi(t1 + 1);
								Oid vtype = (Oid) atooid(t2 + 1);
								int32 vtypmod = atoi(t3 + 1);
								Oid vcoll = t4 ? (Oid) atooid(t4 + 1) : InvalidOid;

								Var *vnode = makeVar((Index) raw_rtindex,
													 attno,
													 vtype,
													 vtypmod,
													 vcoll,
													 (Index) varlevelsup);
								lateral_args = lappend(lateral_args, vnode);
								lateral_arg_oids[nlateral++] = vtype;
							}
						}
					}
				}
				line_idx++;
			}

			if (expected_ncols == 0 && ncompiled_cols > 0)
			{
				for (int i = 0; i < ncompiled_cols; i++)
					expected_cols[i] = compiled_cols[i];
				expected_ncols = ncompiled_cols;
			}
		}

		if (sql_str == NULL || *sql_str == '\0')
			PG_RETURN_POINTER(NULL);

		/*
		 * Now that inlining is succeeding, reset `func_name`'s `pg_proc`
		 * `proallargtypes` back to `RECORDOID` if it was a dynamic table
		 * macro so subsequent `AS (...)` calls see a clean `TYPEFUNC_RECORD`.
		 */
		if (!is_custom_lang_srf)
			plv8_update_func_overloads_columns(func_name, 0, NULL);

		List *helper_args = NIL;
		Oid helper_arg_oids[FUNC_MAX_ARGS];
		int nhelper_args = 0;

		if (pass_call_args && fexpr->args != NIL)
		{
			ListCell *alc;
			foreach(alc, fexpr->args)
			{
				if (nhelper_args >= FUNC_MAX_ARGS)
					break;
				Node *anode = (Node *) copyObjectImpl(lfirst(alc));
				helper_args = lappend(helper_args, anode);
				helper_arg_oids[nhelper_args++] = exprType(anode);
			}
		}
		if (lateral_args != NIL)
		{
			ListCell *llc;
			int lidx = 0;
			foreach(llc, lateral_args)
			{
				if (nhelper_args >= FUNC_MAX_ARGS)
					break;
				helper_args = lappend(helper_args, lfirst(llc));
				helper_arg_oids[nhelper_args++] = lateral_arg_oids[lidx++];
			}
		}

		bool uses_table_out_cols =
			(target_rtfunc->funccolnames == NIL &&
			 (fexpr->funcresulttype == RECORDOID ||
			  plv8_is_single_col_holder_type(fexpr->funcresulttype)));
		int helper_ncols = uses_table_out_cols ? expected_ncols : 0;
		Oid helper_target_rettype = fexpr->funcresulttype;

		uint64 slot_hash = plv8_fnv1a64(14695981039346656037ULL, sql_str, strlen(sql_str));
		slot_hash = plv8_fnv1a64(slot_hash, &nhelper_args, sizeof(nhelper_args));
		if (nhelper_args > 0)
			slot_hash = plv8_fnv1a64(slot_hash, helper_arg_oids, sizeof(Oid) * nhelper_args);
		slot_hash = plv8_fnv1a64(slot_hash, &helper_target_rettype, sizeof(helper_target_rettype));
		slot_hash = plv8_fnv1a64(slot_hash, &helper_ncols, sizeof(helper_ncols));
		for (int i = 0; i < helper_ncols; i++)
		{
			const char *cn = expected_cols[i].name ? expected_cols[i].name : "";
			slot_hash = plv8_fnv1a64(slot_hash, cn, strlen(cn) + 1);
			slot_hash = plv8_fnv1a64(slot_hash, &expected_cols[i].type_oid, sizeof(Oid));
		}

		int base_slot = (int) (slot_hash % PLV8_NUM_INLINE_HELPERS);
		Oid helper_oid = InvalidOid;
		for (int probe = 0; probe < PLV8_NUM_INLINE_HELPERS; probe++)
		{
			int slot_idx = (base_slot + probe) % PLV8_NUM_INLINE_HELPERS;
			char helper_name[64];
			snprintf(helper_name, sizeof(helper_name), "plv8_inline_helper_%d", slot_idx);
			Oid cand_oid = plv8_lookup_func_oid_by_name_nargs(helper_name, -1);
			if (!OidIsValid(cand_oid))
			{
				snprintf(helper_name, sizeof(helper_name), "plpgq_inline_helper_%d", slot_idx);
				cand_oid = plv8_lookup_func_oid_by_name_nargs(helper_name, -1);
			}
			if (!OidIsValid(cand_oid))
				continue;

			bool in_use = false;
			if (root->parse != NULL)
			{
				ListCell *rlc;
				foreach(rlc, root->parse->rtable)
				{
					RangeTblEntry *rte = (RangeTblEntry *) lfirst(rlc);
					if (rte == target_rte || rte->rtekind != RTE_FUNCTION)
						continue;
					ListCell *flc;
					foreach(flc, rte->functions)
					{
						RangeTblFunction *rtf = (RangeTblFunction *) lfirst(flc);
						if (rtf->funcexpr && IsA(rtf->funcexpr, FuncExpr) &&
							((FuncExpr *) rtf->funcexpr)->funcid == cand_oid)
						{
							in_use = true;
							break;
						}
					}
					if (in_use)
						break;
				}
			}
			if (!in_use)
			{
				helper_oid = cand_oid;
				break;
			}
			if (!OidIsValid(helper_oid))
				helper_oid = cand_oid;
		}
		if (!OidIsValid(helper_oid))
			PG_RETURN_POINTER(NULL);

		plv8_update_proc_table_columns(
			helper_oid,
			helper_ncols,
			expected_cols,
			sql_str,
			nhelper_args,
			helper_arg_oids,
			helper_target_rettype);

		if (nlateral > 0)
			target_rte->lateral = true;

		FuncExpr *new_fexpr = makeFuncExpr(helper_oid,
										   get_func_rettype(helper_oid),
										   helper_args,
										   fexpr->funccollid,
										   fexpr->inputcollid,
										   COERCE_EXPLICIT_CALL);
		new_fexpr->funcretset = true;

		PG_RETURN_POINTER(new_fexpr);
	}

	PG_RETURN_POINTER(NULL);
}

Datum
plv8_base_type_in(PG_FUNCTION_ARGS)
{
	char *s = PG_GETARG_CSTRING(0);
	if (s == nullptr)
		PG_RETURN_NULL();

	Oid funcoid = fcinfo->flinfo->fn_oid;
	HeapTuple proc_tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(proc_tp))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proc_tp);
	Oid pronamespace = proc->pronamespace;
	char *proname = pstrdup(NameStr(proc->proname));
	Oid typoid = proc->prorettype;
	ReleaseSysCache(proc_tp);

	Oid argtypes[1] = { TEXTOID };
	List *qualname = list_make2(makeString(get_namespace_name(pronamespace)),
								makeString(proname));
	Oid in_fn_oid = LookupFuncName(qualname, 1, argtypes, false);
	pfree(proname);

	HeapTuple tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typoid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for type %u", typoid);
	Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
	int typeLen = typtup->typlen;
	char *typeName = pstrdup(NameStr(typtup->typname));
	ReleaseSysCache(tp);

	Datum datum = OidFunctionCall1Coll(in_fn_oid, InvalidOid, CStringGetTextDatum(s));
	bytea *result = DatumGetByteaPP(datum);
	if (typeLen >= 0)
	{
		int inputLen = VARSIZE_ANY_EXHDR(result) + VARHDRSZ;
		if (typeLen != inputLen)
			elog(ERROR, "type %s is defined as fixed-size %d, but actual data length is %d",
				 typeName, typeLen, inputLen);
	}
	pfree(typeName);
	PG_RETURN_POINTER(result);
}

Datum
plv8_base_type_out(PG_FUNCTION_ARGS)
{
	Datum datum = PG_GETARG_DATUM(0);
	Oid funcoid = fcinfo->flinfo->fn_oid;
	HeapTuple proc_tp = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcoid));
	if (!HeapTupleIsValid(proc_tp))
		elog(ERROR, "cache lookup failed for function %u", funcoid);
	Form_pg_proc proc = (Form_pg_proc) GETSTRUCT(proc_tp);
	Oid pronamespace = proc->pronamespace;
	char *proname = pstrdup(NameStr(proc->proname));
	ReleaseSysCache(proc_tp);

	Oid out_argtypes[1] = { BYTEAOID };
	List *qualname = list_make2(makeString(get_namespace_name(pronamespace)),
								makeString(proname));
	Oid out_fn_oid = LookupFuncName(qualname, 1, out_argtypes, false);
	pfree(proname);

	Datum result = OidFunctionCall1Coll(out_fn_oid, InvalidOid, datum);
	PG_RETURN_CSTRING(TextDatumGetCString(result));
}



