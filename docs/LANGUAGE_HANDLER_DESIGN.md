# PL/v8 Language Handler Architecture & Design Document

**Status:** Implemented (`plv8` 3.2.3)  
**Target Component:** `third_party/pgext/plv8` (and `third_party/pgext/pg_tle` WASM integration)  
**Primary Source Files:** `plv8.h`, `plv8.cc`, `plv8_func.cc`, `plv8_type.cc`, `plv8_guc.cc`, `plv8.sql.common`

---

## Table of Contents

1. [Executive Summary & Motivation](#1-executive-summary--motivation)
2. [Goals and Non-Goals](#2-goals-and-non-goals)
3. [High-Level Architecture](#3-high-level-architecture)
4. [Language Handler Discovery, Chaining & Transpilation Pipeline](#4-language-handler-discovery-chaining--transpilation-pipeline)
5. [Compilation Cache & Dependency Invalidation](#5-compilation-cache--dependency-invalidation)
6. [Raw Custom Base Type Binary Protocol (`TYPCATEGORY_USER`)](#6-raw-custom-base-type-binary-protocol-typcategory_user)
7. [Structured Error Propagation & PostgreSQL Diagnostics](#7-structured-error-propagation--postgresql-diagnostics)
8. [Trusted Language Extension (TLE) Module System & WASM Native Cache](#8-trusted-language-extension-tle-module-system--wasm-native-cache)
9. [Security & Multi-Tenancy Model](#9-security--multi-tenancy-model)
10. [Case Studies: Built-In & Ecosystem Language Handlers](#10-case-studies-built-in--ecosystem-language-handlers)
11. [Performance Characteristics & Trade-Offs](#11-performance-characteristics--trade-offs)

---

## 1. Executive Summary & Motivation

PostgreSQL allows extensions to register new procedural languages (`CREATE [TRUSTED] LANGUAGE ... HANDLER ...`). Traditionally, implementing a new procedural language in PostgreSQL requires writing a C shared library that directly interfaces with PostgreSQL's internal C APIs (`FunctionCallInfo`, `SPI_connect`, `Datum` conversion, `MemoryContext` management, `PG_TRY`/`PG_CATCH` `sigsetjmp` exception handling, and `pg_proc` syscache invalidation).

In managed cloud database environments (such as Cloud SQL and AlloyDB) and Trusted Language Extensions (`pg_tle`), deploying arbitrary C shared libraries is restricted for security and stability reasons. However, `pl/v8` already provides a battle-tested, memory-limited, per-role isolated V8 JavaScript and WebAssembly execution engine with full Server Programming Interface (SPI) integration.

The **PL/v8 Language Handler Architecture** turns `pl/v8` into a **meta-runtime and compiler target** for PostgreSQL procedural languages. Instead of writing a C extension, a language author writes a **language handler function in `plv8` (or in another `plv8`-backed language)** with SQL return type `language_handler`:

```sql
CREATE FUNCTION my_lang_handler() RETURNS language_handler AS $$
  const [src, meta] = arguments;
  return transpileToJavaScript(src, meta);
$$ LANGUAGE plv8;

CREATE TRUSTED LANGUAGE my_lang
  HANDLER my_lang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;
```

When PostgreSQL invokes a function, trigger, `DO` block, or `CREATE FUNCTION` validator in `my_lang`, `pl/v8` intercepts the call in C++, invokes `my_lang_handler(src, meta)` inside a sandboxed `HandlerExecutionScope` with full SPI access, compiles the returned JavaScript (or WebAssembly wrapper) into a cached `v8::Persistent<v8::Function>`, and executes it using `pl/v8`'s native argument/return marshaling and SPI bridge.

This architecture powers four production language runtimes in the repository:
- **`plpgsql_v8`** (`plv8_plpgsql_handler` in `plv8.sql.common`): A full PL/pgSQL compiler targeting V8 JavaScript.
- **`sql/tsm` / `sql/psm`** (`plv8_sqltsm_handler` in `plv8.sql.common`): A unified T-SQL (`sql/tsm`) and SQL/PSM (`sql/psm`) compiler targeting V8 JavaScript.
- **`pl/fullpython+v8`** (`plv8_fullpython_handler` + `python315_wasm_bundle` in `plv8.sql.common`): A Python 3.15 runtime backed by an embedded WebAssembly module (`python315.wasm`), warm linear-memory snapshots, a zero-copy Binary Slot Buffer SPI bridge, and a Python 3.15 AST compiler/interpreter.
- **`pl/v8-wasm`** (`public.plv8_wasm_handler` in `pg_tle--1.5.3--1.6.0.sql`): A declarative WebAssembly procedural language for executing compiled Rust/C/C++/Go `.wasm` binaries as PostgreSQL functions, operators, and custom base type I/O routines.

---

## 2. Goals and Non-Goals

### 2.1 Goals
1. **Zero-C Language Authoring:** Allow new trusted or untrusted PostgreSQL procedural languages to be defined purely in SQL + JavaScript/WebAssembly without compiling or loading new C shared objects.
2. **Full PostgreSQL Entry-Point Coverage:** Transparently support scalar functions, composite/record functions, set-returning functions (SRFs via `plv8.return_next`), `OUT`/`INOUT`/`VARIADIC` parameters, row-level triggers (`NEW`, `OLD`, `TG_*`), `DO` anonymous code blocks (`plv8_inline_handler`), and `CREATE FUNCTION` syntax validation (`plv8_call_validator`).
3. **Rich Compile-Time Introspection (`meta`) & SPI Access:** Pass a structured `meta` object containing function OID, name, return type OID/category/length, and argument names/OIDs/categories/lengths to the handler, while allowing the handler itself to run `plv8.execute(...)` catalog queries during compilation (`HandlerExecutionScope`).
4. **Multi-Stage Chained Language Handlers:** Support language handlers that are themselves written in another `plv8`-hosted language (up to 32 levels deep via `PLV8_MAX_LANG_HANDLER_DEPTH`), with automatic cycle detection and full dependency tracking across the handler chain.
5. **Automatic Cache Invalidation (`fn_xmin` / `fn_tid`):** Automatically invalidate compiled target functions whenever *any* language handler in their dependency chain is updated via `CREATE OR REPLACE FUNCTION`.
6. **Zero-Overhead Binary Custom Base Types (`TYPCATEGORY_USER`):** Allow language handlers (especially WebAssembly and binary DSLs) to opt into receiving and returning user-defined PostgreSQL base types (`typtype = 'b'`, `typcategory = 'U'`) as raw `Uint8Array` varlena payload bytes (`meta.pass_user_types_as_bytes = true`), bypassing text `typinput`/`typoutput` serialization on hot operator paths.
7. **Full PostgreSQL Error Fidelity:** Preserve all 13 PostgreSQL `ErrorData` diagnostic fields (`sqlerrcode`/`code`, `message`, `detail`, `hint`, `context`, `schema_name`, `table_name`, `column_name`, `datatype_name`, `constraint_name`, `internalquery`, `internalpos`) across SPI exceptions, subtransactions, and language-level `TRY`/`CATCH` blocks.
8. **First-Class ES Modules, CommonJS & Shared WASM Caching:** Provide database-hosted ES Modules (`import`) and CommonJS (`require`) backed by `plv8.modules`, `plv8.packages`, and `pgtle.modules`, plus a cross-backend shared memory (`DSA` + `dshash`) compiled WebAssembly code cache (`plv8.compile_wasm_cached`).

### 2.2 Non-Goals
- **Replacing Native C Language Handlers:** Languages that require direct OS threads, raw pointers into PostgreSQL shared buffers, or non-V8 native code execution remain outside the scope of `plv8` language handlers.
- **Cross-Role V8 Context Sharing:** Compiled `v8::Function` instances are never shared across different PostgreSQL roles (`user_id`); role switches always re-bind or recompile within the target role's isolated `plv8_context`.

---

## 3. High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                         PostgreSQL Executor / DDL                            │
│  • Regular / SRF / Trigger Call  ──────► plv8_call_handler(fcinfo)           │
│  • DO $$ ... $$ LANGUAGE custom  ──────► plv8_inline_handler(fcinfo)         │
│  • CREATE FUNCTION ...           ──────► plv8_call_validator(fcinfo)         │
└───────────────────────────────────────────────────┬──────────────────────────┘
                                                    │
                                                    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                 C++ Entry & Cache Layer (plv8.cc)                            │
│  1. GetPlv8Context() -> per-role v8::Isolate / v8::Context (user_id)         │
│  2. plv8_sync_tle_module_cache(ctx) -> check shared-memory epoch             │
│  3. Lookup plv8_proc_cache by fn_oid in plv8_proc_cache_hash                 │
│  4. Validate:                                                                │
│     • fn_xmin / fn_tid of target pg_proc                                     │
│     • cache->user_id == current_context->user_id                             │
│     • cache->tle_modules_fp == current_context->tle_modules_fp               │
│     • plv8_cache_handlers_valid(cache, proctuple, &stale_handler_oid)        │
└───────────────────────────────────────────────────┬──────────────────────────┘
                                                    │ (Cache Miss / Stale)
                                                    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│              Transpilation Pipeline: plv8_transpile_src() (plv8.cc)          │
│  1. Walk pg_proc.prolang -> pg_language.lanplcallfoid -> handler pg_proc     │
│     until cur_lang_oid == plv8_lang_oid (max depth 32, cycle-checked).       │
│  2. Record handler chain deps[0..k-1] ({fn_oid, fn_xmin, fn_tid}).           │
│  3. Compile immediate handler (deps[0]) via find_js_function(handler_oid)    │
│     (which recursively transpiles deps[0] via deps[1] if chained!).          │
│  4. Enter HandlerExecutionScope (saves/clearsPLV8_INTNL_CONV/TUPSTORE/FCINFO)│
│  5. Build V8 `meta` object (fn_oid, proname, is_do_block, rettype_*, arg*)   │
│  6. Call handler_fn(src, meta) -> returns JS source string                   │
│  7. Read back `meta.pass_user_types_as_bytes` boolean flag                   │
└───────────────────────────────────────────────────┬──────────────────────────┘
                                                    │ (Transpiled JS Source)
                                                    ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│               V8 Compilation & Execution Layer (plv8.cc / plv8_type.cc)      │
│  • If plv8_extract_hoisted_imports() detects top-level `import`/`export`:    │
│    compile via plv8_compile_and_eval_anon_module() (V8 ES Module pipeline)   │
│  • Else: compile via DoCompile() -> new Function(args, body)                 │
│  • Marshal SQL Datums -> V8 Values (ToValue / TYPCATEGORY_USER Uint8Array)   │
│  • Call compiled V8 Function -> Marshal V8 Return Value -> SQL Datum         │
└──────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Core Data Structures (`plv8.h` & `plv8.cc`)

In [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L112-L148), `plv8` tracks compiled functions and their language handler dependencies using three key structures:

```cpp
#define PLV8_MAX_LANG_HANDLER_DEPTH 32

typedef struct plv8_handler_dep
{
    Oid             fn_oid;
    TransactionId   fn_xmin;
    ItemPointerData fn_tid;
} plv8_handler_dep;

typedef struct plv8_proc_cache
{
    Oid                         fn_oid;
    Persistent<Function>        function;

    char                        proname[NAMEDATALEN];
    char                       *prosrc;

    TransactionId               fn_xmin;
    ItemPointerData             fn_tid;
    Oid                         user_id;
    uint64                      tle_modules_fp;
    int                         nhandlers;
    plv8_handler_dep            handlers[PLV8_MAX_LANG_HANDLER_DEPTH];
    bool                        pass_user_types_as_bytes;

    int                         nargs;
    bool                        retset;
    Oid                         rettype;
    Oid                         argtypes[FUNC_MAX_ARGS];
} plv8_proc_cache;
```

Per-invocation runtime type information is stored in `plv8_proc` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L144-L151)), which wraps a pointer to `plv8_proc_cache`, the `plv8_exec_env *xenv`, and resolved `plv8_type` descriptors (`rettype` and `argtypes[FUNC_MAX_ARGS]`) allocated inside the caller's `fn_mcxt`.

---

## 4. Language Handler Discovery, Chaining & Transpilation Pipeline

### 4.1 Language Registration Contract

A `pl/v8`-hosted language is registered in PostgreSQL using `CREATE [TRUSTED] LANGUAGE`:

```sql
CREATE FUNCTION my_lang_handler()
RETURNS language_handler AS $$
  // arguments[0]: raw prosrc (or DO block source string)
  // arguments[1]: metadata object (or undefined when called via find_function)
  const src = arguments[0];
  const meta = arguments[1];
  return compileMyLangToJS(src, meta);
$$ LANGUAGE plv8;

REVOKE ALL ON FUNCTION my_lang_handler() FROM PUBLIC;

CREATE TRUSTED LANGUAGE my_lang
  HANDLER my_lang_handler
  INLINE plv8_inline_handler
  VALIDATOR plv8_call_validator;
```

Wait—why does `CREATE FUNCTION my_lang_handler() RETURNS language_handler ... LANGUAGE plv8` succeed in PostgreSQL?
In standard PostgreSQL, PLs often reject pseudo-types like `language_handler` as return types. In `pl/v8`:
1. **`plv8_call_validator` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1203-L1208))**: Explicitly allows `prorettype == LANGUAGE_HANDLERnoid` (`2280`) alongside normal base, composite, domain, polymorphic, and trigger return types.
2. **`plv8_call_handler` Direct Call Guard ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L920-L929))**: If a user or SQL query attempts to invoke a `RETURNS language_handler` function directly from SQL (where `fcinfo->flinfo->fn_oid == handler_oid` and the function's own return type in `pg_proc` is `LANGUAGE_HANDLERnoid`), `plv8_call_handler` immediately raises:
   ```cpp
   ereport(ERROR,
           (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("language handler function cannot be called directly")));
   ```
3. **`plv8.find_function` Lookup ([`plv8_func.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8_func.cc#L661-L720))**: `find_js_function_by_oid` permits `rettype == LANGUAGE_HANDLERnoid` so that language handlers can be resolved internally (and by other V8 functions with `EXECUTE` privilege) via `find_js_function`.

### 4.2 Language Chain Resolution (`plv8_is_js_language` & `plv8_transpile_src`)

Because a language handler is itself a `pg_proc` entry with a `prolang` OID, a language handler for `Language B` can be written in `Language A`, whose handler is written in `plv8`:

$$\text{Function } F \xrightarrow{\text{prolang}} \text{Lang } L_0 \xrightarrow{\text{lanplcallfoid}} \text{Handler } H_0 \xrightarrow{\text{prolang}} \text{Lang } L_1 \xrightarrow{\text{lanplcallfoid}} \text{Handler } H_1 \xrightarrow{\text{prolang}} \texttt{plv8}$$

Both [`plv8_is_js_language(Oid lang_oid)`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1942-L1981) and [`plv8_transpile_src(...)`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1983-L2182) resolve this chain as follows:

1. **Base Case (`lang_oid == plv8_lang_oid`):** Look up the `"plv8"` `pg_language` tuple via `SearchSysCache1(LANGNAME, NameGetDatum("plv8"))`. If `lang_oid == plv8_lang_oid`, no transpilation is needed (`plv8_transpile_src` returns `pstrdup(src)` with `*nhandlers_out = 0`).
2. **Iterative Chain Walk (Depth `0 .. PLV8_MAX_LANG_HANDLER_DEPTH - 1`):**
   - Maintain `Oid visited_langs[PLV8_MAX_LANG_HANDLER_DEPTH]` and `int depth = 0`.
   - At each step while `cur_lang_oid != plv8_lang_oid`:
     - **Cycle & Depth Check:** If `cur_lang_oid` is already in `visited_langs[0..depth-1]` or `depth >= PLV8_MAX_LANG_HANDLER_DEPTH` (`32`), abort with:
       ```cpp
       ereport(ERROR,
               (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                errmsg("language handler chain is cyclical or exceeds maximum depth")));
       ```
     - **Fetch `pg_language` (`LANOID`):** Extract `handler_oid = langform->lanplcallfoid`.
     - **Fetch Handler `pg_proc` (`PROCOID`):** Extract `next_lang_oid = handlerproc->prolang`, and record `deps[depth]` (`fn_oid = handler_oid`, `fn_xmin`, `fn_tid`).
     - At `depth == 0`, save `lang_handler_oid = handler_oid` (the immediate language handler for the target function's language).
     - Advance `cur_lang_oid = next_lang_oid` and `depth++`.
3. **Recursive Handler Compilation + Invocation:**
   - Once the chain reaches `plv8_lang_oid`, `plv8_transpile_src` copies `deps[0..depth-1]` into `handlers_out` (so the target function tracks *every* handler in the chain for cache invalidation) and calls:
     ```cpp
     Local<Function> fn = find_js_function(lang_handler_oid);
     ```
   - If `lang_handler_oid` ($H_0$) is written in intermediate language $L_1$, `find_js_function(H_0)` calls `Compile(H_0, ...)`, which recursively invokes `plv8_transpile_src` for $H_0$'s source code using $H_1$!
   - Thus, $H_1$ transpiles $H_0$'s source code into a V8 `Function`, and then `plv8_transpile_src` invokes $H_0(\text{src}, \text{meta})$ once to transpile the target function $F$'s source code into JavaScript.

### 4.3 Compile-Time `meta` Object Contract

When `plv8_transpile_src` invokes the language handler function, it passes two arguments:
- `arguments[0]` (`v8::String`): The raw source text (`prosrc` for a `CREATE FUNCTION`, or `laninline` block text for a `DO` block).
- `arguments[1]` (`v8::Object`): The compile-time `meta` object constructed in [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L2077-L2164).

| Property | V8 Type | When `proctuple != NULL` (`CREATE FUNCTION` / Call) | When `proctuple == NULL` (`DO` Block) | Description |
|---|---|---|---|---|
| `is_do_block` | `Boolean` | `false` | `true` | Distinguishes anonymous `DO` blocks from catalog functions. |
| `fn_oid` | `Uint32` | `HeapTupleGetOid(proctuple)` | *(omitted)* | `pg_proc.oid` of the function being compiled. |
| `proname` | `String` | `NameStr(procstruct->proname)` | *(omitted)* | Function name from `pg_proc.proname`. |
| `rettype_oid` | `Uint32` | `procstruct->prorettype` | *(omitted)* | Return type OID from `pg_proc.prorettype`. |
| `rettype_category` | `String` (1 char) | `get_typtype_category(prorettype)` | *(omitted)* | `pg_type.typcategory` (e.g. `'N'`, `'S'`, `'U'`, `'C'`, `'A'`, `'P'`). |
| `rettype_len` | `Int32` | `(len > VARHDRSZ) ? (len - VARHDRSZ) : len` | *(omitted)* | Payload byte length for fixed-size types, or `-1` for variable-length (`varlena`). |
| `argnames` | `Array<String>` | Input argument names (`""` if unnamed) | *(omitted)* | Filtered to input arguments (`PROARGMODE_IN`, `PROARGMODE_INOUT`, `PROARGMODE_VARIADIC`), length `pronargs`. |
| `argtype_oids` | `Array<Uint32>` | `procstruct->proargtypes.values[a]` | *(omitted)* | Array of input argument type OIDs, length `pronargs`. |
| `argtype_categories` | `Array<String>` | `get_typtype_category(arg_oid)` | *(omitted)* | 1-character `pg_type.typcategory` for each input argument. |
| `argtype_lens` | `Array<Int32>` | `(len > VARHDRSZ) ? (len - VARHDRSZ) : len` | *(omitted)* | Payload byte length for each input argument (`-1` for `varlena`). |
| `pass_user_types_as_bytes` | `Boolean` (Out) | Read back after handler returns | *(ignored)* | If mutated to `true` by the handler (`meta.pass_user_types_as_bytes = true`), enables raw `Uint8Array` marshaling for `TYPCATEGORY_USER` arguments and return values. |

#### Why `rettype_len` and `argtype_lens` Subtract `VARHDRSZ` (`4`)
In PostgreSQL (and specifically `pg_tle`'s `create_base_type_if_not_exists`), fixed-length user base types stored with varlena headers have `pg_type.typlen = internal_length + VARHDRSZ` (where `VARHDRSZ == 4`). `plv8_transpile_src` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L2106-L2108)) normalizes `len = (len > (int16) VARHDRSZ) ? (len - (int16) VARHDRSZ) : len` so that `meta.rettype_len` and `meta.argtype_lens[a]` report the exact user payload size (e.g., `8` bytes for a 2×`int32` vector `wasm_vec2i`, matching `"internallength": 8` in the `pg_tle` manifest).

#### How `argnames` Aligns with `argtype_oids`
In PostgreSQL's `pg_proc`, `proargnames` and `proargmodes` include `OUT` (`'o'`) and `TABLE` (`'t'`) parameters, whereas `proargtypes` (`pronargs`) contains only input-bearing parameters (`'i'`, `'b'`, `'v'`). `plv8_transpile_src` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L2115-L2141)) iterates `all_args` and skips modes other than `'i'`, `'b'`, and `'v'`, guaranteeing that `meta.argnames.length === meta.argtype_oids.length === pronargs`.

### 4.4 `HandlerExecutionScope`: Safe SPI Access During Transpilation

A major hazard when running a JavaScript language handler inside `plv8` is that compilation can be triggered *while another function or SRF is already executing* (or while setting up `plv8_call_handler`'s converter/tuplestore state on the global `plv8` object).

In `plv8`, the global `plv8` object stores four internal fields ([`plv8.h`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.h#L43-L50)):
- `PLV8_INTNL_CONV = 1`: Pointer to the active function's `Converter` (`srf_return_types`, `is_srf`).
- `PLV8_INTNL_TUPSTORE = 2`: Pointer to the active SRF's `Tuplestorestate`.
- `PLV8_INTNL_FCINFO = 3`: Pointer to the active `FunctionCallInfoBaseData` (`fcinfo`).

If a language handler calls `plv8.execute("SELECT ...")` during transpilation (for example, `plpgsql_v8` inspecting `pg_attribute` to resolve `table_name%ROWTYPE` or `col%TYPE`), or if a malicious handler attempted to call `plv8.return_next(...)` during compilation, leaving the outer function's internal fields exposed on `plv8` would corrupt the outer call's `Tuplestorestate` or `fcinfo`.

To prevent this, [`plv8.h`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.h#L334-L370) defines the RAII guard `HandlerExecutionScope`:

```cpp
class HandlerExecutionScope
{
private:
    v8::Isolate        *m_isolate;
    v8::Local<v8::Object> m_plv8;
    v8::Local<v8::Value> m_saved_conv;
    v8::Local<v8::Value> m_saved_tupstore;
    v8::Local<v8::Value> m_saved_fcinfo;
    bool                m_active;

public:
    HandlerExecutionScope(v8::Isolate *isolate, v8::Local<v8::Context> context)
        : m_isolate(isolate), m_active(false)
    {
        v8::Local<v8::Value> plv8_val =
            context->Global()->Get(context,
                v8::String::NewFromUtf8Literal(isolate, "plv8")).FromMaybe(v8::Local<v8::Value>());
        if (!plv8_val.IsEmpty() && plv8_val->IsObject())
        {
            m_plv8 = plv8_val.As<v8::Object>();
            if (m_plv8->InternalFieldCount() >= PLV8_INTNL_MAX)
            {
                m_saved_conv = m_plv8->GetInternalField(PLV8_INTNL_CONV);
                m_saved_tupstore = m_plv8->GetInternalField(PLV8_INTNL_TUPSTORE);
                m_saved_fcinfo = m_plv8->GetInternalField(PLV8_INTNL_FCINFO);
                m_plv8->SetInternalField(PLV8_INTNL_CONV, v8::External::New(isolate, NULL));
                m_plv8->SetInternalField(PLV8_INTNL_TUPSTORE, v8::External::New(isolate, NULL));
                m_plv8->SetInternalField(PLV8_INTNL_FCINFO, v8::External::New(isolate, NULL));
                m_active = true;
            }
        }
    }

    ~HandlerExecutionScope()
    {
        if (m_active)
        {
            m_plv8->SetInternalField(PLV8_INTNL_CONV, m_saved_conv);
            m_plv8->SetInternalField(PLV8_INTNL_TUPSTORE, m_saved_tupstore);
            m_plv8->SetInternalField(PLV8_INTNL_FCINFO, m_saved_fcinfo);
        }
    }
};
```

In [`plv8_transpile_src`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L2075), `HandlerExecutionScope handler_scope(isolate, context);` is instantiated immediately before building `meta` and calling `Call(context, fn, recv, nargs, args)`, and automatically restores the outer internal fields when `plv8_transpile_src` exits (whether normally or via C++ `js_error` exception unwinding).

### 4.5 All Four Entry Points Explained

1. **Normal & Set-Returning Function Calls (`plv8_call_handler` -> `plv8_get_proc` -> `CompileFunction`)**:
   - Looks up or creates the `plv8_proc_cache` entry for `fn_oid`.
   - Calls `Compile(fn_oid, fcinfo, validate, false)` -> `plv8_transpile_src(prosrc, prolang, proctuple, ...)` -> `CompileFunction(...)`.
   - Wraps the transpiled JavaScript body in `function(<arg0>, <arg1>, ...) { <transpiled_body> }` (or compiles it as an anonymous ES module if top-level `import`/`export` statements are present).
2. **Trigger Function Calls (`plv8_call_handler` with `CALLED_AS_TRIGGER(fcinfo)`)**:
   - Passes `is_trigger = true` to `plv8_get_proc`.
   - `CompileFunction` binds the standard trigger parameters `(NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV)` into the generated function signature so the transpiled trigger body can read and mutate `NEW`/`OLD` directly.
3. **Anonymous `DO` Blocks (`plv8_inline_handler`, [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1125-L1182))**:
   - Receives `InlineCodeBlock *codeblock` (`codeblock->source_text`, `codeblock->langOid`).
   - Calls `plv8_transpile_src(source_text, codeblock->langOid, NULL, NULL, NULL, NULL)`.
   - Because `proctuple == NULL`, `meta` has `{ is_do_block: true }`.
   - Compiles and executes the transpiled JS immediately in the current role's `plv8_context` inside an `SPI_connect()` / `SPI_finish()` block.
4. **Syntax & Handler Validation (`plv8_call_validator`, [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1188-L1254))**:
   - Invoked by PostgreSQL during `CREATE [OR REPLACE] FUNCTION` when `check_function_bodies = on`.
   - Verifies that the function's `prolang` resolves to `plv8` via `plv8_is_js_language(lang_oid)`.
   - Calls `Compile(fn_oid, NULL, true, is_trigger)` inside an `SPI_connect()` scope!
   - Why does `plv8_call_validator` connect to SPI? Because language handlers like `plv8_plpgsql_handler` and `plv8_sqltsm_handler` execute catalog queries (`SELECT ... FROM pg_catalog.pg_proc WHERE oid = $1`) during transpilation. Connecting to SPI in `plv8_call_validator` ensures handlers have identical SPI capabilities at `CREATE FUNCTION` validation time and at first-call time.

---

## 5. Compilation Cache & Dependency Invalidation

### 5.1 Multi-Dimensional Cache Validation

Each backend maintains `plv8_proc_cache_hash` (`HTAB *`, keyed by `Oid fn_oid`). Whenever `plv8_call_handler` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L931-L967)) or `plv8_get_proc` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L2206-L2248)) finds an existing `plv8_proc_cache` entry (`found == true && !cache->function.IsEmpty()`), it checks **four** dimensions before reusing the cached `v8::Persistent<v8::Function>`:

1. **Target Function Catalog Freshness (`fn_xmin` & `fn_tid`)**:
   ```cpp
   !(cache->fn_xmin == HeapTupleHeaderGetRawXmin(proctuple->t_data) &&
     ItemPointerEquals(&cache->fn_tid, &proctuple->t_self))
   ```
   Detects `CREATE OR REPLACE FUNCTION` on the target function itself.
2. **Role Isolation (`user_id`)**:
   ```cpp
   cache->user_id != current_context->user_id
   ```
   Detects when the function is called under a different effective role (`SET ROLE` / `SECURITY DEFINER`), ensuring `v8::Function` handles are never invoked across different V8 `Context`s.
3. **TLE Module Registry Fingerprint (`tle_modules_fp`)**:
   ```cpp
   cache->tle_modules_fp != current_context->tle_modules_fp
   ```
   Detects when any module in `plv8.modules`, `plv8.packages`, or `pgtle.modules` was modified since the function was compiled.
4. **Language Handler Chain Freshness (`plv8_cache_handlers_valid`)**:
   Checks every handler dependency `cache->handlers[0 .. cache->nhandlers - 1]` against `pg_proc`.

### 5.2 Handler Chain Validation (`plv8_cache_handlers_valid`) and Cascading Eviction

In [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1875-L1914), `plv8_cache_handlers_valid` verifies both that the language's current `lanplcallfoid` still matches `cache->handlers[0].fn_oid` (in case `ALTER LANGUAGE` or `DROP/CREATE LANGUAGE` changed the handler OID) and that every handler in `cache->handlers[0..nhandlers-1]` still has the exact same `(fn_xmin, fn_tid)` in `PROCOID` syscache:

```cpp
static bool
plv8_cache_handlers_valid(plv8_proc_cache *cache, HeapTuple proctuple, Oid *stale_handler_oid)
{
    if (stale_handler_oid)
        *stale_handler_oid = InvalidOid;

    if (cache->nhandlers == 0)
    {
        if (proctuple != NULL)
        {
            Form_pg_proc procstruct = (Form_pg_proc) GETSTRUCT(proctuple);
            HeapTuple langtuple = SearchSysCache1(LANOID, ObjectIdGetDatum(procstruct->prolang));
            if (HeapTupleIsValid(langtuple))
            {
                Form_pg_language langform = (Form_pg_language) GETSTRUCT(langtuple);
                Oid handler_oid = langform->lanplcallfoid;
                ReleaseSysCache(langtuple);
                /* Built-in plv8 has handler plv8_call_handler; custom language has JS handler */
            }
        }
        return true;
    }

    for (int i = 0; i < cache->nhandlers; i++)
    {
        HeapTuple htuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(cache->handlers[i].fn_oid));
        if (!HeapTupleIsValid(htuple))
        {
            if (stale_handler_oid)
                *stale_handler_oid = cache->handlers[i].fn_oid;
            return false;
        }
        bool valid = (cache->handlers[i].fn_xmin == HeapTupleHeaderGetRawXmin(htuple->t_data) &&
                      ItemPointerEquals(&cache->handlers[i].fn_tid, &htuple->t_self));
        ReleaseSysCache(htuple);
        if (!valid)
        {
            if (stale_handler_oid)
                *stale_handler_oid = cache->handlers[i].fn_oid;
            return false;
        }
    }
    return true;
}
```

When a handler $H_i$ is modified (`CREATE OR REPLACE FUNCTION`), two invalidation paths work together:
1. **Immediate Eviction at Validator Time (`plv8_call_validator`, [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1239))**: When `CREATE OR REPLACE FUNCTION my_handler()` runs, `plv8_call_validator` calls `plv8_cache_function_remove(fn_oid)`.
2. **Lazy Detection at Call Time (`plv8_call_handler` / `plv8_get_proc`)**: If `check_function_bodies` was `off` during `CREATE OR REPLACE FUNCTION`, `plv8_cache_handlers_valid` returns `false` with `stale_handler_oid = H_i`, and `plv8_call_handler` / `plv8_get_proc` calls `plv8_cache_function_remove(stale_handler_oid)`.

`plv8_cache_function_remove(Oid fn_oid)` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L1840-L1869)) scans `plv8_proc_cache_hash` with `HASH_SEQ_STATUS` and resets every entry where `cache->fn_oid == fn_oid` OR where any `cache->handlers[i].fn_oid == fn_oid`:
```cpp
if (remove)
{
    if (cache->prosrc)
    {
        pfree(cache->prosrc);
        cache->prosrc = NULL;
    }
    cache->function.Reset();
    cache->nhandlers = 0;
    cache->pass_user_types_as_bytes = false;
}
```
Resetting both the stale handler's own `cache->function` and all dependent functions' `cache->function` guarantees that when `plv8_transpile_src` subsequently calls `find_js_function(lang_handler_oid)`, the handler itself is freshly recompiled before transpiling its dependent functions.

---

## 6. Raw Custom Base Type Binary Protocol (`TYPCATEGORY_USER`)

### 6.1 Motivation

By default in `pl/v8`, any PostgreSQL type that does not have a dedicated C++ conversion path (such as user-defined base types created via `CREATE TYPE ... (INPUT = ..., OUTPUT = ..., INTERNALLENGTH = ...)` or `pgtle.create_base_type_if_not_exists`) falls back to calling PostgreSQL's `OidOutputFunctionCall(type->fn_output, datum)` on input and `OidInputFunctionCall(type->fn_input, str, ...)` on output. That converts the binary type to a C string on every argument and parses it back from a C string on every return value.

For binary-oriented language handlers—such as `pl/v8-wasm` implementing custom base types like `wasm_vec2i` (`internallength = 8`) and its `+` / `=` operators—converting binary datums to text and back on every operator call is both slow and unnatural, because the WASM functions operate directly on the raw 8-byte binary representation in linear memory.

### 6.2 Superuser-Only Opt-In Mechanism (`meta.pass_user_types_as_bytes = true` & `plv8.allow_raw_user_types`)

Because reading and returning raw in-memory `varlena` bytes bypasses PostgreSQL's type input validation (`typinput`), an arbitrary user-defined base type implemented in C (such as `hstore`, `ltree`, `citext`, or `postgis` `geometry`) could crash if fed malformed internal structural headers by an untrusted function. Therefore, raw `TYPCATEGORY_USER` byte marshaling is **restricted to superusers**:

1. **Superuser Ownership Check (`plv8_transpile_src`)**: Every language (`pg_language.lanowner`), every language handler function (`pg_proc.proowner`) in the handler chain, and the target function (`pg_proc.proowner`) must be owned by a superuser (`superuser_arg(...)`), and the `PGC_SUSET` GUC `plv8.allow_raw_user_types` (default `on`) must be enabled. If a non-superuser language handler or function sets `meta.pass_user_types_as_bytes = true`, compilation fails with `ERRCODE_INSUFFICIENT_PRIVILEGE` (`"must be superuser to enable pass_user_types_as_bytes"`).
2. **Runtime Gating (`ToScalarValue` & `ToScalarDatum` in `plv8_type.cc`)**: Even at call time, `ToScalarValue` and `ToScalarDatum` check `plv8_pass_user_types_as_bytes && plv8_allow_raw_user_types`. Normal `LANGUAGE plv8` functions (where `plv8_pass_user_types_as_bytes` is `false`) that attempt to return a `Uint8Array`, `Int8Array`, or `ArrayBuffer` for a `TYPCATEGORY_USER` return type are rejected with `ERRCODE_INSUFFICIENT_PRIVILEGE` (`"returning raw byte buffers for user-defined type %s requires superuser pass_user_types_as_bytes"`).

### 6.3 Why Raw Varlena Bytes Instead of `typreceive`/`typsend` or `typinput`/`typoutput`?

| Mechanism | Input Path (PG $\rightarrow$ JS/WASM) | Output Path (JS/WASM $\rightarrow$ PG) | Security & Performance Characteristics |
| :--- | :--- | :--- | :--- |
| **`typinput` / `typoutput` (Default for `LANGUAGE plv8`)** | `OidOutputFunctionCall` formats `Datum` to NUL-terminated text string. | `OidInputFunctionCall` parses NUL-terminated text string into `Datum`. | **Safe for all `TRUSTED` callers** (validates every invariant in `typinput`), but incurs text formatting/parsing and allocation overhead on every call. |
| **`typreceive` / `typsend` (Wire Binary Format)** | `OidSendFunctionCall` serializes `Datum` into a `bytea` (`StringInfoData`) in network (big-endian) wire format. | Constructs a `StringInfoData` cursor over the returned bytes and calls `OidReceiveFunctionCall(typreceive, &buf, ...)`. | **Safe alternative for untrusted callers** when a type defines `typreceive`/`typsend`: `typreceive` validates structural invariants and `read_only` cursor boundaries, avoiding text parsing. However: (1) `pg_tle` custom base types (`pgtle.create_base_type_if_not_exists`) only register `typinput`/`typoutput` (`typreceive`/`typsend` are `InvalidOid`), and (2) `typsend`/`typreceive` converts fields to big-endian network byte order rather than native little-endian WASM linear memory layout. |
| **Raw `varlena` Bytes (`pass_user_types_as_bytes`, Superuser-Only)** | `PG_DETOAST_DATUM_COPY` (when `type->len == -1`) and `CreateExternalArray(VARDATA_ANY(p), ...)` passes the native varlena payload directly as a `Uint8Array`. | Validates `byte_len <= MaxAllocSize - VARHDRSZ` (and `byte_len + VARHDRSZ == type->len` for fixed-length types), allocates `palloc(varlena_size)`, and `memcpy`s payload bytes directly. | **Zero-serialization overhead** and native little-endian layout matching WASM linear memory (`pl/v8-wasm`), gated strictly behind superuser ownership and `plv8.allow_raw_user_types` (`PGC_SUSET`). |

### 6.4 Input Marshaling (`ToScalarValue` in `plv8_type.cc`)

In [`ToScalarValue`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8_type.cc):
- When `plv8_pass_user_types_as_bytes && plv8_allow_raw_user_types` is active and `type->len == -1` (varlena), the datum is detoasted via `PG_DETOAST_DATUM_COPY(datum)` and its payload (`VARDATA_ANY(p)`, length `VARSIZE_ANY_EXHDR(p)`) is exposed as a V8 `Uint8Array` via `CreateExternalArray`.

### 6.5 Output Marshaling & Fixed-Length Validation (`ToScalarDatum` in `plv8_type.cc`)

In [`ToScalarDatum`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8_type.cc), when `type->category == TYPCATEGORY_USER` and `val->IsUint8Array() || val->IsInt8Array() || val->IsArrayBuffer()`:
1. **Privilege Check:** Verifies `plv8_pass_user_types_as_bytes && plv8_allow_raw_user_types`; otherwise raises `ERRCODE_INSUFFICIENT_PRIVILEGE`.
2. **Overflow & Fixed-Size Validation:** Verifies `byte_len <= (size_t) MaxAllocSize - VARHDRSZ`, computes `size_t varlena_size = byte_len + VARHDRSZ;`, and if the PostgreSQL type has a fixed `typlen > 0` where `(size_t) type->len != varlena_size`, raises `ERRCODE_DATA_EXCEPTION` (`22000`).
3. **Varlena Construction:** Allocates `(struct varlena *) palloc(varlena_size)`, sets `SET_VARSIZE(result, varlena_size)`, copies `byte_len` bytes into `VARDATA(result)` (when `byte_len > 0`), and returns `PointerGetDatum(result)`.

---

## 7. Structured Error Propagation & PostgreSQL Diagnostics

Procedural languages like PL/pgSQL (`RAISE ... USING ERRCODE = ..., DETAIL = ..., HINT = ..., COLUMN = ..., CONSTRAINT = ...`, `GET STACKED DIAGNOSTICS`), T-SQL (`THROW`, `RAISERROR`, `ERROR_NUMBER()`, `ERROR_MESSAGE()`), SQL/PSM (`SIGNAL`, `RESIGNAL`, `GET DIAGNOSTICS`), and Python (`plpy.SPIError`, `postgres.DatabaseError`) require bidirectional fidelity between PostgreSQL's `ErrorData` structure and V8 JavaScript `Error` objects.

### 7.1 PostgreSQL SPI / Function Call Error $\rightarrow$ V8 `Error` Object

When a C/SQL function called via `plv8.execute(...)`, `PreparedPlan.execute(...)`, `Cursor.fetch(...)`, `plv8.subtransaction(...)`, or `plv8_FunctionInvoker` throws a PostgreSQL `elog(ERROR)` / `ereport(ERROR)`:
1. `PG_CATCH()` intercepts the error in `CopyErrorData()` (`ErrorData *edata`), flushes the error state (`FlushErrorState()`), and rolls back the internal subtransaction if applicable.
2. Both `js_error::init(ErrorData *edata)` / `js_error::error_object()` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3312-L3530)) and `plv8_FunctionInvoker` ([`plv8_func.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8_func.cc#L531-L582)) construct a V8 `v8::Exception::Error(message)` and populate up to **13 diagnostic properties**:

| V8 `Error` Property | Type | Source `ErrorData` Field | PostgreSQL Diagnostic Equivalent |
|---|---|---|---|
| `message` | `String` | `edata->message` | `MESSAGE_TEXT` / `SQLERRM` |
| `sqlerrcode` | `String` | `unpack_sql_state(edata->sqlerrcode)` | `RETURNED_SQLSTATE` / `SQLSTATE` (5-char string, or integer `errcode` accepted on input) |
| `code` | `String` | `unpack_sql_state(edata->sqlerrcode)` | Alias for `sqlerrcode` (`RETURNED_SQLSTATE`) |
| `detail` | `String` | `edata->detail` | `PG_EXCEPTION_DETAIL` |
| `hint` | `String` | `edata->hint` | `PG_EXCEPTION_HINT` |
| `context` | `String` | `edata->context` | `PG_EXCEPTION_CONTEXT` |
| `schema_name` | `String` | `edata->schema_name` | `SCHEMA_NAME` |
| `table_name` | `String` | `edata->table_name` | `TABLE_NAME` |
| `column_name` | `String` | `edata->column_name` | `COLUMN_NAME` |
| `datatype_name` | `String` | `edata->datatype_name` | `PG_DATATYPE_NAME` |
| `constraint_name` | `String` | `edata->constraint_name` | `CONSTRAINT_NAME` |
| `internalquery` | `String` | `edata->internalquery` | Internal query text |
| `internalpos` | `Int32` | `edata->internalpos` | 1-based character offset in `internalquery` |

### 7.2 V8 `Error` Object $\rightarrow$ PostgreSQL `ereport(ERROR, ...)` (`js_error::init` & `js_error::rethrow`)

When a V8 function or language handler throws an uncaught JS exception, [`js_error::init(v8::Isolate *isolate, v8::TryCatch &try_catch)`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3331-L3476) inspects the thrown `v8::Value`:

1. **`sqlerrcode` / `code` Parsing:**
   - Reads `errObj.sqlerrcode` (falling back to `errObj.code`).
   - If it is a `Number` (packed PostgreSQL `MAKE_SQLSTATE` integer), assigns `m_code = cval`.
   - If it is a 5-character `String` (`strlen(cstr) == 5`), converts it via `MAKE_SQLSTATE(cstr[0], cstr[1], cstr[2], cstr[3], cstr[4])`.
   - If neither `sqlerrcode` nor `code` is provided on the JS `Error`, `m_code` remains `0`, and PostgreSQL's `ereport(ERROR, ...)` defaults to `ERRCODE_INTERNAL_ERROR` (`XX000`).
2. **Structured Diagnostic Extraction:**
   - Extracts `detail`, `hint`, `schema_name`, `table_name`, `column_name`, `datatype_name`, `constraint_name`, `internalquery`, and `internalpos` from the JS `Error` object if present.
   - Appends the V8 JavaScript stack trace (`try_catch.StackTrace()`) and any existing `errObj.context` into `m_context`.
3. **Re-throwing into PostgreSQL (`js_error::rethrow()` / `js_error::log()`):**
   - Calls `ereport(ERROR, ...)` with:
     - `m_code ? errcode(m_code) : 0`
     - `errmsg("%s", m_msg)`
     - `m_detail ? errdetail("%s", m_detail) : 0`
     - `m_hint ? errhint("%s", m_hint) : 0`
     - `m_context ? errcontext("%s", m_context) : 0`
     - `err_generic_string(PG_DIAG_SCHEMA_NAME, m_schema_name)` (and `PG_DIAG_TABLE_NAME`, `PG_DIAG_COLUMN_NAME`, `PG_DIAG_DATATYPE_NAME`, `PG_DIAG_CONSTRAINT_NAME` when non-null).

---

## 8. Trusted Language Extension (TLE) Module System & WASM Native Cache

Language handlers and user functions often share runtime libraries, parsers, or compiled WebAssembly modules. `pl/v8` provides a database-backed module system supporting both **ES Modules (`import`)** and **CommonJS (`require()` / `plv8.require()`)**, integrated with `pg_tle` and backed by a cross-backend shared-memory WASM native code cache.

### 8.1 Catalog Schema (`plv8.packages` & `plv8.modules`)

Defined in [`plv8.sql.common`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.sql.common#L34-L93):

```sql
CREATE SCHEMA IF NOT EXISTS plv8;

CREATE TABLE IF NOT EXISTS plv8.packages (
    name            TEXT NOT NULL,
    version         TEXT NOT NULL DEFAULT '1.0.0',
    entry_point     TEXT NOT NULL DEFAULT 'index.js',
    dependencies    JSONB DEFAULT '{}',
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ,
    PRIMARY KEY (name, version)
);

CREATE TABLE IF NOT EXISTS plv8.modules (
    path            TEXT NOT NULL,
    package_name    TEXT,
    package_version TEXT,
    source          TEXT,
    bytecode        BYTEA,
    requires        TEXT[],
    is_trusted      BOOLEAN NOT NULL DEFAULT true,
    created_at      TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    updated_at      TIMESTAMPTZ,
    FOREIGN KEY (package_name, package_version) REFERENCES plv8.packages (name, version)
);

CREATE UNIQUE INDEX IF NOT EXISTS plv8_modules_standalone_path
    ON plv8.modules (path)
    WHERE package_name IS NULL;

CREATE UNIQUE INDEX IF NOT EXISTS plv8_modules_package_path
    ON plv8.modules (package_name, package_version, path)
    WHERE package_name IS NOT NULL;
```

Notice that `plv8.packages` uses a composite primary key `(name, version)` so multiple versions of the same package can coexist in the catalog, and `plv8.modules` enforces uniqueness via two partial unique indexes (`plv8_modules_standalone_path` for standalone modules where `package_name IS NULL`, and `plv8_modules_package_path` for versioned package files). Both tables are registered with `pg_extension_config_dump` so `pg_dump` preserves installed modules.

### 8.2 Cross-Backend Module Cache Invalidation (`plv8_modules_changed`)

Statement-level `AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE` triggers (`ENABLE ALWAYS TRIGGER plv8_modules_changed`) on both `plv8.modules` and `plv8.packages` invoke the C trigger function [`plv8_modules_changed`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3615-L3623):

1. `plv8_modules_changed` calls `plv8_bump_tle_modules_epoch()` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3582-L3612)), which increments both a local epoch counter and the atomic `pg_atomic_uint64 tle_modules_epoch` in PostgreSQL shared memory (`plv8_wasm_shared_state`).
2. Before any function execution, inline block, or module lookup, `plv8_sync_tle_module_cache(plv8_context *ctx)` ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3672-L3705)) reads the shared-memory epoch (`plv8_read_tle_modules_epoch()`).
3. If `ctx->tle_modules_epoch_seen != cur_epoch`, `plv8_sync_tle_module_cache` queries `plv8_compute_tle_modules_fingerprint()` (a 64-bit FNV-1a hash over `plv8.modules`, `plv8.packages`, and `pgtle.modules`). If the fingerprint changed:
   - Clears the per-context CommonJS cache (`__plv8_cjs_cache`) and ES module caches (`ctx->esm_modules`, `ctx->esm_meta`, `ctx->esm_ById`).
   - Updates `ctx->tle_modules_fp = fp`.
   - Any `plv8_proc_cache` entry whose `cache->tle_modules_fp != ctx->tle_modules_fp` is automatically recompiled on next call!

### 8.3 Specifier Resolution Order (`plv8_fetch_tle_module`)

When a module is imported via `import ... from '<specifier>'`, dynamic `import('<specifier>')`, or `require('<specifier>')`, [`plv8_fetch_tle_module`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3900-L4336) resolves `<specifier>` as follows:

1. **Prefix Normalization:** Strips optional prefixes `plv8:`, `pgtle:`, `tle://`, or `tle:`.
2. **Relative Path Resolution (`./` or `../`):** Resolved relative to `referrer_dir` (and constrained to the referrer's `(package_name, package_version)` if the referrer belongs to a package).
3. **Versioned Package Specifiers (`pkg@1.2.0` or `@scope/pkg@1.2.0/subpath`):**
   - Queries `plv8.packages` for `name = $1` (matching the exact version or semver range via `plv8_semver_satisfies`, picking the highest matching version via `plv8_semver_compare`).
   - Resolves the target file path as `sub_path.empty() ? best_ep : sub_path` in `plv8.modules` where `package_name = $1 AND package_version = $2 AND path = $3`.
4. **Standalone Module Lookup in `plv8.modules`:**
   - Queries `SELECT source, bytecode, is_trusted, requires FROM plv8.modules WHERE path = $1 AND package_name IS NULL`.
5. **Fallback to `pgtle.modules` (`pgtle.get_module`):**
   - If `pgtle.modules` exists, calls `SELECT module_name, version, source_code, bytecode, dependencies, is_trusted FROM pgtle.get_module($1, $2)`.

#### Enforced Security & Governance Checks During Module Load (`plv8_load_tle_module`)
In [`plv8_load_tle_module`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L4669-L4730):
- **Max Depth Guard (`plv8.max_module_depth`):** Default `16` (range `1..64`). Exceeding `plv8_max_module_depth` throws a V8 `Error("maximum module import depth (N) exceeded (circular dependency or deep chain)")`.
- **Circular Dependency Detection:** If a module is currently in state `1` (evaluating) on the active load stack:
  - For CommonJS (`is_esm == false`), returns the partially populated `exports` object (standard Node.js CommonJS circular `require` semantics).
  - For ES Modules (`is_esm == true`), V8's native `v8::Module::InstantiateModule` handles live ES module bindings; if hit in manual fallback, throws `Error("circular dependency detected while loading module \"...\"")`.
- **Untrusted Module Check (`is_trusted = false`):** If `!mod.is_trusted && !superuser()`, throws a V8 `Error("permission denied for untrusted module \"<key>\"")`.
- **Allowlist Enforcement (`requires TEXT[]`):** If the referrer module has a non-NULL `requires` array (`referrer_has_requires`), any imported module must match either `specifier`, `resolved_key`, `pkg_name`, or `bare_spec` in `referrer_requires`; otherwise `plv8_load_tle_module` throws `Error("plv8 module \"<spec>\" is not listed in requires of \"<referrer>\"")`.

### 8.4 ES Module Hoisting (`plv8_extract_hoisted_imports`) & `import.meta`

Standard V8 `new Function(...)` (`CompileFunction`) parses code as a classic `Script`, which rejects `import` and `export` declarations with `SyntaxError: Cannot use import statement outside a module`.

To support ES Module syntax inside `plv8` functions and language handler output, [`plv8_extract_hoisted_imports`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3720-L3848) performs a lexical scan of the function body (skipping comments, single/double-quoted strings, and template literals):
- Any top-level `import ...` statements are extracted into `hoisted_imports_out`.
- If top-level `export default <expr>` is encountered at brace depth `0`, it is rewritten to `return <expr>` inside the generated inner function body (`body_without_imports_out`), while top-level `export function|const|let|var|class` strips the `export` keyword.
- When `has_esm` is `true`, [`CompileFunction`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L2612-L2644) wraps the function in an anonymous ES module:
  ```javascript
  <hoisted_imports>
  export default (function(<arg0>, <arg1>, ...) {
    <body_without_imports>
  });
  ```
  and compiles/instantiates/evaluates it via [`plv8_compile_and_eval_anon_module`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L4587-L4667) using V8's `ScriptCompiler::CompileModule`, `plv8_ResolveModuleCallback`, `plv8_HostImportModuleDynamicallyCallback` (for dynamic `await import(...)`), and `plv8_HostInitializeImportMetaObjectCallback`.
- Inside any loaded ES module, `import.meta` is populated with ([`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L4164-L4169), [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L4542-L4585)):
  - `import.meta.url`: `"plv8:<package_name>@<package_version>/<path>"` (for package modules) or `"plv8:<path>"` (for standalone modules).
  - `import.meta.version`: `"<package_version>"` (or `"standalone"`).

### 8.5 First-Class WASM Modules & Cross-Backend Shared Memory Cache (`DSA` + `dshash`)

1. **Direct WASM Import (`\0asm` Detection):**
   When `plv8_load_tle_module` sees that a module's `bytecode` column starts with the 4-byte WebAssembly magic header `\x00\x61\x73\x6d` (`\0asm`, length $\ge 8$), it compiles and instantiates the WebAssembly binary (providing WASI and `postgres` host imports) and exposes its exports as both ES module named/default exports and CommonJS `require()` exports!
2. **Cross-Backend Shared Memory Native WASM Cache (`plv8.compile_wasm_cached`):**
   Compiling a large `.wasm` binary (such as `python315.wasm` or a Rust extension) with V8's Liftoff/TurboFan compiler takes several milliseconds per PostgreSQL backend process. To eliminate per-backend cold-start compilation overhead:
   - Controlled by the `PGC_POSTMASTER` GUC `plv8.wasm_cache_size` (default `64` MB, range `0..2048` MB, active when `plv8` is loaded via `shared_preload_libraries`).
   - On backend startup (`_PG_init`) or first use (`plv8_ensure_wasm_cache_attached`), `plv8` attaches to a PostgreSQL Dynamic Shared Area (`dsa_area`) and a shared concurrent hash table (`dshash_table`) named `"plv8_wasm_code_cache"` guarded by `LWLock`.
   - Each `Plv8WasmCacheEntry` is keyed by `Plv8WasmCacheKey { Oid db_oid; Oid role_oid; uint8 wire_sha256[32]; }` (computed directly from a local copy of `wire_bytes` via `pg_cryptohash_create(PG_SHA256)`) and stores `dsa_pointer code_ptr`, `uint32 code_size`, and `uint64 last_used_seq`.
   - `plv8.compile_wasm_cached(wasm_bytes)` ([`plv8_func.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8_func.cc)):
     - **Cache Hit:** Calls `plv8_wasm_cache_lookup(wire_bytes, wire_len, ...)`. If found, bumps `last_used_seq` and calls `v8::WasmModuleObject::DeserializeOrCompile(isolate, wire_span, cached_code_span)` to deserialize the pre-compiled native machine code directly into a `v8::WasmModuleObject` without re-running Liftoff/TurboFan.
     - **Cache Miss:** Compiles the module, extracts the native code via `mod_obj->GetCompiledModule().Serialize()`, and stores it into the shared `DSA` + `dshash` table via `plv8_wasm_cache_store` (enforcing a 25% per-role byte quota for non-superusers and LRU eviction by `last_used_seq`).
   - Cache telemetry (`enabled`, `max_bytes`, `used_bytes`, `entries`, `hits`, `misses`, `stores`, `evictions`) is exposed via `SELECT plv8_info()->0->'wasm_cache'`.

---

## 9. Security & Multi-Tenancy Model

1. **Per-Role V8 Isolate/Context Separation (`GetPlv8Context`, [`plv8.cc`](file:///google/src/cloud/hannuk/port_plv8_to_pljs/google3/third_party/pgext/plv8/plv8.cc#L3125-L3225))**:
   - Every PostgreSQL role (`GetUserId()`) gets its own `plv8_context` (`v8::Persistent<v8::Context>`).
   - Global objects (`globalThis`) initialized by a language handler (such as `globalThis.__plpgsql_v8_compile` or `globalThis.__plpython_v8_runtime`) live strictly inside that role's V8 context.
   - `plv8_proc_cache` tracks `cache->user_id`; switching roles (`SET ROLE` or `SECURITY DEFINER`) forces recompilation in the active role's `plv8_context`.
2. **Handler Direct-Call Prevention**:
   - Language handler functions (`RETURNS language_handler`) have `REVOKE ALL ON FUNCTION ... FROM PUBLIC` in `plv8.sql.common` and are blocked from direct SQL invocation by `plv8_call_handler` (`ERRCODE_FEATURE_NOT_SUPPORTED`).
   - Only PostgreSQL's internal language call mechanism (`plv8_transpile_src`) invokes the handler to transpile a function's `prosrc`.
3. **Compile-Time Isolation (`HandlerExecutionScope`)**:
   - During handler execution, `PLV8_INTNL_CONV`, `PLV8_INTNL_TUPSTORE`, and `PLV8_INTNL_FCINFO` are nulled out on `plv8` and restored on exit, preventing a handler from interfering with an in-progress caller's tuple store or `fcinfo`.
4. **Trusted vs. Untrusted Modules (`is_trusted`) & Dependency Allowlists (`requires`)**:
   - Both `plv8.modules` and `pgtle.modules` enforce `is_trusted`: non-superusers cannot import or execute modules marked `is_trusted = false`.
   - Modules with a non-NULL `requires` array can only import specifiers explicitly listed in `requires`.
5. **Heap Memory Limits (`plv8.memory_limit`) & Execution Timeout (`plv8.execution_timeout`)**:
   - Every V8 isolate is bounded by `plv8.memory_limit` (default `256` MB, enforced by `Allocator` and V8 heap limits) and `plv8.execution_timeout` (default `300` seconds via `KillTimerThread` / `v8::Isolate::TerminateExecution`).

---

## 10. Case Studies: Built-In & Ecosystem Language Handlers

### 10.1 `plpgsql_v8` (`plv8_plpgsql_handler` in `plv8.sql.common:94-4270`)

- **Registration:**
  ```sql
  CREATE TRUSTED LANGUAGE plpgsql_v8
      HANDLER plv8_plpgsql_handler
      INLINE plv8_inline_handler
      VALIDATOR plv8_call_validator;
  ```
- **Architecture:**
  - Lazily initializes `globalThis.__plpgsql_v8_compile` and the runtime helper object `globalThis.__plpgsql_v8_rt`.
  - During `compilePlpgsql(src, meta)`, if `meta.fn_oid > 0`, queries `pg_catalog.pg_proc` and `pg_catalog.pg_type` via `plv8.execute(...)` to inspect `proretset`, `prorettype`, `proargmodes` (`i`, `o`, `b`, `v`, `t`), `proargnames`, and `prokind`.
  - Resolves `%TYPE` and `%ROWTYPE` declarations at compile time by querying `pg_catalog.pg_attribute`.
  - Compiles PL/pgSQL blocks (`DECLARE ... BEGIN ... EXCEPTION WHEN ... END`), loops (`FOR i IN a..b BY step`, `FOR rec IN SELECT ...`, `FOR rec IN cursor`, `FOREACH ... SLICE`, `WHILE`, `LOOP`, labeled `EXIT`/`CONTINUE`), cursors (`OPEN`, `FETCH`, `MOVE`, `CLOSE`), `RETURN NEXT` / `RETURN QUERY`, `RAISE ... USING`, `ASSERT`, and `GET [STACKED] DIAGNOSTICS` into fast synchronous JavaScript.
  - Compiles `BEGIN ... EXCEPTION WHEN ... END` blocks into `plv8.subtransaction(function() { ... })` wrapped in `try / catch (__err)` that matches PostgreSQL SQLSTATE codes and condition names (`COND_MAP`).
  - Exposes `plv8.compile_plpgsql(src text, meta jsonb DEFAULT NULL) RETURNS text` for inspecting the generated JavaScript.

### 10.2 `sql/tsm` & `sql/psm` (`plv8_sqltsm_handler` in `plv8.sql.common:4272-10230`)

- **Registration:**
  Registers four language names sharing `plv8_sqltsm_handler`: `sqltsm_v8`, `"sql/tsm"`, `sqlpsm_v8`, and `"sql/psm"`.
- **Architecture:**
  - Extends `globalThis.__plpgsql_v8_rt` with T-SQL and SQL/PSM runtime helpers (`tsqlCoerce`, `tsqlConvert`, `tsqlFormatMessage`, `doTsqlRaiserror`, `doSignal`, `findMatchingHandler`, `execSpExecuteSql`, table variable management).
  - Supports T-SQL `@var` local variables, `@@ROWCOUNT`, `@@ERROR`, `@@FETCH_STATUS`, `@@TRANCOUNT`, `SELECT TOP (n) [WITH TIES]`, `SELECT @var = col FROM ...`, `DECLARE @t TABLE (...)`, `BEGIN TRY ... END TRY BEGIN CATCH ... END CATCH`, `THROW`, `RAISERROR`, `EXEC` / `sp_executesql` with `OUTPUT` parameters, and `GOTO` state-machine dispatch (`__goto_dispatch`).
  - Supports SQL/PSM `BEGIN ATOMIC`, `DECLARE ... CONDITION FOR SQLSTATE ...`, `DECLARE CONTINUE | EXIT | UNDO HANDLER FOR ...` (with `UNDO` handlers automatically wrapping the compound block in `plv8.subtransaction`), `SIGNAL`, `RESIGNAL`, `REPEAT ... UNTIL`, `LEAVE`, and `ITERATE`.
  - Exposes `plv8.compile_sqltsm(src, meta)` and `plv8.compile_sqlpsm(src, meta)`.

### 10.3 `pl/fullpython+v8` (`python315_wasm_bundle` & `plv8_fullpython_handler` in `plv8.sql.common:10232-19215`)

- **Registration:**
  Registers `"pl/fullpython+v8"`, `plfullpython_v8`, `"pl/python+v8"`, and `plpython_v8` with `HANDLER plv8_fullpython_handler`.
- **Architecture:**
  - `python315_wasm_bundle()` embeds a compiled Python 3.15 / MicroPython WebAssembly module (`MP_WASM_BASE64`, `python315.wasm`) and compiles it via `plv8.compile_wasm_cached("plpython_v8_py3_15_0", wasmBytes)`.
  - Initializes a 64 KB linear-memory heap (`wasmExports.py315_init(64 * 1024)`) and captures a warm linear-memory snapshot (`mp_snapshot_save`); before each invocation (`beginWasmInvocation()`), `mp_snapshot_restore()` resets the WASM heap in microseconds.
  - Validates Python 3.15 syntax and indentation using the compiled C lexer/validator inside WASM (`mp_check_syntax`).
  - Bridges the `_postgres` C WASM module to PostgreSQL SPI via `env.host_direct_spi_execute` and a packed **Binary Slot Buffer** (`readRowsFromWasmBinarySlotBuffer` / `unpack_binary_slot_buffer`) in WASM linear memory, avoiding JSON serialization overhead.
  - Provides both the classic `plpy` API (`plpy.execute`, `plpy.prepare`, `plpy.cursor`, `plpy.subtransaction`, `plpy.info`/`notice`/`warning`/`error`, `SD`, `GD`) and the modern `postgres` DB-API 2.0 / PEP 249 connection/cursor interface, plus a SQL-backed module registry (`plv8.plpython_v8_modules`) supporting both pure-Python (`plpython_v8_install_module`) and compiled C/WASM extension modules (`plpython_v8_install_wasm_module`).

### 10.4 `pl/v8-wasm` (`public.plv8_wasm_handler` in `pg_tle--1.5.3--1.6.0.sql:136-1530`)

- **Registration:**
  Bootstrapped by `SELECT pgtle.bootstrap_wasm_engine();` (or automatically by `SELECT pgtle.install_wasm_extension(...)`), which creates `public.plv8_wasm_modules`, `public.plv8_wasm_bootstrap_polyfills()`, `public.plv8_wasm_handler()`, and:
  ```sql
  CREATE TRUSTED LANGUAGE "pl/v8-wasm"
    HANDLER public.plv8_wasm_handler
    INLINE plv8_inline_handler
    VALIDATOR plv8_call_validator;
  ```
- **Architecture:**
  - Sets `meta.pass_user_types_as_bytes = true` so custom PostgreSQL base types (`TYPCATEGORY_USER`, such as `wasm_vec2i`) are passed directly as `Uint8Array` buffers without text conversion.
  - Parses the function body (`prosrc`) as a **JSON configuration object** requiring `"export"` (the exported WASM function name) and either `"module"` (a module registered in `plv8.modules`, `pgtle.modules`, or `public.plv8_wasm_modules`) or `"wasm"` (an inline Base64-encoded `.wasm` binary).
  - Pre-warms the cross-backend shared memory WASM cache via `plv8.compile_wasm_cached(cacheKey, bytes)` at compilation time.
  - Automatically marshals `int2`/`int4`/`int8` (`BigInt`), `float4`/`float8`, `bool`, and linear-memory buffers (`bytea`, `text`, and `TYPCATEGORY_USER` custom base types via `alloc`/`malloc`/`__wbindgen_malloc` or bump allocation at `__heap_base`), and automatically evicts the cached `WebAssembly.Instance` on a WASM trap (`catch (err) { delete globalThis._wasm_instances[cacheKey]; ... }`) so subsequent calls cleanly re-instantiate from the cached `WebAssembly.Module`.

### 10.5 `pl-pgq` (`CREATE PROPERTY GRAPH` & `GRAPH_TABLE` in Pure JS/WASM on `pl/v8`)

- **Registration & Zero-C Architecture:**
  - Registers `LANGUAGE "CREATE PROPERTY GRAPH"` (plus `"create property graph"`, `property_graph`, `sqlpgq_v8`) backed by `pgq_create_property_graph_handler()`.
  - Uses `CREATE DOMAIN pgq_pattern AS plv8_source;` and declares `graph_table(pg, pattern)` in pure `LANGUAGE plv8 STABLE SUPPORT plv8_call_support` — requiring **zero C files** (`0` `.c` files, no `.so` module of its own).
  - Registers `plv8.register_column_inference('graph_table', 'pgq_infer_columns_tsv')` and `plv8.register_support('graph_table', 'pgq_compile_graph_table_tsv')` so `SELECT * FROM GRAPH_TABLE(myshop(), 'MATCH ... COLUMNS (...)')` works without `AS (...)` and inlines directly into the outer plan with `LATERAL` `Var` correlation.

---

## 11. Dynamic Table Functions (`RETURNS SETOF record`), Parse-Time Column Inference (`plv8_source`), and Planner `prosupport` Subquery Inlining (`plv8_call_support`)

PostgreSQL normally requires any `RETURNS SETOF record` function called in a `FROM` clause without `OUT`/`TABLE` parameters to be followed by an explicit `AS (col1 type1, ...)` column definition list, and executes non-SQL set-returning functions as an opaque `FunctionScan` (`Tuplestorestate` barrier).

To enable **100% pure-SQL/JS/WASM extensions** (like `pl-pgq`, Datalog/PRQL/Malloy query compilers, or dynamic `pl/v8` table macros) to infer output columns at parse time and inline compiled SQL subqueries into the outer query plan with zero C code of their own, core `pl/v8` v4.0 provides a complete C-to-JS Table Macro & Planner Support subsystem in `plv8.cc` and `plv8_func.cc`.

### 11.1 Architectural Overview

```
  SELECT a, b FROM my_macro(ctx, 'MATCH ... COLUMNS (x AS a, y AS b)') WHERE a = 42
                                  │
  ┌───────────────────────────────┴───────────────────────────────────────────┐
  │ 1. PARSE TIME (`ParseFuncOrColumn` -> `make_fn_arguments`)                │
  │    • Coercing the string literal to `plv8_source` (or any domain over     │
  │      `plv8_source`) invokes `plv8_source_in(cstring)` in `plv8.cc`.       │
  │    • `plv8_extract_enclosing_call(debug_query_string, str)` locates       │
  │      the enclosing `my_macro(...)` call, extracts `first_arg` (`ctx`),    │
  │      and checks whether an explicit `AS (...)` follows `)`.               │
  │    • Invokes the JS callback registered via                               │
  │      `plv8.register_column_inference('my_macro', fn)` (or SQL catalog     │
  │      hook `<func>_infer_columns(query_ctx, source_arg)`).                 │
  │    • Stamps `PROARGMODE_TABLE` output columns onto `my_macro`'s `pg_proc` │
  │      tuple (or `plv8.plv8_single_col_holder` for 1-column outputs)        │
  │      so `addRangeTableEntryForFunction()` builds the exact `TupleDesc`    │
  │      without requiring `AS (...)`!                                        │
  └───────────────────────────────┬───────────────────────────────────────────┘
                                  ▼
  ┌───────────────────────────────────────────────────────────────────────────┐
  │ 2. PLAN TIME (`simplify_function` -> `plv8_call_support`)                 │
  │    • Functions declared with `SUPPORT plv8_call_support` invoke           │
  │      `plv8_call_support(internal)` during `eval_const_expressions`.       │
  │    • Collects `expected_columns` and expands preceding `FROM` RTEs in     │
  │      `root->parse->rtable` (and `parent_root`) via `expandRTE()` into     │
  │      `lateral_env`.                                                       │
  │    • Invokes the JS callback registered via                               │
  │      `plv8.register_support('my_macro', fn)` (or SQL catalog hook         │
  │      `<func>_support(...)`), which returns `{ sql, columns, lateral_vars }`│
  │    • Resets `my_macro`'s `pg_proc` tuple back to clean `RECORDOID`,       │
  │      stamps a rotating `LANGUAGE sql` helper (`plv8_inline_helper_0..7`)  │
  │      with `prosrc = sql` and `lateral_vars` as `Var` arguments (`$1..$N`),│
  │      and returns `makeFuncExpr(helper_oid, ...)`.                         │
  │    • PostgreSQL's `inline_set_returning_function()` immediately inlines   │
  │      the helper SQL function into an `RTE_SUBQUERY` and pulls it up into  │
  │      the outer join tree with full index/predicate pushdown!              │
  └───────────────────────────────┬───────────────────────────────────────────┘
                                  ▼
  ┌───────────────────────────────────────────────────────────────────────────┐
  │ 3. RUNTIME FALLBACK (`plv8_call_handler` -> `plv8.expected_columns()`     │
  │    & `plv8.return_query()`)                                               │
  │    • When arguments are non-constant runtime expressions, planner         │
  │      inlining returns `NULL` and PostgreSQL executes `my_macro` via       │
  │      `plv8_call_handler`.                                                 │
  │    • `CreateTupleStore()` accepts `RETURNS SETOF record` whenever         │
  │      `rsinfo->expectedDesc` has concrete column types (`!= RECORDOID`).   │
  │    • Inside JS, `plv8.expected_columns()` returns the caller's expected   │
  │      column metadata and `plv8.return_query(sql [, args])` streams SPI    │
  │      result tuples directly into the SRF `tuplestore` in C++.             │
  └───────────────────────────────────────────────────────────────────────────┘
```

### 11.2 SQL Catalog Objects Provided by Core `pl/v8` (`plv8.sql.common`)

1. **`plv8_source` Base Type (`CATEGORY = 'S'`)**:
   - Input/output functions: `plv8_source_in(cstring) RETURNS plv8_source` and `plv8_source_out(plv8_source) RETURNS cstring`.
   - Implicit casts: `text <-> plv8_source` and `varchar -> plv8_source`.
   - Language extensions can either use `plv8_source` directly as a parameter type or define a domain over it (e.g., `CREATE DOMAIN pgq_pattern AS plv8_source;`). Because `plv8_source` has `CATEGORY = 'S'` (`TYPCATEGORY_STRING`), it is always passed to JavaScript functions as a standard JS string (even when `pass_user_types_as_bytes = true`).
2. **`plv8_call_support(internal) RETURNS internal`**:
   - General-purpose PostgreSQL planner support function (`SUPPORT plv8_call_support`) handling `SupportRequestCost`, `SupportRequestRows`, and `SupportRequestSimplify`.
3. **`plv8.plv8_single_col_holder` Composite Type & `plv8_inline_helper_0..7`**:
   - `plv8.plv8_single_col_holder` is a 1-attribute composite type whose single attribute (`attnum = 1`) is dynamically updated in `pg_attribute` when a `RETURNS SETOF record` function infers **1 output column** without `AS (...)` (working around PostgreSQL's `build_function_result_tupdesc_d` rule that ignores `PROARGMODE_TABLE` when `numoutargs < 2`).
   - `plv8_inline_helper_0` through `plv8_inline_helper_7` are 8 rotating `LANGUAGE sql STABLE` helper slots used during `SupportRequestSimplify` to hand compiled SQL and `LATERAL` `Var` arguments to `inline_set_returning_function()`.

### 11.3 JavaScript API Reference (`plv8.*`)

| Function | Signature | Description |
|---|---|---|
| `plv8.expected_columns()` | `() => Array<{name, type_oid, typmod, collation_oid, type_sql}>` | Returns the expected output columns from `fcinfo->resultinfo->expectedDesc` inside any active set-returning function (`RETURNS SETOF record` or `RETURNS TABLE(...)`). |
| `plv8.return_query()` | `(sql: string, args?: Array<any>) => number` | Executes `sql` (with optional `$1..$n` parameters `args`) via SPI in C++ and streams all resulting `HeapTuple` rows directly into the active SRF `tuplestore` (converting column types automatically if needed). Returns the number of rows appended. |
| `plv8.register_column_inference()` | `(func_name: string, handler: Function \| string) => boolean` | Registers a parse-time column inference hook for `func_name` (triggered whenever a `plv8_source` or domain-over-`plv8_source` literal is parsed inside `func_name(...)`). `handler` can be an in-memory JS `Function(req)` or the name of a 2-arg SQL function `(query_ctx text, source_arg text) RETURNS text`. |
| `plv8.register_support()` | `(func_name: string, handler: Function \| string) => boolean` | Registers a plan-time `prosupport` compilation hook for `func_name` (when declared with `SUPPORT plv8_call_support`). `handler` can be an in-memory JS `Function(req)` or the name of a 5-arg SQL function `(first_arg text, source_arg text, expected_cols_json text, lateral_env_json text, params_json text) RETURNS text`. |
| `plv8.set_function_columns()` | `(func_name: string, columns: Array<{name, type_oid?, type?, typmod?, collation_oid?}>) => boolean` | Explicitly stamps `PROARGMODE_TABLE` output columns onto `func_name`'s `pg_proc` entry (or resets it to `RECORDOID` when `columns` is empty/`null`). Synchronizes `plv8_proc_cache` (`fn_xmin`/`fn_tid`) so the V8 function cache stays warm. |

---

## 12. Performance Characteristics & Trade-Offs

| Operation | Cold Start (First Call in Backend) | Warm Call (Cached in `plv8_proc_cache`) | Mechanism |
|---|---|---|---|
| **Transpilation (`plv8_transpile_src`)** | ~0.2 – 3.0 ms (runs JS handler once) | **0 ns** (skipped) | Cached in `plv8_proc_cache::function`; validated via `fn_xmin`/`fn_tid` syscache check. |
| **Handler Chain Validation (`plv8_cache_handlers_valid`)** | ~1 – 2 µs per handler in chain | ~1 – 2 µs per handler in chain | In-memory `SearchSysCache1(PROCOID, ...)` lookup per handler in `cache->handlers`. |
| **WASM Module Compilation (`plv8.compile_wasm_cached`)** | ~2 – 15 ms (first backend in cluster) | **~50 – 200 µs** (other backends via `DSA` + `dshash`) | Serialized V8 TurboFan/Liftoff machine code shared across all PostgreSQL backends. |
| **Custom Base Type I/O (`pass_user_types_as_bytes`)** | N/A | **Zero text parsing** (`memcpy` of varlena payload) | Bypasses `OidOutputFunctionCall` / `OidInputFunctionCall` for `TYPCATEGORY_USER`. |
