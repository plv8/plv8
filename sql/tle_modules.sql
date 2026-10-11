-- Regression test suite for plv8 Module Loader (plv8.packages & plv8.modules)
\set ON_ERROR_STOP on

SET client_min_messages = warning;
DROP EXTENSION IF EXISTS plv8 CASCADE;
DROP EXTENSION IF EXISTS pg_tle CASCADE;
RESET client_min_messages;

CREATE EXTENSION pg_tle;
CREATE EXTENSION plv8;

-- 1. Install versioned ES module packages in plv8.packages & plv8.modules (including multi-digit semver 1.9.0 vs 1.10.0)
INSERT INTO plv8.packages (name, version, entry_point) VALUES
    ('math_utils', '1.0.0', 'index.js'),
    ('math_utils', '1.9.0', 'index.js'),
    ('math_utils', '1.10.0', 'index.js');

INSERT INTO plv8.modules (path, package_name, package_version, source) VALUES
    ('index.js', 'math_utils', '1.0.0', $$
    export function add(a, b) { return a + b; }
    export const VERSION = '1.0.0';
    $$),
    ('index.js', 'math_utils', '1.9.0', $$
    export function add(a, b) { return (a + b) * 5; }
    export const VERSION = '1.9.0';
    $$),
    ('index.js', 'math_utils', '1.10.0', $$
    export function add(a, b) { return (a + b) * 10; }
    export const VERSION = '1.10.0';
    export const META_URL = import.meta.url;
    $$);

-- 2. Install multi-file standalone modules with relative imports and requires allowlist
INSERT INTO plv8.modules (path, source, requires, is_trusted) VALUES
    ('pkg/sub/leaf.js', $$
    export function leafVal() { return 7; }
    $$, NULL, true),
    ('pkg/helper.js', $$
    import { leafVal } from './sub/leaf.js';
    export function triple(x) { return x * 3 + leafVal(); }
    $$, ARRAY['pkg/sub/leaf.js'], true),
    ('pkg/index.js', $$
    import { triple } from './helper.js';
    export function compute(x) { return triple(x) + 1; }
    export const PKG_URL = import.meta.url;
    $$, ARRAY['pkg/helper.js'], true);

-- 3. Install a default-export-only module, a multi-file CommonJS module, a sloppy CJS module, and a package with semver dependencies
INSERT INTO plv8.modules (path, source) VALUES
    ('default_greeter', $$
    export default function greet(name) {
        return 'Hello, ' + name + '!';
    }
    $$),
    ('cjs_pkg/leaf', $$
    exports.tag = function(v) { return 'leaf:' + v; };
    $$);

INSERT INTO plv8.modules (path, source, requires, is_trusted) VALUES
    ('cjs_pkg/formatter', $$
    const math = require('tle:math_utils@1.0.0');
    const leaf = require('./leaf');
    exports.formatSum = function(a, b) {
        return 'sum=' + math.add(a, b) + ',' + leaf.tag(a);
    };
    $$, ARRAY['math_utils@1.0.0', 'cjs_pkg/leaf'], true);

INSERT INTO plv8.modules (path, source, sloppy) VALUES
    ('sloppy_cjs_mod', $$
    implicitSloppyVar = 42;
    exports.getSloppy = function() { return implicitSloppyVar; };
    $$, true);

INSERT INTO plv8.packages (name, version, entry_point, dependencies) VALUES
    ('pinned_calc', '1.0.0', 'index.js', '{"math_utils": "~1.0.0"}');

INSERT INTO plv8.modules (path, package_name, package_version, source) VALUES
    ('index.js', 'pinned_calc', '1.0.0', $$
    import { add, VERSION } from 'math_utils';
    export function calc(a, b) { return VERSION + ':' + add(a, b); }
    $$);

-- 4. Test ES static import (default highest semver version 1.10.0 over 1.9.0)
CREATE FUNCTION plv8_test_esm_latest(a int, b int) RETURNS jsonb AS $$
    import { add, VERSION, META_URL } from 'math_utils';
    return { sum: add(a, b), version: VERSION, meta_url: META_URL };
$$ LANGUAGE plv8;

SELECT plv8_test_esm_latest(3, 4);

-- 5. Test ES static import with explicit @version, semver range (^1.0.0), and package dependency pinning (~1.0.0)
CREATE FUNCTION plv8_test_esm_v1(a int, b int) RETURNS jsonb AS $$
    import { add, VERSION } from 'pgtle:math_utils@1.0.0';
    import { add as add_tle } from 'tle:math_utils@1.0.0';
    import { calc } from 'pinned_calc';
    return { sum: add(a, b), sum_tle: add_tle(a, b), version: VERSION, pinned: calc(a, b) };
$$ LANGUAGE plv8;

SELECT plv8_test_esm_v1(3, 4);

-- 6. Test transitive relative imports
CREATE FUNCTION plv8_test_relative(x int) RETURNS jsonb AS $$
    import { compute, PKG_URL } from 'pkg/index.js';
    return { result: compute(x), url: PKG_URL };
$$ LANGUAGE plv8;

SELECT plv8_test_relative(5);

-- 7. Test CommonJS require(), plv8.require(), plv8.import(), and sloppy CJS mode
CREATE FUNCTION plv8_test_require(name text) RETURNS jsonb AS $$
    const m2 = require('math_utils');
    const m1 = plv8.require('math_utils@1.0.0');
    const greet = require('default_greeter');
    const greet_ns = plv8.import('default_greeter');
    const fmt = require('tle:cjs_pkg/formatter');
    const sloppy = require('sloppy_cjs_mod');
    return {
        m2_add: m2.add(2, 3),
        m1_add: m1.add(2, 3),
        greeting: greet(name),
        ns_has_default: typeof greet_ns.default === 'function',
        cjs_fmt: fmt.formatSum(7, 8),
        sloppy_val: sloppy.getSloppy()
    };
$$ LANGUAGE plv8;

SELECT plv8_test_require('Postgres');

-- 8. Test DO block and dynamic import() returning a Promise
DO $$
    import { add, VERSION } from 'math_utils@1.0.0';
    const pkg = require('pkg/index.js');
    plv8.elog(NOTICE, 'DO block ESM add=' + add(5, 6) + ' ver=' + VERSION + ' pkg=' + pkg.compute(2));
$$ LANGUAGE plv8;

CREATE FUNCTION plv8_test_dyn_import_global() RETURNS text AS $$
    globalThis.__dyn_res = 'not_run';
    import('math_utils@1.0.0').then(ns => {
        globalThis.__dyn_res = 'dyn:' + ns.VERSION + ':' + ns.add(8, 9);
    });
    return 'initiated';
$$ LANGUAGE plv8;

CREATE FUNCTION plv8_read_dyn_import_global() RETURNS text AS $$
    return globalThis.__dyn_res;
$$ LANGUAGE plv8;

SELECT plv8_test_dyn_import_global();
SELECT plv8_read_dyn_import_global();

-- 9. Test V8 CodeCache bytecode compilation (plv8.compile_bytecode + plv8.modules)
CREATE FUNCTION plv8_compile_module_bc(src text, modname text) RETURNS bytea AS $$
    return plv8.compile_bytecode(src, modname);
$$ LANGUAGE plv8;

INSERT INTO plv8.modules (path, source, bytecode, is_trusted) VALUES (
    'fast_vec',
    '/* bytecode */',
    plv8_compile_module_bc(
        $$
        export function dot(a, b) {
            let s = 0;
            for (let i = 0; i < a.length; i++) s += a[i] * b[i];
            return s;
        }
        export const ENGINE = 'v8-code-cache';
        $$,
        'fast_vec'
    ),
    true
);

SELECT path, source, octet_length(bytecode) > 32 AS has_bytecode, is_trusted
FROM plv8.modules
WHERE path = 'fast_vec';

CREATE FUNCTION plv8_test_bytecode_mod() RETURNS jsonb AS $$
    import { dot, ENGINE } from 'fast_vec';
    const req_fast = require('fast_vec');
    return {
        esm_dot: dot([1, 2, 3], [4, 5, 6]),
        req_dot: req_fast.dot([2, 3], [10, 20]),
        engine: ENGINE
    };
$$ LANGUAGE plv8;

SELECT plv8_test_bytecode_mod();

-- 10. Test live catalog invalidation (UPDATE plv8.modules invalidates cached module & function)
UPDATE plv8.modules
SET source = $$
    export function add(a, b) { return (a + b) * 100; }
    export const VERSION = '1.10.0-patched';
    export const META_URL = import.meta.url;
$$
WHERE package_name = 'math_utils' AND package_version = '1.10.0';

SELECT plv8_test_esm_latest(3, 4);

-- 11. Test missing module error handling
CREATE FUNCTION plv8_test_missing_mod() RETURNS int AS $$
    const m = require('nonexistent_module_xyz');
    return 1;
$$ LANGUAGE plv8;

\set ON_ERROR_STOP off
SELECT plv8_test_missing_mod();
\set ON_ERROR_STOP on

-- 12. Test untrusted module security (is_trusted = false) with unprivileged role
INSERT INTO plv8.modules (path, source, is_trusted) VALUES (
    'admin_only_mod',
    $$
    export function secret() { return 'top-secret-42'; }
    $$,
    false
);

CREATE FUNCTION plv8_call_admin_mod() RETURNS text AS $$
    import { secret } from 'admin_only_mod';
    return secret();
$$ LANGUAGE plv8;

CREATE FUNCTION plv8_req_admin_mod() RETURNS text AS $$
    const m = require('admin_only_mod');
    return m.secret();
$$ LANGUAGE plv8;

-- Superuser succeeds:
SELECT plv8_call_admin_mod();
SELECT plv8_req_admin_mod();

-- Unprivileged user is denied for untrusted module, but allowed for trusted module:
DROP ROLE IF EXISTS plv8_unpriv_tester;
CREATE ROLE plv8_unpriv_tester;
GRANT USAGE ON SCHEMA plv8 TO plv8_unpriv_tester;
GRANT SELECT ON plv8.modules, plv8.packages TO plv8_unpriv_tester;
GRANT EXECUTE ON FUNCTION plv8_call_admin_mod() TO plv8_unpriv_tester;
GRANT EXECUTE ON FUNCTION plv8_req_admin_mod() TO plv8_unpriv_tester;
GRANT EXECUTE ON FUNCTION plv8_test_esm_v1(int, int) TO plv8_unpriv_tester;

SET ROLE plv8_unpriv_tester;
SELECT plv8_test_esm_v1(10, 20);
\set ON_ERROR_STOP off
SELECT plv8_call_admin_mod();
SELECT plv8_req_admin_mod();
\set ON_ERROR_STOP on
RESET ROLE;
REVOKE ALL ON plv8.modules, plv8.packages FROM plv8_unpriv_tester;
REVOKE ALL ON SCHEMA plv8 FROM plv8_unpriv_tester;
REVOKE ALL ON FUNCTION plv8_call_admin_mod() FROM plv8_unpriv_tester;
REVOKE ALL ON FUNCTION plv8_req_admin_mod() FROM plv8_unpriv_tester;
REVOKE ALL ON FUNCTION plv8_test_esm_v1(int, int) FROM plv8_unpriv_tester;
DROP ROLE plv8_unpriv_tester;
