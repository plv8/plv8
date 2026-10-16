# PL/v8 Language Handler API Users' Guide

Welcome to the **PL/v8 Language Handler API Users' Guide**. This guide explains how to build, register, test, and optimize new PostgreSQL procedural languages, domain-specific languages (DSLs), and WebAssembly runtimes that compile to or execute within `pl/v8`.

For internal C++ implementation details, data structures, and syscache invalidation mechanics, see the companion [PL/v8 Language Handler Architecture & Design Document](LANGUAGE_HANDLER_DESIGN.md).

---

## Table of Contents

1. [Overview: What Is a PL/v8 Language Handler?](#1-overview-what-is-a-plv8-language-handler)
2. [Quickstart: Creating Your First Language in 60 Seconds](#2-quickstart-creating-your-first-language-in-60-seconds)
3. [The Language Handler Function Contract (`src`, `meta`)](#3-the-language-handler-function-contract-src-meta)
4. [Supporting All PostgreSQL Execution Modes](#4-supporting-all-postgresql-execution-modes)
   - [4.1 Scalar & Composite Functions](#41-scalar--composite-functions)
   - [4.2 Set-Returning Functions (SRFs)](#42-set-returning-functions-srfs)
   - [4.3 `OUT`, `INOUT`, and `RETURNS TABLE` Parameters](#43-out-inout-and-returns-table-parameters)
   - [4.4 Trigger Functions (`RETURNS trigger`)](#44-trigger-functions-returns-trigger)
   - [4.5 Anonymous `DO` Blocks (`INLINE plv8_inline_handler`)](#45-anonymous-do-blocks-inline-plv8_inline_handler)
   - [4.6 Compile-Time Syntax Validation (`VALIDATOR plv8_call_validator`)](#46-compile-time-syntax-validation-validator-plv8_call_validator)
5. [Multi-Stage Chained Language Handlers](#5-multi-stage-chained-language-handlers)
6. [Compile-Time Catalog Introspection via SPI](#6-compile-time-catalog-introspection-via-spi)
7. [Raw Binary Custom Base Types (`meta.pass_user_types_as_bytes`)](#7-raw-binary-custom-base-types-metapass_user_types_as_bytes)
8. [Structured PostgreSQL Error Reporting & Diagnostics](#8-structured-postgresql-error-reporting--diagnostics)
9. [Using ES Modules, CommonJS `require()`, and Shared WASM Caching](#9-using-es-modules-commonjs-require-and-shared-wasm-caching)
10. [Using the Built-In PL/v8-Hosted Languages](#10-using-the-built-in-plv8-hosted-languages)
    - [10.1 `plpgsql_v8` (PL/pgSQL on V8)](#101-plpgsql_v8-plpgsql-on-v8)
    - [10.2 `sql/tsm` (T-SQL) and `sql/psm` (SQL/PSM)](#102-sqltsm-t-sql-and-sqlpsm-sqlpsm)
    - [10.3 `pl/fullpython+v8` (Python 3.15 + WASM on V8)](#103-plfullpythonv8-python-315--wasm-on-v8)
    - [10.4 `pl/v8-wasm` (WebAssembly Functions, Types & Operators)](#104-plv8-wasm-webassembly-functions-types--operators)
11. [Best Practices, Security & Troubleshooting](#11-best-practices-security--troubleshooting)

---

## 1. Overview: What Is a PL/v8 Language Handler?

In PostgreSQL, a procedural language (`CREATE LANGUAGE`) is backed by up to three functions:
1. **`HANDLER` (`lanplcallfoid`)**: Executes functions and triggers written in the language.
2. **`INLINE` (`laninline`)**: Executes anonymous `DO $$ ... $$ LANGUAGE <lang>` blocks.
3. **`VALIDATOR` (`lanvalidator`)**: Validates syntax when `CREATE FUNCTION ... LANGUAGE <lang>` is executed.

Normally, all three must be C functions compiled into a `.so` shared library. With **`pl/v8` Language Handlers**, you write only the **`HANDLER` function in `plv8`** (or in any language whose handler chain terminates at `plv8`), and reuse `plv8_inline_handler` and `plv8_call_validator` for `INLINE` and `VALIDATOR`:

```sql
CREATE FUNCTION my_lang_handler()
RETURNS language_handler AS $$
  const src = arguments[0];   // Raw source string from CREATE FUNCTION or DO block
  const meta = arguments[1];  // Compile-time metadata object from pl/v8
  return transpileToJS(src, meta);
$$ LANGUAGE plv8;

CREATE TRUSTED LANGUAGE my_lang
  HANDLER my_lang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;
```

### How It Works Under the Hood
- **At `CREATE FUNCTION` time**: `plv8_call_validator` invokes `my_lang_handler(src, meta)` to transpile the function body into JavaScript and compiles the resulting JavaScript with V8. If either your handler throws an error or V8 rejects the generated JavaScript syntax, `CREATE FUNCTION` fails immediately with your error message and SQLSTATE.
- **At First Execution**: `plv8_call_handler` transpiles the function body via `my_lang_handler(src, meta)`, compiles the returned JavaScript into a cached `v8::Persistent<v8::Function>`, and runs it.
- **On Subsequent Executions**: `plv8` executes the cached `v8::Function` directly at native V8 speed **without re-running `my_lang_handler`**, unless either the function itself or `my_lang_handler` is replaced via `CREATE OR REPLACE FUNCTION`.

---

## 2. Quickstart: Creating Your First Language in 60 Seconds

Let's create a simple expression language called `calc_lang` where:
- Arguments are referenced as `$1`, `$2`, ... (or by their SQL parameter names),
- `=> <expr>` is shorthand for `return (<expr>);`.

```sql
CREATE EXTENSION IF NOT EXISTS plv8;

-- 1. Define the language handler in plv8
CREATE OR REPLACE FUNCTION calc_lang_handler()
RETURNS language_handler AS $$
  const src = arguments[0];
  const meta = arguments[1] || {};

  // Reject empty bodies at compile time
  if (!src || !src.trim()) {
    const err = new Error('calc_lang function body cannot be empty');
    err.sqlerrcode = '42601'; // syntax_error
    throw err;
  }

  // Rewrite $1, $2, ... to arguments[0], arguments[1], ...
  let js = src.replace(/\$(\d+)\b/g, function(_, n) {
    return 'arguments[' + (parseInt(n, 10) - 1) + ']';
  });

  // Rewrite leading "=> expr" to "return (expr);"
  js = js.replace(/^\s*=>\s*([\s\S]+?)\s*;?\s*$/, 'return ($1);');

  return js;
$$ LANGUAGE plv8;

-- Prevent direct SQL calls to the handler function
REVOKE ALL ON FUNCTION calc_lang_handler() FROM PUBLIC;

-- 2. Register the procedural language
CREATE TRUSTED LANGUAGE calc_lang
  HANDLER calc_lang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;

-- 3. Create and call functions in calc_lang!
CREATE FUNCTION hypotenuse(a float8, b float8)
RETURNS float8 AS $$
  => Math.sqrt($1 * $1 + b * b)
$$ LANGUAGE calc_lang IMMUTABLE STRICT;

SELECT hypotenuse(3, 4);
-- Result: 5

-- 4. Run an anonymous DO block in calc_lang!
DO $$
  plv8.elog(NOTICE, 'calc_lang DO block works!');
$$ LANGUAGE calc_lang;
```

---

## 3. The Language Handler Function Contract (`src`, `meta`)

### 3.1 Function Signature
A language handler function must be created with:
- **Zero declared SQL parameters** (`CREATE FUNCTION my_handler()`)
- **Return type `language_handler`** (`RETURNS language_handler`)
- **Language `plv8`** (or another custom language whose handler chain resolves to `plv8`)

Inside the handler body, `pl/v8` passes two runtime arguments via `arguments`:
- **`const src = arguments[0]` (`string`)**: The raw source code of the SQL function (`pg_proc.prosrc`) or `DO` block.
- **`const meta = arguments[1]` (`object`)**: A metadata object describing the function being compiled.

> **Important Note on Return Value:**  
> Your handler must return a **JavaScript function body string** (the statements that go *inside* `function(arg0, arg1, ...) { ... }`), **not** an outer `function(...) { ... }` declaration—unless your returned string uses top-level ES Module `export default function(...) { ... }` (see [Section 9.2](#92-generating-es-module-imports-and-export-default-from-a-handler)).

### 3.2 Complete `meta` Object Reference

When compiling a `CREATE FUNCTION` / `CREATE PROCEDURE` (`meta.is_do_block === false`), `meta` has the following properties:

```javascript
{
  is_do_block: false,                 // boolean: false for catalog functions, true for DO blocks
  fn_oid: 16420,                      // number (Uint32): pg_proc.oid of the target function
  proname: "my_func",                 // string: pg_proc.proname
  rettype_oid: 23,                    // number (Uint32): pg_proc.prorettype (e.g. 23 = int4, 2279 = trigger)
  rettype_category: "N",              // string (1 char): pg_type.typcategory of return type
  rettype_len: 4,                     // number (Int32): payload byte length, or -1 for varlena
  argnames: ["x", "y"],               // string[]: input parameter names ("" for unnamed parameters)
  argtype_oids: [23, 25],             // number[]: input parameter type OIDs (length == pronargs)
  argtype_categories: ["N", "S"],     // string[]: 1-char typcategory for each input parameter
  argtype_lens: [4, -1]               // number[]: payload byte length for each input parameter (-1 for varlena)
}
```

When compiling an anonymous `DO $$ ... $$` block (`plv8_inline_handler`), no `pg_proc` tuple exists, so `meta` is:

```javascript
{
  is_do_block: true
}
```

#### Common `pg_type.typcategory` Codes (`rettype_category` / `argtype_categories`)

| Category Char | Meaning | Examples |
|---|---|---|
| `'N'` | Numeric types | `int2` (`21`), `int4` (`23`), `int8` (`20`), `float4` (`700`), `float8` (`701`), `numeric` (`1700`) |
| `'S'` | String types | `text` (`25`), `varchar` (`1043`), `bpchar` (`1042`) |
| `'B'` | Boolean type | `bool` (`16`) |
| `'D'` | Date/Time types | `date` (`1082`), `timestamp` (`1114`), `timestamptz` (`1184`) |
| `'C'` | Composite types | Table row types, `CREATE TYPE ... AS (...)`, `record` (`2249`) |
| `'A'` | Array types | `int4[]` (`1007`), `text[]` (`1009`), etc. |
| `'P'` | Pseudo-types | `void` (`2278`), `trigger` (`2279`), `anyelement` (`2283`), etc. |
| `'U'` | User-defined types | `bytea` (`17`), `json` (`114`), `jsonb` (`3802`), `uuid` (`2950`), and custom base types (`TYPCATEGORY_USER`) |

#### Output Flag: `meta.pass_user_types_as_bytes`
Your handler can mutate `meta.pass_user_types_as_bytes = true;` before returning. When set to `true`, `pl/v8` marshals user-defined base types (`typcategory === 'U'`, excluding composites and built-in `json`/`jsonb`/`bytea`) directly as `Uint8Array` views of their raw varlena payload bytes rather than invoking PostgreSQL's text input/output functions. See [Section 7](#7-raw-binary-custom-base-types-metapass_user_types_as_bytes) for details.

---

## 4. Supporting All PostgreSQL Execution Modes

### 4.1 Scalar & Composite Functions

Inside the JavaScript code returned by your language handler:
- Named SQL input parameters are bound as local JavaScript identifiers (`meta.argnames[i]`) **and** all input parameters are always accessible via `arguments[0]`, `arguments[1]`, etc.
- For unnamed SQL parameters (`CREATE FUNCTION f(int, int)`), `meta.argnames[i]` is `""`, and `pl/v8` binds positional parameter names `$1`, `$2`, ... as well as `arguments[0]`, `arguments[1]`, ...
- Return a scalar value (`number`, `string`, `boolean`, `BigInt`, `Date`, `Uint8Array`, `Array`, or `null`) for scalar return types, or a plain JavaScript object `{ col1: val1, col2: val2 }` for composite/`record` return types.

### 4.2 Set-Returning Functions (SRFs)

For functions declared with `RETURNS SETOF <type>` or `RETURNS TABLE (...)`:
- Emit each row by calling `plv8.return_next(rowOrScalar)` inside the generated JavaScript.
- Return nothing (`return;`) at the end of the function body.

```sql
-- Example: Transpiling a "YIELD <expr>;" statement in a custom language
-- into plv8.return_next(<expr>);
CREATE OR REPLACE FUNCTION yield_lang_handler()
RETURNS language_handler AS $$
  const src = arguments[0];
  return src.replace(/\bYIELD\s+([^;]+);/g, 'plv8.return_next($1);');
$$ LANGUAGE plv8;
```

### 4.3 `OUT`, `INOUT`, and `RETURNS TABLE` Parameters

In PostgreSQL:
- Single `OUT` parameter (without `RETURNS SETOF`): the function returns that single scalar value directly.
- Multiple `OUT` / `INOUT` parameters: the function returns a JavaScript object whose keys match the `OUT` / `INOUT` parameter names.
- `RETURNS TABLE (col1 type1, col2 type2)`: `plv8.return_next({ col1: ..., col2: ... })` emits each row.

Because `meta.argnames` and `meta.argtype_oids` only list input-bearing parameters (`IN`, `INOUT`, `VARIADIC`), if your language binds `OUT` or `RETURNS TABLE` column names as local variables (like PL/pgSQL and T-SQL do), query `pg_catalog.pg_proc` using `meta.fn_oid` at compile time (see [Section 6](#6-compile-time-catalog-introspection-via-spi)).

### 4.4 Trigger Functions (`RETURNS trigger`)

When a function is declared with `RETURNS trigger` (`meta.rettype_oid === 2279`), `pl/v8` compiles the generated JavaScript with the following pre-bound local variables:
- `NEW`: Mutable JavaScript object representing the new row (for `INSERT`/`UPDATE` row-level triggers).
- `OLD`: JavaScript object representing the old row (for `UPDATE`/`DELETE` row-level triggers).
- `TG_NAME`: Trigger name (`string`).
- `TG_WHEN`: `'BEFORE'`, `'AFTER'`, or `'INSTEAD OF'`.
- `TG_LEVEL`: `'ROW'` or `'STATEMENT'`.
- `TG_OP`: `'INSERT'`, `'UPDATE'`, `'DELETE'`, or `'TRUNCATE'`.
- `TG_RELID`: OID of the target table (`number`).
- `TG_TABLE_NAME`: Table name (`string`).
- `TG_TABLE_SCHEMA`: Schema name (`string`).
- `TG_ARGV`: `Array<string>` of trigger arguments from `CREATE TRIGGER ... EXECUTE FUNCTION fn('arg0', ...)`.

Your generated JS should `return NEW;` (to proceed with the possibly modified row), `return OLD;`, or `return null;` (to skip the operation in a `BEFORE` row-level trigger).

### 4.5 Anonymous `DO` Blocks (`INLINE plv8_inline_handler`)

To support `DO $$ ... $$ LANGUAGE my_lang;`, include `INLINE plv8_inline_handler` in `CREATE LANGUAGE`:

```sql
CREATE TRUSTED LANGUAGE my_lang
  HANDLER my_lang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;
```

When a `DO` block runs, `my_lang_handler(src, meta)` is called with `meta = { is_do_block: true }`. Note that `meta.fn_oid` and `meta.proname` are `undefined` for `DO` blocks, so always guard catalog lookups with `if (meta && !meta.is_do_block && meta.fn_oid > 0)`.

### 4.6 Compile-Time Syntax Validation (`VALIDATOR plv8_call_validator`)

When `VALIDATOR plv8_call_validator` is specified on `CREATE LANGUAGE`, PostgreSQL runs `plv8_call_validator` during `CREATE FUNCTION` (whenever `SET check_function_bodies = on`, the default).

`plv8_call_validator`:
1. Connects to SPI (`SPI_connect()`), so your handler can run `plv8.execute(...)` queries even during `CREATE FUNCTION`.
2. Calls your handler `my_lang_handler(src, meta)` inside a `HandlerExecutionScope`.
3. Compiles the JavaScript string returned by your handler using V8.

If your handler throws an `Error` (for example, `const e = new Error('syntax error at line 3'); e.sqlerrcode = '42601'; throw e;`), `CREATE FUNCTION` aborts immediately and reports your exact error and SQLSTATE to the SQL client.

---

## 5. Multi-Stage Chained Language Handlers

`pl/v8` supports **multi-stage language handler chains** up to 32 levels deep (`PLV8_MAX_LANG_HANDLER_DEPTH = 32`).

### 5.1 How Chained Handlers Work

Suppose:
1. `pseudolang` has handler `pseudolang_handler` written in `plv8`.
2. `lang_b` has handler `lang_b_handler` written in `pseudolang`.
3. Function `test_chained(x int)` is written in `lang_b`.

When `test_chained` is compiled:
1. `pl/v8` walks the language chain (`lang_b` $\rightarrow$ `lang_b_handler` in `pseudolang` $\rightarrow$ `pseudolang_handler` in `plv8`) and records **both** `lang_b_handler` and `pseudolang_handler` in `test_chained`'s dependency array (`cache->handlers[0..1]`).
2. `pl/v8` compiles the immediate handler `lang_b_handler` via `find_js_function(lang_b_handler_oid)`. Because `lang_b_handler` is written in `pseudolang`, `pl/v8` invokes `pseudolang_handler` to transpile `lang_b_handler`'s source code into a V8 `Function`.
3. `pl/v8` then invokes the compiled `lang_b_handler(src, meta)` on `test_chained`'s source code, and `lang_b_handler` returns the final JavaScript source string for `test_chained`!

```sql
-- Stage 1: pseudolang replaces "RETURN" with "return"
CREATE FUNCTION pseudolang_handler()
RETURNS language_handler AS $$
  return arguments[0].split("RETURN").join("return");
$$ LANGUAGE plv8;

CREATE TRUSTED LANGUAGE pseudolang
  HANDLER pseudolang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;

-- Stage 2: lang_b_handler is written in pseudolang!
-- It transpiles "DOUBLE_RET <expr>" into JavaScript "return 2 * <expr>".
CREATE FUNCTION lang_b_handler()
RETURNS language_handler AS $$
  RETURN arguments[0].split("DOUBLE_RET ").join("return 2 * ");
$$ LANGUAGE pseudolang;

CREATE TRUSTED LANGUAGE lang_b
  HANDLER lang_b_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;

-- Target function written in lang_b:
CREATE FUNCTION test_chained(x int)
RETURNS int AS $$
  DOUBLE_RET x;
$$ LANGUAGE lang_b;

SELECT test_chained(21);
-- Result: 42
```

### 5.2 Automatic Cascading Cache Invalidation

Because `pl/v8` records every handler in the chain (`cache->handlers[0 .. nhandlers-1]`) using its `pg_proc` tuple's `(fn_xmin, fn_tid)`:
- If you run `CREATE OR REPLACE FUNCTION lang_b_handler() ...`, `test_chained` is automatically invalidated and recompiled on its next call.
- If you run `CREATE OR REPLACE FUNCTION pseudolang_handler() ...` (at the root of the chain), **both** `lang_b_handler` and `test_chained` are automatically invalidated and recompiled on the next call to `test_chained`!

```sql
-- Update the Stage 2 handler to triple instead of double:
CREATE OR REPLACE FUNCTION lang_b_handler()
RETURNS language_handler AS $$
  RETURN arguments[0].split("DOUBLE_RET ").join("return 3 * ");
$$ LANGUAGE pseudolang;

-- No need to recreate test_chained(x)! It automatically recompiles:
SELECT test_chained(21);
-- Result: 63
```

---

## 6. Compile-Time Catalog Introspection via SPI

Because `plv8_transpile_src` runs your handler inside an active SPI connection protected by `HandlerExecutionScope`, your handler can call `plv8.execute(...)` at compile time to inspect PostgreSQL system catalogs (`pg_proc`, `pg_type`, `pg_attribute`, `pg_class`).

Here is the standard pattern used by `plv8_plpgsql_handler` and `plv8_sqltsm_handler` to inspect `OUT` parameters, `INOUT` parameters, `RETURNS TABLE` columns, and `proretset`:

```javascript
function inspectFunctionCatalog(meta) {
  if (!meta || meta.is_do_block || !(meta.fn_oid > 0)) {
    return null;
  }
  const rows = plv8.execute(
    "SELECT p.proname, p.proretset, p.prorettype::oid AS prorettype, " +
    "pg_catalog.format_type(p.prorettype, NULL) AS rettype_sql, " +
    "t.typtype AS rettype_typtype, " +
    "ARRAY(SELECT pg_catalog.format_type(u, NULL) FROM unnest(p.proargtypes) u) AS in_arg_types, " +
    "CASE WHEN p.proallargtypes IS NOT NULL " +
    "  THEN ARRAY(SELECT pg_catalog.format_type(u, NULL) FROM unnest(p.proallargtypes) u) " +
    "  ELSE NULL END AS all_arg_types, " +
    "p.proargmodes::text[] AS proargmodes, " +
    "p.proargnames AS proargnames, " +
    "p.prokind AS prokind " +
    "FROM pg_catalog.pg_proc p " +
    "LEFT JOIN pg_catalog.pg_type t ON t.oid = p.prorettype " +
    "WHERE p.oid = $1",
    [meta.fn_oid]
  );
  return (rows && rows.length > 0) ? rows[0] : null;
}
```

> **Tip:** Wrapping your handler's compiler in a lazy singleton on `globalThis` (e.g., `if (typeof globalThis.__my_lang_compile !== 'function') { globalThis.__my_lang_compile = ...; }`) avoids re-initializing parser tables on every `CREATE FUNCTION`.

---

## 7. Raw Binary Custom Base Types (`meta.pass_user_types_as_bytes`)

When you define a custom PostgreSQL base type (for example via `CREATE TYPE` or `pgtle.create_base_type_if_not_exists`), PostgreSQL assigns it `typtype = 'b'` and `typcategory = 'U'` (`TYPCATEGORY_USER`).

By default, `pl/v8` converts `TYPCATEGORY_USER` values to JavaScript strings using the type's text output function and parses returned strings using the type's text input function.

If your language handler sets:
```javascript
if (meta) {
  meta.pass_user_types_as_bytes = true;
}
```
then for that compiled function:
1. **Input Arguments (`TYPCATEGORY_USER`)**: Passed to your generated JavaScript function as a `Uint8Array` containing the raw varlena payload bytes (`VARSIZE_ANY_EXHDR(datum)` bytes, with the 4-byte `VARHDRSZ` header stripped).
2. **Return Value (`TYPCATEGORY_USER`)**: Your generated JavaScript function must return a `Uint8Array` (or `ArrayBufferView` / `ArrayBuffer`).
3. **Fixed-Size Type Enforcement**: If the custom base type was created with a fixed `internallength > 0` (so `pg_type.typlen = internallength + VARHDRSZ`), `meta.rettype_len` and `meta.argtype_lens[i]` report `internallength`. When your function returns a `Uint8Array`, `pl/v8` verifies that `byte_len + VARHDRSZ == type->len`; if the buffer length does not match, `pl/v8` raises `SQLSTATE 22000` (`ERRCODE_DATA_EXCEPTION`):
   ```
   ERROR:  type <typename> is defined as fixed-size <N>, but returned buffer has internal length <M>
   ```

### Complete Working Example: Custom 2D Integer Vector (`vec2i`) with Binary Language Handler

```sql
CREATE EXTENSION IF NOT EXISTS pg_tle;
CREATE EXTENSION IF NOT EXISTS plv8;

-- 1. Create a binary language handler that enables pass_user_types_as_bytes
CREATE OR REPLACE FUNCTION bin_lang_handler()
RETURNS language_handler AS $$
  const src = arguments[0];
  const meta = arguments[1];
  if (meta) {
    meta.pass_user_types_as_bytes = true;
  }
  return src;
$$ LANGUAGE plv8;
REVOKE ALL ON FUNCTION bin_lang_handler() FROM PUBLIC;

CREATE TRUSTED LANGUAGE bin_lang
  HANDLER bin_lang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;

-- 2. Define a custom 8-byte base type (two int32s: x, y) using pg_tle
SELECT pgtle.create_shell_type_if_not_exists('public', 'bin_vec2i');

CREATE OR REPLACE FUNCTION public.bin_vec2i_in(s text)
RETURNS bytea AS $$
  const m = String(s).trim().match(/^\(\s*(-?\d+)\s*,\s*(-?\d+)\s*\)$/);
  if (!m) {
    const err = new Error('invalid input syntax for type bin_vec2i: "' + s + '"');
    err.sqlerrcode = '22P02';
    throw err;
  }
  const buf = new ArrayBuffer(8);
  const dv = new DataView(buf);
  dv.setInt32(0, parseInt(m[1], 10), true);
  dv.setInt32(4, parseInt(m[2], 10), true);
  return new Uint8Array(buf);
$$ LANGUAGE plv8 IMMUTABLE STRICT;

CREATE OR REPLACE FUNCTION public.bin_vec2i_out(b bytea)
RETURNS text AS $$
  const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  return '(' + dv.getInt32(0, true) + ',' + dv.getInt32(4, true) + ')';
$$ LANGUAGE plv8 IMMUTABLE STRICT;

SELECT pgtle.create_base_type_if_not_exists(
  'public', 'bin_vec2i',
  'public.bin_vec2i_in(text)'::regprocedure,
  'public.bin_vec2i_out(bytea)'::regprocedure,
  8, 'int4', 'plain'
);

-- 3. Write a fast binary addition function in bin_lang that operates directly on Uint8Array bytes!
CREATE OR REPLACE FUNCTION bin_vec2i_add(a bin_vec2i, b bin_vec2i)
RETURNS bin_vec2i AS $$
  const dvA = new DataView(a.buffer, a.byteOffset, a.byteLength);
  const dvB = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const out = new Uint8Array(8);
  const dvOut = new DataView(out.buffer);
  dvOut.setInt32(0, (dvA.getInt32(0, true) + dvB.getInt32(0, true)) | 0, true);
  dvOut.setInt32(4, (dvA.getInt32(4, true) + dvB.getInt32(4, true)) | 0, true);
  return out;
$$ LANGUAGE bin_lang IMMUTABLE STRICT;

SELECT bin_vec2i_add('(10,-25)'::bin_vec2i, '(30,40)'::bin_vec2i);
-- Result: (40,15)
```

---

## 8. Structured PostgreSQL Error Reporting & Diagnostics

Both your language handler (at compile time) and the JavaScript code it generates (at runtime) can throw and catch rich PostgreSQL errors with full `ErrorData` diagnostic properties.

### 8.1 Throwing Errors with Full PostgreSQL Diagnostics

Create a standard JavaScript `new Error(message)` and attach any of the following properties before throwing:

```javascript
const err = new Error('duplicate key value violates unique constraint "users_pkey"');
err.sqlerrcode      = '23505';        // 5-char SQLSTATE (or err.code = '23505')
err.detail          = 'Key (id)=(42) already exists.';
err.hint            = 'Use ON CONFLICT DO UPDATE instead.';
err.schema_name     = 'public';
err.table_name      = 'users';
err.column_name     = 'id';
err.datatype_name   = 'int4';
err.constraint_name = 'users_pkey';
throw err;
```

> **Note:** If neither `err.sqlerrcode` nor `err.code` is set on a thrown JS `Error`, PostgreSQL reports `SQLSTATE XX000` (`ERRCODE_INTERNAL_ERROR`). Always set `err.sqlerrcode` (e.g. `'42601'` for syntax errors, `'P0001'` for `raise_exception`, `'22012'` for `division_by_zero`) when implementing a language runtime.

### 8.2 Catching SPI Errors and Subtransaction Rollback

When generated JavaScript executes SQL via `plv8.execute(...)` and the query fails, PostgreSQL aborts the current subtransaction unless wrapped in `plv8.subtransaction(function() { ... })`.

To implement language-level exception handling (such as PL/pgSQL `BEGIN ... EXCEPTION WHEN ... END`, T-SQL `BEGIN TRY ... BEGIN CATCH`, or Python `with plpy.subtransaction():`):

```javascript
try {
  plv8.subtransaction(function() {
    plv8.execute('INSERT INTO users VALUES ($1)', [userId]);
  });
} catch (err) {
  // All 13 PostgreSQL diagnostic fields are populated on `err`:
  // err.message, err.sqlerrcode, err.code, err.detail, err.hint, err.context,
  // err.schema_name, err.table_name, err.column_name, err.datatype_name,
  // err.constraint_name, err.internalquery, err.internalpos
  if (err.sqlerrcode === '23505') {
    plv8.elog(NOTICE, 'Caught unique_violation on constraint: ' + err.constraint_name);
  } else {
    throw err; // Re-throw preserving all PostgreSQL diagnostics
  }
}
```

---

## 9. Using ES Modules, CommonJS `require()`, and Shared WASM Caching

### 9.1 Registering Modules & Packages in `plv8.packages` and `plv8.modules`

`pl/v8` provides two catalog tables for storing reusable JavaScript and WebAssembly modules:
- **`plv8.packages`**: Stores versioned package metadata (`PRIMARY KEY (name, version)`, `entry_point TEXT NOT NULL DEFAULT 'index.js'`, `dependencies JSONB DEFAULT '{}'`).
- **`plv8.modules`**: Stores standalone modules (`package_name IS NULL`, unique on `path`) and package modules (`package_name IS NOT NULL`, unique on `(package_name, package_version, path)`).

```sql
-- 1. Register a standalone ES module in plv8.modules
INSERT INTO plv8.modules (path, source, is_trusted)
VALUES (
  'math_utils',
  'export function clamp(x, lo, hi) { return Math.min(Math.max(x, lo), hi); }
   export default { clamp };',
  true
);

-- 2. Register a versioned package in plv8.packages + plv8.modules
INSERT INTO plv8.packages (name, version, entry_point)
VALUES ('@acme/formatter', '1.2.0', 'index.js');

INSERT INTO plv8.modules (path, package_name, package_version, source, is_trusted)
VALUES (
  'index.js',
  '@acme/formatter',
  '1.2.0',
  'export function banner(msg) { return "[" + import.meta.url + "] " + String(msg).toUpperCase(); }',
  true
);
```

> **Automatic Cache Invalidation:** Modifying `plv8.modules` or `plv8.packages` (`INSERT`, `UPDATE`, `DELETE`, `TRUNCATE`) fires the `plv8_modules_changed` trigger, which bumps the shared-memory module epoch across all backends and automatically invalidates cached functions that depend on TLE modules.

### 9.2 Generating ES Module `import`s and `export default` from a Handler

A language handler can emit top-level `import` declarations or `export default` in the JavaScript code it returns! `pl/v8` automatically hoists top-level `import` statements and compiles the function through V8's ES Module pipeline:

```sql
CREATE FUNCTION test_esm_in_plv8(val int)
RETURNS text AS $$
  import { clamp } from 'math_utils';
  import { banner } from '@acme/formatter@^1.0.0';
  return banner('clamped=' + clamp(val, 0, 100));
$$ LANGUAGE plv8;

SELECT test_esm_in_plv8(250);
-- Result: [plv8:@acme/formatter@1.2.0/index.js] CLAMPED=100
```

You can also load modules synchronously via CommonJS `require('<specifier>')` (or `plv8.require('<specifier>')`) and asynchronously via `await import('<specifier>')`.

### 9.3 Module Governance: `is_trusted` and `requires` Allowlists

- **`is_trusted = false`**: Only PostgreSQL superusers can import or `require()` an untrusted module. If an unprivileged role attempts to load it, `pl/v8` throws:
  `Error: permission denied for untrusted module "<name>"`
- **`requires TEXT[]`**: When a module row in `plv8.modules` has a non-NULL `requires` array (e.g. `requires = ARRAY['math_utils']`), that module is restricted to importing only the listed specifiers. Any attempt to import an unlisted module throws:
  `Error: plv8 module "<target>" is not listed in requires of "<referrer>"`

### 9.4 Cross-Backend Shared Memory WASM Compilation (`plv8.compile_wasm_cached`)

If your language handler embeds or loads a WebAssembly module (such as a compiler, VM, or native library), use `plv8.compile_wasm_cached(wasm_bytes)` instead of `new WebAssembly.Module(wasm_bytes)`:

```javascript
// wasm_bytes: Uint8Array or ArrayBuffer containing a valid \0asm binary
const wasmModule = plv8.compile_wasm_cached(wasmBytes);
const wasmInstance = new WebAssembly.Instance(wasmModule, imports);
```

- When `plv8` is preloaded via `shared_preload_libraries`, the first backend to call `plv8.compile_wasm_cached(wasmBytes)` compiles the `.wasm` bytes with V8 and stores the serialized native machine code in PostgreSQL shared memory (`DSA` + `dshash`, keyed by `{ db_oid, role_oid, sha256(wire_bytes) }` and sized by the `PGC_POSTMASTER` GUC `plv8.wasm_cache_size`, default `64` MB).
- Every subsequent backend in the same database and role deserializes the pre-compiled machine code directly from shared memory (`hits` counter in `SELECT plv8_info()->0->'wasm_cache'`).

---

## 10. Using the Built-In PL/v8-Hosted Languages

When you run `CREATE EXTENSION plv8;`, three complete language families are installed out of the box (`plpgsql_v8`, `sql/tsm` / `sql/psm`, and `pl/fullpython+v8`). When combined with `pg_tle`, a fourth language (`pl/v8-wasm`) is also available.

### 10.1 `plpgsql_v8` (PL/pgSQL on V8)

`plpgsql_v8` compiles standard PL/pgSQL syntax into V8 JavaScript via `plv8_plpgsql_handler()`.

```sql
CREATE OR REPLACE FUNCTION fib_plpgsql_v8(n int)
RETURNS int AS $$
DECLARE
  a int := 0;
  b int := 1;
  tmp int;
BEGIN
  FOR i IN 1..n LOOP
    tmp := a + b;
    a := b;
    b := tmp;
  END LOOP;
  RETURN a;
END;
$$ LANGUAGE plpgsql_v8 IMMUTABLE STRICT;

SELECT fib_plpgsql_v8(10);
-- Result: 55

-- Inspect the generated JavaScript code using plv8.compile_plpgsql():
SELECT plv8.compile_plpgsql($$
DECLARE
  x int := 42;
BEGIN
  RETURN x + 1;
END;
$$);
```

### 10.2 `sql/tsm` (T-SQL) and `sql/psm` (SQL/PSM)

`plv8_sqltsm_handler()` backs four language names: `"sql/tsm"`, `sqltsm_v8`, `"sql/psm"`, and `sqlpsm_v8`.

#### T-SQL Example (`LANGUAGE "sql/tsm"`)
```sql
CREATE OR REPLACE FUNCTION tsql_discount(@price numeric(10,2), @tier varchar(20))
RETURNS numeric(10,2) AS $$
BEGIN
  DECLARE @rate numeric(10,2) = 0.00;
  BEGIN TRY
    IF @price < 0
      THROW 50001, 'Price cannot be negative', 1;
    SET @rate = IIF(@tier = 'GOLD', 0.20, 0.05);
    RETURN @price * (1.00 - @rate);
  END TRY
  BEGIN CATCH
    RETURN 0.00;
  END CATCH
END
$$ LANGUAGE "sql/tsm";

SELECT tsql_discount(100.00, 'GOLD');
-- Result: 80.00
```

#### SQL/PSM Example (`LANGUAGE "sql/psm"`)
```sql
CREATE OR REPLACE FUNCTION psm_safe_div(a int, b int)
RETURNS int AS $$
BEGIN ATOMIC
  DECLARE result int DEFAULT -1;
  DECLARE EXIT HANDLER FOR SQLSTATE '22012'
    BEGIN
      RETURN -999;
    END;
  IF b = 0 THEN
    SIGNAL SQLSTATE '22012' SET MESSAGE_TEXT = 'division by zero in SQL/PSM';
  END IF;
  SET result = a / b;
  RETURN result;
END
$$ LANGUAGE "sql/psm";

SELECT psm_safe_div(20, 4), psm_safe_div(20, 0);
-- Result: 5, -999
```

You can inspect the generated JavaScript for either dialect via `SELECT plv8.compile_sqltsm(src, meta)` or `SELECT plv8.compile_sqlpsm(src, meta)`.

### 10.3 `pl/fullpython+v8` (Python 3.15 + WASM on V8)

`plv8_fullpython_handler()` (registered under `"pl/fullpython+v8"`, `plfullpython_v8`, `"pl/python+v8"`, and `plpython_v8`) runs Python 3.15 inside V8 using an embedded WebAssembly runtime (`python315.wasm`) with warm linear-memory snapshots (`mp_snapshot_restore`) and a Binary Slot Buffer SPI bridge.

```sql
CREATE OR REPLACE FUNCTION py_analyze_orders(min_amount int)
RETURNS text AS $$
import ujson
import postgres

rows = plpy.execute(f"SELECT generate_series(1, 5) AS id, generate_series(1, 5) * 10 AS amount")
filtered = [r["amount"] for r in rows if r["amount"] >= min_amount]
return ujson.dumps({"count": len(filtered), "sum": sum(filtered)})
$$ LANGUAGE "pl/fullpython+v8";

SELECT py_analyze_orders(30);
-- Result: {"count":3,"sum":120}
```

#### Installing Reusable Python & C/WASM Modules for `pl/fullpython+v8`
`pl/fullpython+v8` includes its own module registry table `plv8.plpython_v8_modules` and helper functions:
- `SELECT plpython_v8_install_module(module_name, source_code, version DEFAULT '1.0.0', is_trusted DEFAULT true);`
- `SELECT plpython_v8_install_wasm_module(module_name, wasm_binary, abi_spec DEFAULT NULL, version DEFAULT '1.0.0', is_trusted DEFAULT true);`
- `SELECT plpython_v8_uninstall_module(module_name, version DEFAULT NULL);`
- `SELECT plpython_v8_reload_modules();`

### 10.4 `pl/v8-wasm` (WebAssembly Functions, Types & Operators)

When `pg_tle` and `plv8` are installed, `pgtle.bootstrap_wasm_engine()` and `pgtle.install_wasm_extension()` provide the `"pl/v8-wasm"` procedural language backed by `public.plv8_wasm_handler()`.

#### Method A: Standalone `LANGUAGE "pl/v8-wasm"` Function with JSON Configuration

A `"pl/v8-wasm"` function body is a **JSON configuration object** specifying:
- `"export"` *(required)*: Name of the exported WebAssembly function to call.
- Exactly one of:
  - `"wasm"`: Inline Base64-encoded `.wasm` binary, **or**
  - `"module"`: Name of a WASM module registered in `plv8.modules`, `pgtle.modules`, or `public.plv8_wasm_modules` (plus optional `"version"`).
- Optional marshaling overrides: `"arg_types"`, `"arg_mode"` (`"auto"`, `"ptr_len"`, `"ptr_only"`, `"cstring"`), `"return_type"`, `"return_mode"` (`"auto"`, `"ptr_len"`, `"fixed"`, `"cstring"`), `"return_length"`, `"alloc_export"`, `"free_export"`, `"scratch_offset"`.

```sql
CREATE EXTENSION IF NOT EXISTS pg_tle;
CREATE EXTENSION IF NOT EXISTS plv8;

-- Ensure the pl/v8-wasm language and runtime polyfills are bootstrapped:
SELECT pgtle.bootstrap_wasm_engine();

-- Define a function backed by an inline Base64 WebAssembly binary that exports `add(i32, i32) -> i32`:
CREATE OR REPLACE FUNCTION wasm_inline_add(a int4, b int4)
RETURNS int4 LANGUAGE "pl/v8-wasm" IMMUTABLE STRICT AS $$
{
  "wasm": "AGFzbQEAAAABBwFgAn9/AX8DAgEABwcBA2FkZAAACgkBBwAgACABags=",
  "export": "add"
}
$$;

SELECT wasm_inline_add(20, 22);
-- Result: 42
```

#### Method B: Packaging a Complete WASM Extension via `pgtle.install_wasm_extension()`

`pgtle.install_wasm_extension(name, version, description, wasm_bytecode, manifest)` registers a `.wasm` binary in `pgtle.modules`, `plv8.modules`, and `public.plv8_wasm_modules`, and automatically generates SQL functions, custom binary base types (`TYPCATEGORY_USER`), and operators from a declarative JSON manifest:

```sql
SELECT pgtle.install_wasm_extension(
  'my_wasm_ext',
  '1.0.0',
  'Example WASM extension',
  decode('0061736d0100000001070160027f7f017f030201000707010361646400000a09010700200020016a0b', 'hex'),
  '{
    "functions": [
      {
        "name": "my_wasm_add",
        "args": ["int4", "int4"],
        "returns": "int4",
        "export": "add",
        "volatility": "IMMUTABLE"
      }
    ]
  }'::jsonb
);

CREATE EXTENSION my_wasm_ext;
SELECT my_wasm_add(100, 23);
-- Result: 123
```

### 10.5 `pl-pgq` (ISO SQL/PGQ `CREATE PROPERTY GRAPH` & `GRAPH_TABLE` in Pure JS/WASM)

`pl-pgq` is a 100% pure SQL/JS/WASM extension built on top of `pl/v8` v4.0 with **zero C files**. It registers `LANGUAGE "CREATE PROPERTY GRAPH"` (and `property_graph`, `sqlpgq_v8`) for declaring property graphs and uses `CREATE DOMAIN pgq_pattern AS plv8_source;` + `SUPPORT plv8_call_support` so `GRAPH_TABLE(...)` infers its output columns at parse time and inlines into native PostgreSQL joins at plan time.

---

## 11. Dynamic Table Functions (`RETURNS SETOF record`), Parse-Time Column Inference (`plv8_source`), and Planner Inlining (`plv8_call_support`)

`pl/v8` v4.0 includes a built-in **Dynamic Table Macro & Planner `prosupport` Subsystem** that can be used by both **plain `LANGUAGE plv8` functions** and **compiled-to-JS languages (`pl/<lang>`)**.

It solves three longstanding PostgreSQL limitations for non-C extensions:
1. **Runtime `RETURNS SETOF record` introspection (`plv8.expected_columns()` & `plv8.return_query()`)**: Inspect the caller's `AS (col1 type1, ...)` column list at runtime and stream SPI query results directly into the SRF tuplestore without per-row JS object conversion overhead.
2. **Parse-Time Column Inference (`plv8_source` & `plv8.register_column_inference()`)**: Allow callers to write `SELECT * FROM my_func('...')` **without** requiring an `AS (...)` column definition list!
3. **Planner Subquery Inlining (`SUPPORT plv8_call_support` & `plv8.register_support()`)**: Inline `FROM my_func('...')` into a native PostgreSQL `RTE_SUBQUERY` during query planning so outer `WHERE` predicates push down into B-tree indexes and `LATERAL` references to preceding `FROM` tables correlate natively.

### 11.1 Plain `pl/v8` Functions Returning `SETOF record` (`plv8.expected_columns()` & `plv8.return_query()`)

Any `LANGUAGE plv8` (or `LANGUAGE pl/<lang>`) function declared as `RETURNS SETOF record` can inspect the caller's expected columns via `plv8.expected_columns()` and stream an SPI query directly into the result tuplestore via `plv8.return_query(sql [, args])`:

```sql
CREATE TABLE products (id int PRIMARY KEY, category text, price numeric(10,2));
INSERT INTO products VALUES (1, 'books', 19.99), (2, 'books', 29.50), (3, 'games', 49.00);

CREATE OR REPLACE FUNCTION filter_products(cat text, min_price numeric)
RETURNS SETOF record
LANGUAGE plv8 STABLE AS $$
  // Inspect the caller's `AS (col1 type1, ...)` specification:
  // [{ name, type_oid, typmod, collation_oid, type_sql }, ...]
  const cols = plv8.expected_columns();
  const projection = cols.map(c => `${c.name}::${c.type_sql}`).join(', ');
  plv8.return_query(
    `SELECT ${projection} FROM products WHERE category = $1 AND price >= $2 ORDER BY id`,
    [cat, min_price]
  );
$$;

SELECT * FROM filter_products('books', 15.00) AS (id int, price numeric(10,2));
--  id | price
-- ----+-------
--   1 | 19.99
--   2 | 29.50
```

### 11.2 Eliminating `AS (...)` and Inlining into the Query Plan (`plv8_source` + `plv8_call_support`)

By declaring your query/pattern parameter as `plv8_source` (or a domain over `plv8_source`, such as `CREATE DOMAIN my_query AS plv8_source;`) and attaching `SUPPORT plv8_call_support`, you can register JavaScript (or SQL) hooks for parse-time column inference and plan-time SQL inlining:

```sql
DO $$
  // Helper that parses "col1:type1=expr1, col2:type2=expr2 | FROM ..."
  function parseMacro(spec) {
    const [colPart, fromPart = ''] = String(spec).split('|').map(s => s.trim());
    const cols = colPart.split(',').map(item => {
      const [decl, expr] = item.trim().split('=');
      const [name, typeName] = decl.trim().split(':');
      const rows = plv8.execute(
        `SELECT oid::int AS type_oid FROM pg_catalog.pg_type WHERE typname = $1`,
        [typeName.trim()]
      );
      return {
        name: name.trim(),
        type_name: typeName.trim(),
        type_oid: (rows && rows.length > 0) ? rows[0].type_oid : 25,
        typmod: -1,
        collation_oid: 0,
        expr: expr.trim()
      };
    });
    return { cols, fromPart };
  }

  // 1. Parse-time hook: infer output columns when no `AS (...)` list is written
  plv8.register_column_inference('js_table_macro', function(req) {
    // req = { func_name, query_string, source_arg, first_arg, has_coldeflist }
    const parsed = parseMacro(req.source_arg);
    return {
      has_coldeflist: req.has_coldeflist,
      columns: parsed.cols
    };
  });

  // 2. Plan-time prosupport hook: compile into SQL for subquery inlining
  plv8.register_support('js_table_macro', function(req) {
    // req = {
    //   request_type: 'simplify', func_oid, func_name, is_from_clause,
    //   has_coldeflist, first_arg, source_arg, params,
    //   expected_columns: [{ name, type_oid, typmod, collation_oid, type_sql }, ...],
    //   lateral_env: [{ alias, rtindex, columns: [{ name, attno, type_oid, ... }] }, ...]
    // }
    const parsed = parseMacro(req.source_arg);
    const outCols = (req.expected_columns && req.expected_columns.length > 0)
      ? req.expected_columns
      : parsed.cols;

    const selectList = parsed.cols.map((c, idx) => {
      const target = outCols[idx] || c;
      return `(${c.expr})::${target.type_sql || c.type_name} AS "${target.name}"`;
    }).join(', ');

    // Optional LATERAL outer Var correlation via $1..$N
    const lateralVars = [];
    const outerMatch = parsed.fromPart.match(/outer\.([a-zA-Z0-9_]+)/);
    if (outerMatch && req.lateral_env && req.lateral_env.length > 0) {
      const outerRel = req.lateral_env[0];
      const outerCol = outerRel.columns.find(c => c.name === outerMatch[1]);
      if (outerCol) {
        lateralVars.push({
          rtindex: outerRel.rtindex,
          attno: outerCol.attno,
          type_oid: outerCol.type_oid,
          typmod: outerCol.typmod,
          collation_oid: outerCol.collation_oid
        });
      }
    }

    const fromClause = parsed.fromPart.replace(/outer\.[a-zA-Z0-9_]+/g, '$1');
    return {
      sql: `SELECT ${selectList} ${fromClause}`,
      columns: outCols,
      lateral_vars: lateralVars
    };
  });
$$ LANGUAGE plv8;

-- Note: Define overloaded signatures rather than DEFAULT parameters when
-- single-column inference without AS (...) is desired.
CREATE OR REPLACE FUNCTION js_table_macro(spec plv8_source)
RETURNS SETOF record
LANGUAGE plv8 STABLE SUPPORT plv8_call_support AS $$
  const cols = plv8.expected_columns();
  const [colPart, fromPart = ''] = String(spec).split('|').map(s => s.trim());
  const selectList = colPart.split(',').map((item, idx) => {
    const [, expr] = item.trim().split('=');
    return `(${expr.trim()})::${cols[idx].type_sql} AS "${cols[idx].name}"`;
  }).join(', ');
  plv8.return_query(`SELECT ${selectList} ${fromPart}`);
$$;

-- Works WITHOUT `AS (...)`!
SELECT * FROM js_table_macro('item_id:int4=id, item_price:numeric=price | FROM products WHERE category = ''books'' ORDER BY id');

-- Works WITH `AS (...)`!
SELECT * FROM js_table_macro('item_id:int4=id, item_price:numeric=price | FROM products WHERE category = ''games''')
  AS (game_id int, cost numeric(10,2));

-- Inlines directly into an Index Scan on `products_pkey`!
EXPLAIN (COSTS OFF)
SELECT * FROM js_table_macro('item_id:int4=id, item_price:numeric=price | FROM products')
WHERE item_id = 2;
--                   QUERY PLAN
-- ----------------------------------------------
--  Index Scan using products_pkey on products
--    Index Cond: (id = 2)
```

### 11.3 Using Persistent SQL Catalog Hooks for Multi-Session Extensions

In addition to passing a JavaScript `Function` to `plv8.register_column_inference()` and `plv8.register_support()`, extensions that should work immediately in fresh database backends without running an initialization `DO` block can either:
1. Pass the name of a SQL function:
   ```javascript
   plv8.register_column_inference('my_macro', 'my_macro_infer_columns');
   plv8.register_support('my_macro', 'my_macro_support');
   ```
2. Or simply define SQL functions following `pl/v8`'s naming convention (`<func_name>_infer_columns(query_ctx text, source_arg text) RETURNS text` and `<func_name>_support(first_arg text, source_arg text, expected_cols_json text, lateral_env_json text, params_json text) RETURNS text`), which `plv8_source_in` and `plv8_call_support` discover automatically in `pg_proc`!

### 11.4 Stamping Fixed Output Columns on Custom `pl/<lang>` Functions (`plv8.set_function_columns()`)

If a custom `pl/<lang>` language compiles functions declared as `RETURNS SETOF record` whose output columns are determined at `CREATE FUNCTION` time (from the function body), the language handler or setup script can call `plv8.set_function_columns(func_name, columns)`:

```javascript
plv8.set_function_columns('top_books_macro', [
  { name: 'book_id', type_oid: 23 },      // int4
  { name: 'book_price', type_oid: 1700 }  // numeric
]);
```

---

## 12. Best Practices, Security & Troubleshooting

### 11.1 Security Checklist for Language Authors
1. **Always `REVOKE ALL ON FUNCTION <handler>() FROM PUBLIC;`**: Users only need `USAGE` on the language (`GRANT USAGE ON LANGUAGE <lang> TO ...`), never direct `EXECUTE` on the handler function itself.
2. **Choose `CREATE TRUSTED LANGUAGE` vs. `CREATE LANGUAGE` Carefully**:
   - Mark your language `TRUSTED` only if non-superusers should be allowed to create functions and `DO` blocks in it.
   - Even in a `TRUSTED` language, `pl/v8` enforces per-role V8 isolate contexts, `plv8.memory_limit`, `plv8.execution_timeout`, and `is_trusted` checks on `plv8.modules` / `pgtle.modules`.
3. **Use Parameterized SPI Queries (`$1`, `$2`)**: When your compiler or runtime executes SQL via `plv8.execute(sql, args)`, always pass user values via the parameter array rather than string concatenation.

### 11.2 Troubleshooting Common Errors

| Error Message | SQLSTATE | Cause & Resolution |
|---|---|---|
| `language handler function cannot be called directly` | `0A000` (`feature_not_supported`) | You called `SELECT my_lang_handler();` in SQL. Language handlers are invoked automatically by `pl/v8` when compiling functions in that language. To expose a debug compiler helper, create a separate `RETURNS text` function (like `plv8.compile_plpgsql(text, jsonb)`). |
| `language handler must return a JavaScript source string` | `XX000` (`internal_error`) | Your language handler returned `undefined`, `null`, or a non-string object instead of a string of JavaScript code. Ensure all code paths in your handler return a `string`. |
| `language handler chain is cyclical or exceeds maximum depth` | `54000` (`program_limit_exceeded`) | A language handler's `prolang` forms a cycle or chains more than 32 languages deep (`PLV8_MAX_LANG_HANDLER_DEPTH = 32`). Ensure the chain terminates at `LANGUAGE plv8`. |
| `cannot convert JS value to custom user type: expected Uint8Array or ArrayBuffer` | `42804` (`datatype_mismatch`) | Your handler set `meta.pass_user_types_as_bytes = true` on a function returning a `TYPCATEGORY_USER` base type, but the function returned a non-buffer JS value. Return a `Uint8Array` or `ArrayBuffer`. |
| `type <T> is defined as fixed-size <N>, but returned buffer has internal length <M>` | `22000` (`data_exception`) | A function with `meta.pass_user_types_as_bytes = true` returned a `Uint8Array` whose `byteLength` (`M`) does not match the custom base type's `internallength` (`N`). |
| `permission denied for untrusted module "<name>"` | `XX000` | A non-superuser role attempted to `import` or `require()` a module in `plv8.modules` / `pgtle.modules` with `is_trusted = false`. |
| `plv8 module "<A>" is not listed in requires of "<B>"` | `XX000` | Module `<B>` has a non-NULL `requires` allowlist in `plv8.modules` that does not include `<A>`. Add `<A>` to `<B>`'s `requires` array. |
