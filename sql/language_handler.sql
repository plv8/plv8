CREATE EXTENSION IF NOT EXISTS plv8;

-- 1. Create a custom language handler in PL/v8 (ported from plv8 e03e38f7 and pljs fe44b24)
CREATE FUNCTION pseudolang_handler() RETURNS language_handler AS $$
  globalThis.transpile_count = (globalThis.transpile_count || 0) + 1;
  return arguments[0].split("RETURN").join("return");
$$ LANGUAGE plv8;

CREATE FUNCTION get_transpile_count() RETURNS int AS $$
  return globalThis.transpile_count || 0;
$$ LANGUAGE plv8;

CREATE TRUSTED LANGUAGE pseudolang
  HANDLER pseudolang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;

-- Validator transpiles and caches at CREATE FUNCTION time
CREATE FUNCTION test_pl(a int) RETURNS int AS $$
  RETURN a * 2;
$$ LANGUAGE pseudolang;

SELECT get_transpile_count() AS count_after_create;

-- First call: uses cached compiled function
SELECT test_pl(21);
SELECT get_transpile_count() AS count_after_first_call;

-- Second call: cache hit, uses cached compiled function without re-transpiling
SELECT test_pl(21);
SELECT get_transpile_count() AS count_after_second_call;

-- 2. Inline DO block in custom language
DO $$
  var msg = "inline pseudolang works";
  plv8.elog(NOTICE, msg);
  RETURN;
$$ LANGUAGE pseudolang;

-- 3. plv8.find_function() with custom-language functions and non-JS functions
CREATE FUNCTION uncalled_pl(x int) RETURNS int AS $$
  RETURN x + 5;
$$ LANGUAGE pseudolang;

CREATE FUNCTION call_via_find(x int) RETURNS int AS $$
  var f1 = plv8.find_function("test_pl");
  var f2 = plv8.find_function("uncalled_pl");
  return f1(x) + f2(x);
$$ LANGUAGE plv8;

SELECT call_via_find(10);

CREATE FUNCTION sql_only_fn() RETURNS int AS $$ SELECT 1 $$ LANGUAGE sql;
CREATE FUNCTION find_sql_fn() RETURNS int AS $$
  var f = plv8.find_function("sql_only_fn");
  return f();
$$ LANGUAGE plv8;

\set VERBOSITY terse
SELECT find_sql_fn();
\set VERBOSITY default

CREATE FUNCTION plpgsql_only_fn() RETURNS int AS $$
BEGIN
  RETURN 1;
END;
$$ LANGUAGE plpgsql;

CREATE FUNCTION find_plpgsql_fn() RETURNS int AS $$
  var f = plv8.find_function("plpgsql_only_fn");
  return f();
$$ LANGUAGE plv8;

\set VERBOSITY terse
SELECT find_plpgsql_fn();
\set VERBOSITY default

-- 4. Chained language handlers (lang_b handler written in pseudolang)
CREATE FUNCTION lang_b_handler() RETURNS language_handler AS $$
  RETURN arguments[0].split("DOUBLE_RET ").join("RETURN 2 * ");
$$ LANGUAGE pseudolang;

CREATE LANGUAGE lang_b
  HANDLER lang_b_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;

CREATE FUNCTION test_chained(x int) RETURNS int AS $$
  DOUBLE_RET x;
$$ LANGUAGE lang_b;

SELECT test_chained(15);

DO $$
  plv8.elog(NOTICE, "chained inline DO works");
$$ LANGUAGE lang_b;

-- 5. Triggers, procedures, and set-returning functions (storage isolation check)
CREATE TABLE trig_pl_tbl(x int);

CREATE FUNCTION trig_pl_fn() RETURNS trigger AS $$
  NEW.x = NEW.x * 10;
  RETURN NEW;
$$ LANGUAGE pseudolang;

CREATE TRIGGER tr_pl BEFORE INSERT ON trig_pl_tbl
  FOR EACH ROW EXECUTE FUNCTION trig_pl_fn();

INSERT INTO trig_pl_tbl VALUES (5);
SELECT x FROM trig_pl_tbl;

CREATE PROCEDURE proc_pl(v int) AS $$
  plv8.execute("INSERT INTO trig_pl_tbl VALUES ($1)", [v]);
  RETURN;
$$ LANGUAGE pseudolang;

CALL proc_pl(7);
SELECT x FROM trig_pl_tbl ORDER BY x;

-- Handler that attempts plv8.return_next during transpilation inside an SRF:
-- storage isolation ensures the handler cannot pollute the outer SRF's return_state
CREATE FUNCTION srf_iso_handler() RETURNS language_handler AS $$
  try {
    plv8.return_next(999);
  } catch (e) {
    // Expected: return_next is rejected inside a language handler
  }
  return arguments[0].split("RETURN").join("return");
$$ LANGUAGE plv8;

CREATE LANGUAGE srf_iso_lang HANDLER srf_iso_handler;

CREATE FUNCTION srf_helper_pl(v int) RETURNS int AS $$
  RETURN v + 100;
$$ LANGUAGE srf_iso_lang;

CREATE FUNCTION srf_pl() RETURNS SETOF int AS $$
  plv8.return_next(1);
  var h = plv8.find_function("srf_helper_pl");
  plv8.return_next(h(2));
  RETURN;
$$ LANGUAGE pseudolang;

SELECT * FROM srf_pl();

-- 6. Language handler using SPI (plv8.execute) during transpilation
CREATE FUNCTION spi_lang_handler() RETURNS language_handler AS $$
  var rows = plv8.execute("SELECT 'return ' AS prefix");
  return rows[0].prefix + "(" + arguments[0] + ");";
$$ LANGUAGE plv8;

CREATE LANGUAGE spi_lang
  HANDLER spi_lang_handler
  VALIDATOR plv8_call_validator;

CREATE FUNCTION test_spi_lang(x int) RETURNS int AS $$
  x + 99
$$ LANGUAGE spi_lang;

SELECT test_spi_lang(1);

-- 7. Validator & error path negative tests
\set VERBOSITY terse

-- Invalid transpiled JS syntax rejected by validator at CREATE FUNCTION time
CREATE FUNCTION bad_syntax_pl() RETURNS int AS $$
  RETURN (unclosed;
$$ LANGUAGE pseudolang;

-- Disallowed pseudotype return rejected by validator
CREATE FUNCTION bad_rettype() RETURNS cstring AS $$
  return "abc";
$$ LANGUAGE plv8;

-- Handler that throws a JS exception during transpilation
CREATE FUNCTION throwing_handler() RETURNS language_handler AS $$
  throw new Error("transpile failed intentionally");
$$ LANGUAGE plv8;

CREATE LANGUAGE throwing_lang
  HANDLER throwing_handler
  VALIDATOR plv8_call_validator;

CREATE FUNCTION bad_transpile() RETURNS int AS $$
  123
$$ LANGUAGE throwing_lang;

-- Handler that throws when resolved via plv8.find_function inside try/catch
CREATE LANGUAGE throwing_lang_noval HANDLER throwing_handler;

CREATE FUNCTION bad_transpile_noval() RETURNS int AS $$
  123
$$ LANGUAGE throwing_lang_noval;

CREATE FUNCTION catch_failed_transpile() RETURNS text AS $$
  try {
    plv8.find_function("bad_transpile_noval");
    return "unexpected success";
  } catch (e) {
    var rows = plv8.execute("SELECT 'spi still works' AS status");
    return e.message + " / " + rows[0].status;
  }
$$ LANGUAGE plv8;

SELECT catch_failed_transpile();

-- Handler that returns null
CREATE FUNCTION null_handler() RETURNS language_handler AS $$
  return null;
$$ LANGUAGE plv8;

CREATE LANGUAGE null_lang HANDLER null_handler;

CREATE FUNCTION null_transpile() RETURNS int AS $$
  123
$$ LANGUAGE null_lang;

SELECT null_transpile();

\set VERBOSITY default

-- 8. Multi-role execution (fresh per-user V8 context during validation and call)
CREATE ROLE regress_plv8_lang_user;
GRANT CREATE ON SCHEMA public TO regress_plv8_lang_user;
GRANT USAGE ON LANGUAGE plv8 TO regress_plv8_lang_user;
GRANT USAGE ON LANGUAGE pseudolang TO regress_plv8_lang_user;

SET ROLE regress_plv8_lang_user;

CREATE FUNCTION role_pl_fn(a int) RETURNS int AS $$
  RETURN a + 1000;
$$ LANGUAGE pseudolang;

SELECT role_pl_fn(23);

RESET ROLE;

-- 9. Automatic cache invalidation when language handlers are replaced
CREATE FUNCTION other_lang_handler() RETURNS language_handler AS $$
  globalThis.other_transpile_count = (globalThis.other_transpile_count || 0) + 1;
  return arguments[0].split("RET_OTHER").join("return");
$$ LANGUAGE plv8;

CREATE FUNCTION get_other_transpile_count() RETURNS int AS $$
  return globalThis.other_transpile_count || 0;
$$ LANGUAGE plv8;

CREATE LANGUAGE other_lang
  HANDLER other_lang_handler
  VALIDATOR plv8_call_validator;

CREATE FUNCTION test_other_lang(x int) RETURNS int AS $$
  RET_OTHER x + 1;
$$ LANGUAGE other_lang;

SELECT test_other_lang(10);
SELECT get_other_transpile_count() AS other_count_before_pseudolang_replace;

-- 9a. Replacing pseudolang_handler in the same backend invalidates test_pl and uncalled_pl
CREATE OR REPLACE FUNCTION pseudolang_handler() RETURNS language_handler AS $$
  globalThis.transpile_count = (globalThis.transpile_count || 0) + 1;
  return arguments[0]
    .split("RETURN").join("return")
    .split("a * 2").join("a * 3")
    .split("x + 5").join("x + 50");
$$ LANGUAGE plv8;

SELECT get_transpile_count() AS count_before_retranspile;
SELECT test_pl(21) AS test_pl_after_handler_replace;
SELECT get_transpile_count() AS count_after_retranspile;

-- Subsequent call uses the newly cached compilation without re-transpiling
SELECT test_pl(21) AS test_pl_cached_again;
SELECT get_transpile_count() AS count_after_cached_call;

-- plv8.find_function() also picks up the invalidated downstream functions
SELECT call_via_find(10) AS call_via_find_after_handler_replace;

-- Unrelated custom language functions remain cached and are not re-transpiled
SELECT test_other_lang(10);
SELECT get_other_transpile_count() AS other_count_after_pseudolang_replace;

-- 9b. Chained handler invalidation: replacing intermediate handler (lang_b_handler)
CREATE OR REPLACE FUNCTION lang_b_handler() RETURNS language_handler AS $$
  RETURN arguments[0].split("DOUBLE_RET ").join("RETURN 4 * ");
$$ LANGUAGE pseudolang;

SELECT test_chained(15) AS test_chained_after_mid_handler_replace;

-- 9c. Chained handler invalidation: replacing root handler (pseudolang_handler)
CREATE OR REPLACE FUNCTION pseudolang_handler() RETURNS language_handler AS $$
  globalThis.transpile_count = (globalThis.transpile_count || 0) + 1;
  return arguments[0]
    .split("RETURN").join("return")
    .split("a * 2").join("a * 3")
    .split("x + 5").join("x + 50")
    .split("4 * ").join("5 * ");
$$ LANGUAGE plv8;

SELECT test_chained(15) AS test_chained_after_root_handler_replace;

-- 9d. Replacing handler with check_function_bodies = off still invalidates
SET check_function_bodies = off;
CREATE OR REPLACE FUNCTION pseudolang_handler() RETURNS language_handler AS $$
  globalThis.transpile_count = (globalThis.transpile_count || 0) + 1;
  return arguments[0]
    .split("RETURN").join("return")
    .split("a * 2").join("a * 4")
    .split("4 * ").join("6 * ");
$$ LANGUAGE plv8;
RESET check_function_bodies;

SELECT test_pl(21) AS test_pl_after_nocheck_replace;
SELECT test_chained(15) AS test_chained_after_nocheck_replace;

-- 9e. Cross-backend handler replacement invalidates cached downstream functions
\setenv PGDATABASE :DBNAME
\! psql -X -q -c 'CREATE OR REPLACE FUNCTION pseudolang_handler() RETURNS language_handler AS $$ globalThis.transpile_count = (globalThis.transpile_count || 0) + 1; return arguments[0].split("RETURN").join("return").split("a * 2").join("a * 10").split("4 * ").join("10 * "); $$ LANGUAGE plv8;'

SELECT test_pl(21) AS test_pl_after_cross_backend_replace;
SELECT test_chained(15) AS test_chained_after_cross_backend_replace;

-- 9f. Re-entrant handler replacement from inside a running custom-language
-- function and from inside a handler during transpilation
CREATE FUNCTION reentrant_handler() RETURNS language_handler AS $$
  if (globalThis.replace_during_transpile) {
    globalThis.replace_during_transpile = false;
    plv8.execute(
      "CREATE OR REPLACE FUNCTION reentrant_handler() RETURNS language_handler AS $h$ " +
      "return arguments[0].split('RET_RE').join('return 1000 + '); " +
      "$h$ LANGUAGE plv8"
    );
  }
  return arguments[0].split("RET_RE").join("return 100 + ");
$$ LANGUAGE plv8;

CREATE LANGUAGE reentrant_lang HANDLER reentrant_handler;

CREATE FUNCTION reentrant_fn(x int, do_replace bool) RETURNS int AS $$
  if (do_replace) {
    plv8.execute(
      "CREATE OR REPLACE FUNCTION reentrant_handler() RETURNS language_handler AS $h$ " +
      "return arguments[0].split('RET' + '_RE').join('return 200 + '); " +
      "$h$ LANGUAGE plv8"
    );
  }
  var suffix = "constant_pool_survived_" + x;
  RET_RE (x + suffix.length);
$$ LANGUAGE reentrant_lang;

CREATE FUNCTION reentrant_fn(a int, b int, c int) RETURNS int AS $$
  RET_RE (a + b + c);
$$ LANGUAGE reentrant_lang;

SELECT reentrant_fn(5, false) AS reentrant_fn_initial;
SELECT reentrant_fn(1, 2, 3) AS reentrant_fn_3arg_initial;

SELECT reentrant_fn(5, true) AS reentrant_fn_during_self_evict;

-- Next call to both overloads re-transpiles using the new handler (200 + ...)
SELECT reentrant_fn(5, false) AS reentrant_fn_after_mid_call_replace;
SELECT reentrant_fn(1, 2, 3) AS reentrant_fn_3arg_after_replace;

-- Now test handler replacing itself during transpilation of a new function
CREATE FUNCTION set_replace_during_transpile() RETURNS void AS $$
  globalThis.replace_during_transpile = true;
$$ LANGUAGE plv8;

CREATE OR REPLACE FUNCTION reentrant_handler() RETURNS language_handler AS $$
  if (globalThis.replace_during_transpile) {
    globalThis.replace_during_transpile = false;
    plv8.execute(
      "CREATE OR REPLACE FUNCTION reentrant_handler() RETURNS language_handler AS $h$ " +
      "return arguments[0].split('RET_RE').join('return 1000 + '); " +
      "$h$ LANGUAGE plv8"
    );
  }
  return arguments[0].split("RET_RE").join("return 100 + ");
$$ LANGUAGE plv8;

SELECT set_replace_during_transpile();

-- First call runs the old handler (which replaces itself mid-transpile and returns 100 + x)
SELECT reentrant_fn(5, false) AS reentrant_fn_transpile_replace_first;

-- Second call detects that reentrant_handler's catalog tuple changed during the
-- previous transpilation and re-transpiles with the new handler (1000 + x)
SELECT reentrant_fn(5, false) AS reentrant_fn_transpile_replace_second;

DROP FUNCTION set_replace_during_transpile();
DROP FUNCTION reentrant_fn(int, int, int);
DROP FUNCTION reentrant_fn(int, bool);
DROP LANGUAGE reentrant_lang;
DROP FUNCTION reentrant_handler();

-- 9g. Re-entrant compilation of the same function OID during transpilation
CREATE FUNCTION self_compile_handler() RETURNS language_handler AS $$
  if (!globalThis.in_self_compile) {
    globalThis.in_self_compile = true;
    var f = plv8.find_function("self_compile_fn");
    globalThis.inner_self_compile_res = f(7);
    globalThis.in_self_compile = false;
  }
  return arguments[0].split("RET_SC").join("return");
$$ LANGUAGE plv8;

CREATE LANGUAGE self_compile_lang HANDLER self_compile_handler;

CREATE FUNCTION self_compile_fn(x int) RETURNS int AS $$
  RET_SC x * 11;
$$ LANGUAGE self_compile_lang;

SELECT self_compile_fn(6) AS self_compile_outer_result;
SELECT self_compile_fn(6) AS self_compile_cached_result;

DROP FUNCTION self_compile_fn(int);
DROP LANGUAGE self_compile_lang;
DROP FUNCTION self_compile_handler();

-- 9h. Transaction rollback of CREATE OR REPLACE FUNCTION on a language handler
-- reverts catalog tuple (xmin, tid) and invalidates the rolled-back compilation
BEGIN;
CREATE OR REPLACE FUNCTION pseudolang_handler() RETURNS language_handler AS $$
  globalThis.transpile_count = (globalThis.transpile_count || 0) + 1;
  return arguments[0]
    .split("RETURN").join("return")
    .split("a * 2").join("a * 100");
$$ LANGUAGE plv8;

SELECT test_pl(21) AS test_pl_inside_tx;
ROLLBACK;

SELECT test_pl(21) AS test_pl_after_rollback;

-- Cleanup
DROP FUNCTION test_other_lang(int);
DROP LANGUAGE other_lang;
DROP FUNCTION get_other_transpile_count();
DROP FUNCTION other_lang_handler();

DROP FUNCTION role_pl_fn(int);
REVOKE CREATE ON SCHEMA public FROM regress_plv8_lang_user;
REVOKE USAGE ON LANGUAGE pseudolang FROM regress_plv8_lang_user;
REVOKE USAGE ON LANGUAGE plv8 FROM regress_plv8_lang_user;
DROP ROLE regress_plv8_lang_user;

DROP FUNCTION null_transpile();
DROP LANGUAGE null_lang;
DROP FUNCTION null_handler();

DROP FUNCTION catch_failed_transpile();
DROP FUNCTION bad_transpile_noval();
DROP LANGUAGE throwing_lang_noval;
DROP LANGUAGE throwing_lang;
DROP FUNCTION throwing_handler();

DROP FUNCTION test_spi_lang(int);
DROP LANGUAGE spi_lang;
DROP FUNCTION spi_lang_handler();

DROP FUNCTION srf_pl();
DROP FUNCTION srf_helper_pl(int);
DROP LANGUAGE srf_iso_lang;
DROP FUNCTION srf_iso_handler();
DROP PROCEDURE proc_pl(int);
DROP TRIGGER tr_pl ON trig_pl_tbl;
DROP FUNCTION trig_pl_fn();
DROP TABLE trig_pl_tbl;

DROP FUNCTION test_chained(int);
DROP LANGUAGE lang_b;
DROP FUNCTION lang_b_handler();

DROP FUNCTION find_plpgsql_fn();
DROP FUNCTION plpgsql_only_fn();
DROP FUNCTION find_sql_fn();
DROP FUNCTION sql_only_fn();
DROP FUNCTION call_via_find(int);
DROP FUNCTION uncalled_pl(int);
DROP FUNCTION test_pl(int);
DROP LANGUAGE pseudolang;
DROP FUNCTION get_transpile_count();
DROP FUNCTION pseudolang_handler();
