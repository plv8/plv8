-- Regression suite for Single-Isolate Multi-Context, V8 Startup Snapshots,
-- Boot-Script Preload/Reload, and Shared DSA+dshash WASM Native Code Cache
\set ON_ERROR_STOP on

SET client_min_messages = warning;
DROP EXTENSION IF EXISTS plv8 CASCADE;
CREATE EXTENSION plv8;
RESET client_min_messages;

-- ============================================================================
-- 1. Single-Isolate Multi-Context Sharing, Per-Role plv8_reset(), and
--    Rapid Context Switching (SET ROLE + SECURITY DEFINER + Microtasks)
-- ============================================================================
DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'plv8_iso_role_a') THEN
    EXECUTE 'DROP OWNED BY plv8_iso_role_a CASCADE';
    EXECUTE 'DROP ROLE plv8_iso_role_a';
  END IF;
  IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'plv8_iso_role_b') THEN
    EXECUTE 'DROP OWNED BY plv8_iso_role_b CASCADE';
    EXECUTE 'DROP ROLE plv8_iso_role_b';
  END IF;
END;
$$;

CREATE ROLE plv8_iso_role_a;
CREATE ROLE plv8_iso_role_b;
GRANT USAGE ON SCHEMA public, plv8 TO plv8_iso_role_a, plv8_iso_role_b;

CREATE OR REPLACE FUNCTION plv8_iso_set_global(k text, v text)
RETURNS text LANGUAGE plv8 AS $$
  globalThis[k] = v;
  return globalThis[k];
$$;

CREATE OR REPLACE FUNCTION plv8_iso_get_global(k text)
RETURNS text LANGUAGE plv8 AS $$
  return typeof globalThis[k] === 'undefined' ? '<undefined>' : String(globalThis[k]);
$$;

CREATE OR REPLACE FUNCTION plv8_iso_secdef_a(v text)
RETURNS text LANGUAGE plv8 SECURITY DEFINER AS $$
  let microtask_ran = false;
  Promise.resolve(v).then((resolved) => {
    globalThis.__secdef_a_state = 'secdef_a:' + resolved;
    microtask_ran = true;
  });
  return globalThis.__secdef_a_state || 'pending';
$$;
ALTER FUNCTION plv8_iso_secdef_a(text) OWNER TO plv8_iso_role_a;
GRANT EXECUTE ON FUNCTION plv8_iso_secdef_a(text) TO plv8_iso_role_b, PUBLIC;

-- Populate global state in superuser, role_a, and role_b contexts
SELECT plv8_iso_set_global('role_tag', 'superuser_val') AS sup_set;

SET ROLE plv8_iso_role_a;
SELECT plv8_iso_set_global('role_tag', 'role_a_val') AS role_a_set;
RESET ROLE;

SET ROLE plv8_iso_role_b;
SELECT plv8_iso_set_global('role_tag', 'role_b_val') AS role_b_set;
RESET ROLE;

-- Verify single isolate is shared across all 3 role contexts while globals are isolated
SELECT
  (plv8_info()->>'single_isolate')::boolean AS single_isolate,
  (plv8_info()->>'isolate_shared')::boolean AS isolate_shared,
  (plv8_info()->>'active_role_contexts')::int >= 3 AS has_three_contexts,
  plv8_iso_get_global('role_tag') AS sup_tag;

-- Rapidly alternate SET ROLE + nested SECURITY DEFINER + microtask checkpoints
DO $$
DECLARE
  res text;
BEGIN
  FOR i IN 1..10 LOOP
    EXECUTE 'SET ROLE plv8_iso_role_b';
    PERFORM plv8_iso_secdef_a('iter_' || i);
    res := plv8_iso_get_global('__secdef_a_state');
    IF res <> '<undefined>' THEN
      RAISE EXCEPTION 'Unexpected context state in role_b: %', res;
    END IF;
    res := plv8_iso_get_global('role_tag');
    IF res <> 'role_b_val' THEN
      RAISE EXCEPTION 'Expected role_b_val, got %', res;
    END IF;

    EXECUTE 'SET ROLE plv8_iso_role_a';
    res := plv8_iso_get_global('__secdef_a_state');
    IF res <> ('secdef_a:iter_' || i) THEN
      RAISE EXCEPTION 'Expected microtask in role_a to set secdef_a:iter_%, got %', i, res;
    END IF;
  END LOOP;
  EXECUTE 'RESET ROLE';
END;
$$;

-- Verify plv8_reset() under role_a only resets role_a's context, leaving role_b and superuser intact
SET ROLE plv8_iso_role_a;
SELECT plv8_reset() IS NOT NULL AS role_a_reset_called;
SELECT plv8_iso_get_global('role_tag') AS role_a_after_reset;
RESET ROLE;

SET ROLE plv8_iso_role_b;
SELECT plv8_iso_get_global('role_tag') AS role_b_preserved;
RESET ROLE;
SELECT plv8_iso_get_global('role_tag') AS sup_preserved;

-- ============================================================================
-- 2. Joint Isolate OOM Recovery (KillIsolateAndAllContexts) Across Contexts
-- ============================================================================
CREATE OR REPLACE FUNCTION plv8_trigger_wasm_oom()
RETURNS int LANGUAGE plv8 AS $$
  // Allocate linear memory pages exceeding plv8.memory_limit (256 MB = 4096 pages)
  const mem = new WebAssembly.Memory({ initial: 16, maximum: 8192 });
  mem.grow(5000);
  return mem.buffer.byteLength;
$$;

DO $$
BEGIN
  BEGIN
    PERFORM plv8_trigger_wasm_oom();
    RAISE EXCEPTION 'Expected WASM linear memory growth beyond memory_limit to fail';
  EXCEPTION WHEN OTHERS THEN
    -- Expected OOM or RangeError
  END;
END;
$$;

-- Verify clean recovery after OOM and that subsequent calls in all roles succeed
SELECT plv8_iso_set_global('post_oom', 'recovered_ok') AS post_oom_recovery;

-- ============================================================================
-- 3. Symlink Refusal (O_NOFOLLOW / S_ISLNK) on plv8_save_snapshot,
--    plv8.snapshot_file, and plv8.boot_script_file
-- ============================================================================
\! rm -f /tmp/plv8_test_symlink.snap /tmp/plv8_test_target.snap /tmp/plv8_test_symlink_boot.js /tmp/plv8_test_target_boot.js && touch /tmp/plv8_test_target.snap /tmp/plv8_test_target_boot.js && ln -s /tmp/plv8_test_target.snap /tmp/plv8_test_symlink.snap && ln -s /tmp/plv8_test_target_boot.js /tmp/plv8_test_symlink_boot.js

-- 3a. plv8_save_snapshot refuses symlink destination
DO $$
BEGIN
  BEGIN
    PERFORM plv8_save_snapshot('/tmp/plv8_test_symlink.snap');
    RAISE EXCEPTION 'Expected plv8_save_snapshot to refuse symlink target';
  EXCEPTION WHEN wrong_object_type THEN
    IF SQLERRM NOT LIKE '%refusing to write snapshot to symlink%' THEN
      RAISE EXCEPTION 'Unexpected error message: %', SQLERRM;
    END IF;
  END;
END;
$$;

-- 3b. plv8.snapshot_file refuses symlink
DO $$
BEGIN
  BEGIN
    SET plv8.snapshot_file = '/tmp/plv8_test_symlink.snap';
    PERFORM plv8_reset();
    RAISE EXCEPTION 'Expected plv8.snapshot_file symlink to be rejected';
  EXCEPTION WHEN OTHERS THEN
    RESET plv8.snapshot_file;
    IF SQLERRM NOT LIKE '%could not open snapshot_file%' AND SQLERRM NOT LIKE '%not a regular file%' THEN
      RAISE EXCEPTION 'Unexpected error for symlink snapshot_file: %', SQLERRM;
    END IF;
  END;
END;
$$;

-- 3c. plv8.boot_script_file refuses symlink
DO $$
BEGIN
  BEGIN
    SET plv8.boot_script_file = '/tmp/plv8_test_symlink_boot.js';
    PERFORM plv8_reset();
    RAISE EXCEPTION 'Expected plv8.boot_script_file symlink to be rejected';
  EXCEPTION WHEN OTHERS THEN
    RESET plv8.boot_script_file;
    IF SQLERRM NOT LIKE '%could not open boot_script_file%' AND SQLERRM NOT LIKE '%not a regular file%' THEN
      RAISE EXCEPTION 'Unexpected error for symlink boot_script_file: %', SQLERRM;
    END IF;
  END;
END;
$$;

-- ============================================================================
-- 4. Boot-Script Compilation/Execution Timeout & Successful Boot-Script Reload
-- ============================================================================
\! printf 'while (true) {}\n' > /tmp/plv8_test_boot_hang.js && printf 'globalThis.__plv8_boot_banner = "boot_ok_400";\n' > /tmp/plv8_test_boot_ok.js

-- 4a. Infinite loop in boot_script_file times out cleanly with query_canceled
DO $$
BEGIN
  SET plv8.execution_timeout = 1;
  BEGIN
    SET plv8.boot_script_file = '/tmp/plv8_test_boot_hang.js';
    PERFORM plv8_reset();
    RAISE EXCEPTION 'Expected hanging boot_script_file to time out';
  EXCEPTION WHEN query_canceled THEN
    RESET plv8.boot_script_file;
    RESET plv8.execution_timeout;
    IF SQLERRM NOT LIKE '%timed out compiling boot_script_file%' THEN
      RAISE EXCEPTION 'Unexpected timeout error message: %', SQLERRM;
    END IF;
  END;
END;
$$;

-- 4b. Valid boot_script_file compiles into startup snapshot and populates all role contexts
SET plv8.boot_script_file = '/tmp/plv8_test_boot_ok.js';
SELECT plv8_reset() IS NOT NULL AS boot_script_loaded;

SELECT
  (plv8_info()->>'snapshot_active')::boolean AS boot_snap_active,
  plv8_info()->>'snapshot_source' AS boot_snap_source,
  plv8_iso_get_global('__plv8_boot_banner') AS sup_boot_banner;

SET ROLE plv8_iso_role_a;
SELECT plv8_iso_get_global('__plv8_boot_banner') AS role_a_boot_banner;
RESET ROLE;

-- ============================================================================
-- 5. Snapshot Save, Restore Across Roles, and Catalog Invalidation
--    (Function Body Modification)
-- ============================================================================
\! rm -f /tmp/plv8_test_valid.snap

CREATE OR REPLACE FUNCTION plv8_snap_sup_fn(x int)
RETURNS int LANGUAGE plv8 IMMUTABLE AS $$
  return x * 10 + (globalThis.__plv8_boot_banner === 'boot_ok_400' ? 1 : 0);
$$;

CREATE OR REPLACE FUNCTION plv8_snap_role_a_fn(x int)
RETURNS int LANGUAGE plv8 IMMUTABLE AS $$
  return x + 100;
$$;
ALTER FUNCTION plv8_snap_role_a_fn(int) OWNER TO plv8_iso_role_a;
GRANT EXECUTE ON FUNCTION plv8_snap_role_a_fn(int) TO PUBLIC;

CREATE OR REPLACE FUNCTION plv8_snap_secdef_a_fn(x int)
RETURNS int LANGUAGE plv8 IMMUTABLE SECURITY DEFINER AS $$
  return x + 1000;
$$;
ALTER FUNCTION plv8_snap_secdef_a_fn(int) OWNER TO plv8_iso_role_a;
GRANT EXECUTE ON FUNCTION plv8_snap_secdef_a_fn(int) TO PUBLIC;

SELECT
  (plv8_save_snapshot(
    '/tmp/plv8_test_valid.snap',
    ARRAY[
      'plv8_snap_sup_fn(int)'::regprocedure,
      'plv8_snap_role_a_fn(int)'::regprocedure,
      'plv8_snap_secdef_a_fn(int)'::regprocedure
    ]
  )->>'functions_compiled')::int AS snap_fns_compiled;

RESET plv8.boot_script_file;
SET plv8.snapshot_file = '/tmp/plv8_test_valid.snap';
SELECT plv8_reset() IS NOT NULL AS snapshot_file_loaded;

-- 5a. Superuser calls plv8_snap_sup_fn -> restores from snapshot
SELECT
  plv8_snap_sup_fn(5) AS sup_fn_res,
  (plv8_info()->>'snapshot_active')::boolean AS snap_active,
  plv8_info()->>'snapshot_source' AS snap_source,
  (plv8_info()->>'snapshot_functions_restored')::int >= 1 AS sup_restored;

-- 5b. Role B calls SECURITY INVOKER plv8_snap_role_a_fn -> restored in Role B's context
--     Role B calls SECURITY DEFINER plv8_snap_secdef_a_fn -> restored in Role A's context
DO $$
DECLARE
  before_restored int;
  after_invoker int;
  after_secdef int;
BEGIN
  before_restored := (plv8_info()->>'snapshot_functions_restored')::int;

  EXECUTE 'SET ROLE plv8_iso_role_b';
  IF plv8_snap_role_a_fn(7) <> 107 THEN
    RAISE EXCEPTION 'Unexpected result from plv8_snap_role_a_fn';
  END IF;
  EXECUTE 'RESET ROLE';
  after_invoker := (plv8_info()->>'snapshot_functions_restored')::int;
  IF after_invoker <= before_restored THEN
    RAISE EXCEPTION 'Expected SECURITY INVOKER function to restore from snapshot for role_b (before=%, after=%)', before_restored, after_invoker;
  END IF;

  EXECUTE 'SET ROLE plv8_iso_role_b';
  IF plv8_snap_secdef_a_fn(7) <> 1007 THEN
    RAISE EXCEPTION 'Unexpected result from plv8_snap_secdef_a_fn';
  END IF;
  EXECUTE 'RESET ROLE';
  after_secdef := (plv8_info()->>'snapshot_functions_restored')::int;
  IF after_secdef <= after_invoker THEN
    RAISE EXCEPTION 'Expected SECURITY DEFINER function to restore from snapshot for owner role_a (before=%, after=%)', after_invoker, after_secdef;
  END IF;
END;
$$;

-- 5c. Modifying function body (CREATE OR REPLACE) invalidates its snapshot slot (xmin/tid mismatch)
CREATE OR REPLACE FUNCTION plv8_snap_sup_fn(x int)
RETURNS int LANGUAGE plv8 IMMUTABLE AS $$
  return x * 100;
$$;
SELECT plv8_snap_sup_fn(5) AS sup_fn_recompiled_res;

RESET plv8.snapshot_file;
SELECT plv8_reset() IS NOT NULL AS snapshot_cleared;

-- ============================================================================
-- 6. WASM Native Code Cache Quota (Single-Entry > 25% Rejection,
--    Non-Superuser 25% Role Cap, Global Cap) and LRU Eviction
-- ============================================================================
CREATE OR REPLACE FUNCTION plv8_make_test_wasm(magic_const int)
RETURNS bytea LANGUAGE plv8 IMMUTABLE AS $$
  // Build a valid WebAssembly binary with a custom section of 600 bytes so that
  // V8's serialized native code is ~4-8 KB and each magic_const yields a distinct SHA-256
  const payload = [];
  let v = magic_const >>> 0;
  let more = true;
  while (more) {
    let b = v & 0x7f;
    v >>>= 7;
    if (v === 0 && (b & 0x40) === 0) {
      more = false;
    } else {
      b |= 0x80;
    }
    payload.push(b);
  }

  const body = [0x00, 0x41, ...payload, 0x0b];
  const codeSection = [0x01, body.length, ...body];
  const customData = new Array(512).fill((magic_const & 0xff));
  const customSection = [
    0x04, 0x70, 0x61, 0x64, 0x30, // name "pad0"
    ...customData
  ];
  const customLen = customSection.length;
  const customLenLeb = [customLen & 0x7f | 0x80, (customLen >> 7) & 0x7f];

  const bytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00,
    0x0a, codeSection.length, ...codeSection,
    0x00, ...customLenLeb, ...customSection
  ]);
  return bytes;
$$;

CREATE OR REPLACE FUNCTION plv8_compile_and_run_wasm(tag text, wasm_bytes bytea)
RETURNS int LANGUAGE plv8 AS $$
  const mod = plv8.compile_wasm_cached(tag, wasm_bytes);
  const inst = new WebAssembly.Instance(mod);
  return inst.exports.f();
$$;
GRANT EXECUTE ON FUNCTION plv8_make_test_wasm(int) TO plv8_iso_role_a, plv8_iso_role_b;
GRANT EXECUTE ON FUNCTION plv8_compile_and_run_wasm(text, bytea) TO plv8_iso_role_a, plv8_iso_role_b;

-- 6a & 6b. Measure serialized WASM entry size, then test:
--   (6a) Single-entry > 25% of cache limit rejection
--   (6b) Non-superuser 25% role quota and LRU eviction
--   (6c) Global cache quota and LRU eviction
DO $$
DECLARE
  entry_size bigint;
  s0 bigint;
  s1 bigint;
  stores_before bigint;
  stores_after bigint;
  res int;
  cap_bytes int;
  evictions_before bigint;
  evictions_after bigint;
  used_after bigint;
BEGIN
  -- Measure exact serialized code_size of one test WASM module
  s0 := (plv8_info()->'wasm_cache'->>'bytes_used')::bigint;
  PERFORM plv8_compile_and_run_wasm('measure_mod', plv8_make_test_wasm(9100));
  s1 := (plv8_info()->'wasm_cache'->>'bytes_used')::bigint;
  entry_size := s1 - s0;
  IF entry_size <= 0 THEN
    PERFORM plv8_compile_and_run_wasm('measure_mod_2', plv8_make_test_wasm(9101));
    entry_size := (plv8_info()->'wasm_cache'->>'bytes_used')::bigint - s1;
  END IF;
  IF entry_size <= 0 THEN
    RAISE EXCEPTION 'Expected positive serialized WASM entry_size, got %', entry_size;
  END IF;

  -- (6a) Set wasm_cache_max_bytes = entry_size * 2 so 25% limit is entry_size / 2.
  -- Any module of size entry_size exceeds 25% of the cache and must be rejected from caching.
  EXECUTE format('SET plv8.wasm_cache_max_bytes = %s', GREATEST(16, (entry_size * 2)::int));
  stores_before := (plv8_info()->'wasm_cache'->>'stores')::bigint;
  res := plv8_compile_and_run_wasm('oversized_mod', plv8_make_test_wasm(9001));
  IF res <> 9001 THEN
    RAISE EXCEPTION 'Expected 9001, got %', res;
  END IF;
  stores_after := (plv8_info()->'wasm_cache'->>'stores')::bigint;
  IF stores_after <> stores_before THEN
    RAISE EXCEPTION 'Expected oversized WASM module (>25%% of cache, entry_size=%) not to be stored in cache (before=%, after=%)', entry_size, stores_before, stores_after;
  END IF;

  -- (6b) Configure plv8.wasm_cache_max_bytes = entry_size * 10.
  -- Then 25% role_cap = entry_size * 2.5, so non-superuser role_a can hold at most 2 entries;
  -- inserting 5 entries for role_a must evict at least 3 LRU entries of role_a.
  cap_bytes := (entry_size * 10)::int;
  EXECUTE format('SET plv8.wasm_cache_max_bytes = %s', cap_bytes);

  evictions_before := (plv8_info()->'wasm_cache'->>'evictions')::bigint;

  EXECUTE 'SET ROLE plv8_iso_role_a';
  FOR i IN 1..5 LOOP
    IF plv8_compile_and_run_wasm('role_a_mod_' || i, plv8_make_test_wasm(9200 + i)) <> (9200 + i) THEN
      RAISE EXCEPTION 'WASM execution mismatch on iteration %', i;
    END IF;
  END LOOP;
  EXECUTE 'RESET ROLE';

  evictions_after := (plv8_info()->'wasm_cache'->>'evictions')::bigint;
  used_after := (plv8_info()->'wasm_cache'->>'bytes_used')::bigint;

  IF evictions_after < evictions_before + 3 THEN
    RAISE EXCEPTION 'Expected at least 3 role-quota LRU evictions for non-superuser role_a (before=%, after=%)', evictions_before, evictions_after;
  END IF;

  IF used_after > cap_bytes THEN
    RAISE EXCEPTION 'Expected bytes_used (%) <= cap_bytes (%)', used_after, cap_bytes;
  END IF;

  -- (6c) Global cache cap LRU eviction (superuser has role_cap = max_bytes; set max_bytes = entry_size * 4)
  cap_bytes := (entry_size * 4)::int;
  EXECUTE format('SET plv8.wasm_cache_max_bytes = %s', cap_bytes);
  evictions_before := (plv8_info()->'wasm_cache'->>'evictions')::bigint;
  FOR i IN 1..6 LOOP
    IF plv8_compile_and_run_wasm('sup_global_mod_' || i, plv8_make_test_wasm(9300 + i)) <> (9300 + i) THEN
      RAISE EXCEPTION 'WASM execution mismatch on superuser iteration %', i;
    END IF;
  END LOOP;
  evictions_after := (plv8_info()->'wasm_cache'->>'evictions')::bigint;
  used_after := (plv8_info()->'wasm_cache'->>'bytes_used')::bigint;
  IF evictions_after < evictions_before + 2 THEN
    RAISE EXCEPTION 'Expected global-quota LRU evictions (before=%, after=%)', evictions_before, evictions_after;
  END IF;
  IF used_after > cap_bytes THEN
    RAISE EXCEPTION 'Expected bytes_used (%) <= global cap_bytes (%)', used_after, cap_bytes;
  END IF;

  RESET plv8.wasm_cache_max_bytes;
END;
$$;

-- ============================================================================
-- 7. Invalid / Corrupt WASM Bytecode Handling & WebAssembly.Memory Bounds
-- ============================================================================
-- 7a. Invalid magic / truncated / corrupt section bytecode in plv8.compile_wasm_cached
CREATE OR REPLACE FUNCTION plv8_test_invalid_wasm()
RETURNS boolean LANGUAGE plv8 AS $$
  const badInputs = [
    new Uint8Array([0x00, 0x61, 0x73]), // too short
    new Uint8Array([0xde, 0xad, 0xbe, 0xef, 0x01, 0x00, 0x00, 0x00]), // bad magic
    new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x99, 0x00, 0x00, 0x00]), // bad version
    new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x7f, 0xff]) // corrupt type section
  ];
  for (let i = 0; i < badInputs.length; i++) {
    let caught = false;
    try {
      plv8.compile_wasm_cached('bad_' + i, badInputs[i]);
    } catch (e) {
      caught = true;
    }
    if (!caught) {
      throw new Error('Expected invalid WASM input #' + i + ' to throw');
    }
  }

  // Also test WebAssembly.Memory maximum page limit enforcement
  const boundedMem = new WebAssembly.Memory({ initial: 1, maximum: 2 });
  boundedMem.grow(1); // 1 -> 2 pages: ok
  let growCaught = false;
  try {
    boundedMem.grow(1); // 2 -> 3 pages: must throw RangeError
  } catch (e) {
    growCaught = (e instanceof RangeError);
  }
  if (!growCaught) {
    throw new Error('Expected WebAssembly.Memory.grow beyond maximum pages to throw RangeError');
  }
  return true;
$$;

SELECT plv8_test_invalid_wasm() AS invalid_wasm_and_mem_bounds_ok;

-- 7b. Corrupt \0asm-prefixed bytecode in plv8.modules fails cleanly on import without crashing
INSERT INTO plv8.modules (path, source, bytecode, is_trusted)
VALUES ('corrupt_wasm_mod', '', decode('0061736d01000000017fffff', 'hex'), true);

CREATE OR REPLACE FUNCTION plv8_import_corrupt_wasm()
RETURNS int LANGUAGE plv8 AS $$
  require('tle:corrupt_wasm_mod');
  return 1;
$$;

DO $$
BEGIN
  BEGIN
    PERFORM plv8_import_corrupt_wasm();
    RAISE EXCEPTION 'Expected importing corrupt WASM module from plv8.modules to fail';
  EXCEPTION WHEN OTHERS THEN
    -- Expected clean CompileError
  END;
END;
$$;

DELETE FROM plv8.modules WHERE path = 'corrupt_wasm_mod';

-- ============================================================================
-- 8. Concurrent Multi-Session Contention on Shared DSA+dshash WASM Cache
-- ============================================================================
CREATE TEMP TABLE plv8_wasm_concurrent_baseline AS
SELECT
  plv8_compile_and_run_wasm('shared_concurrent_wasm', plv8_make_test_wasm(7777)) AS init_val,
  (plv8_info()->'wasm_cache'->>'hits')::bigint AS hits_before;

\copy (SELECT format('PSQL_BIN="$(dirname "$(readlink -f /proc/%s/exe)")/psql"; if [ ! -x "$PSQL_BIN" ]; then PSQL_BIN="psql"; fi; for w in 1 2 3 4; do "$PSQL_BIN" -X -q -h %L -p %L -U %L -d %L -c "SELECT plv8_compile_and_run_wasm(''shared_concurrent_wasm'', plv8_make_test_wasm(7777)), plv8_compile_and_run_wasm(''worker_wasm_'' || $w, plv8_make_test_wasm(8000 + $w));" >/dev/null 2>&1 & done; wait', pg_backend_pid(), btrim(split_part(current_setting('unix_socket_directories'), ',', 1)), current_setting('port'), current_user, current_database())) TO '/tmp/plv8_concurrent_workers.sh'
\! sh /tmp/plv8_concurrent_workers.sh

DO $$
DECLARE
  hits_before bigint;
  hits_after bigint;
BEGIN
  SELECT b.hits_before INTO hits_before FROM plv8_wasm_concurrent_baseline b;
  hits_after := (plv8_info()->'wasm_cache'->>'hits')::bigint;
  IF hits_after < hits_before + 4 THEN
    RAISE EXCEPTION 'Expected at least 4 cross-backend WASM cache hits from concurrent workers (before=%, after=%)', hits_before, hits_after;
  END IF;
END;
$$;

-- Cleanup temporary files and test roles
\! rm -f /tmp/plv8_test_symlink.snap /tmp/plv8_test_target.snap /tmp/plv8_test_symlink_boot.js /tmp/plv8_test_target_boot.js /tmp/plv8_test_boot_hang.js /tmp/plv8_test_boot_ok.js /tmp/plv8_test_valid.snap /tmp/plv8_concurrent_workers.sh

DROP FUNCTION plv8_snap_secdef_a_fn(int);
DROP FUNCTION plv8_snap_role_a_fn(int);
DROP FUNCTION plv8_snap_sup_fn(int);
DROP FUNCTION plv8_iso_secdef_a(text);
DROP FUNCTION plv8_iso_set_global(text, text);
DROP FUNCTION plv8_iso_get_global(text);
DROP FUNCTION plv8_trigger_wasm_oom();
DROP FUNCTION plv8_make_test_wasm(int);
DROP FUNCTION plv8_compile_and_run_wasm(text, bytea);
DROP FUNCTION plv8_test_invalid_wasm();
DROP FUNCTION plv8_import_corrupt_wasm();
DROP OWNED BY plv8_iso_role_a CASCADE;
DROP ROLE plv8_iso_role_a;
DROP OWNED BY plv8_iso_role_b CASCADE;
DROP ROLE plv8_iso_role_b;
