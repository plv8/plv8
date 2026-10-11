# PostgreSQL-WASM Developer Guide: Compiling Rust, Go, and TinyGo Extensions (`pl/v8-wasm`)

`plv8` and `pg_tle` provide a native WebAssembly (`LANGUAGE "pl/v8-wasm"`) execution engine backed by V8's Liftoff/TurboFan WebAssembly compiler and a cross-backend shared memory (`DSA` + `dshash`) native machine code cache.

This guide covers how to compile and package WebAssembly functions, custom PostgreSQL base types (`CREATE TYPE`), custom operators (`CREATE OPERATOR`), and SPI database queries (`postgres.spi_exec`) in:

1. **Rust** (`wasm32-unknown-unknown` `cdylib` and `wasm-bindgen`) — see [`pg_tle/docs/10_wasm_rust_examples.md`](../../pg_tle/docs/10_wasm_rust_examples.md)
2. **Standard Go (`go` 1.24+)** (`GOOS=wasip1 GOARCH=wasm -buildmode=c-shared`) — see [`pg_tle/docs/11_wasm_go_tinygo_examples.md`](../../pg_tle/docs/11_wasm_go_tinygo_examples.md)
3. **TinyGo (`tinygo` 0.33+)** (`-target=wasi -buildmode=c-shared` and `-target=wasm-unknown`) — see [`pg_tle/docs/11_wasm_go_tinygo_examples.md`](../../pg_tle/docs/11_wasm_go_tinygo_examples.md)

---

## 1. Architecture & Execution Model

1. **Registration & Packaging (`tle-pack wasm` / `pgtle.install_wasm_extension`)**:
   - Validates the 8-byte WebAssembly magic header (`\0asm\x01\x00\x00\x00`).
   - Stores the `.wasm` bytecode in `pgtle.modules` (and `public.plv8_wasm_modules`) with `is_trusted = true`.
   - Either parses the binary's Type (`0x01`), Function (`0x03`), and Export (`0x07`) sections automatically or reads your JSON manifest to generate `LANGUAGE "pl/v8-wasm"` SQL functions, custom PostgreSQL base types (`pgtle.create_base_type_if_not_exists`), and custom operators (`CREATE OPERATOR`).
2. **Cross-Backend Shared Native Code Cache (`plv8_wasm_shared_cache`)**:
   - The first backend to compile a `.wasm` module serializes the V8 native machine code (`CompiledWasmModule::Serialize()`) into PostgreSQL dynamic shared memory (`DSA`) indexed by a `dshash` table keyed by the FNV-1a 64-bit hash of the `.wasm` bytecode.
   - All subsequent PostgreSQL backends deserialize the compiled native code directly from shared memory in microseconds, skipping WebAssembly compilation completely.
3. **WASI Preview 1 & Reactor Initialization**:
   - On instantiation, `plv8` and `pg_tle` attach `wasi_snapshot_preview1`, `wasi_unstable`, `postgres`, and `env` import namespaces.
   - If the module exports `_initialize()` (standard for Go `GOOS=wasip1 -buildmode=c-shared` and TinyGo `-target=wasi -buildmode=c-shared` WASI reactors), it is automatically called once when the `WebAssembly.Instance` is created.
4. **Automatic Trap Recovery**:
   - If a WebAssembly execution traps (`RuntimeError: unreachable` or out-of-bounds memory access), the cached `WebAssembly.Instance` is evicted so that the next SQL call cleanly re-instantiates a fresh instance from the cached `WebAssembly.Module`.

---

## 2. Complete ABI Specification

### 2.1 Scalar Types

| PostgreSQL Type | WebAssembly Type | Rust Type | Go / TinyGo Type |
| :--- | :--- | :--- | :--- |
| `int2` (`smallint`) | `i32` | `i16` / `i32` | `int32` |
| `int4` (`integer`) | `i32` | `i32` | `int32` |
| `int8` (`bigint`) | `i64` | `i64` | `int64` |
| `float4` (`real`) | `f32` | `f32` | `float32` |
| `float8` (`double precision`) | `f64` | `f64` | `float64` |
| `bool` (`boolean`) | `i32` | `i32` (`0` or `1`) | `int32` (`0` or `1`) |

### 2.2 Linear Memory Buffer Types (`text`, `bytea`, and Custom Base Types)

- **Required Allocator Exports**:
  Export either:
  - `alloc(size: i32) -> i32` and `dealloc(ptr: i32, size: i32)` (recommended for Rust and Go/TinyGo), or
  - `malloc(size: i32) -> i32` and `free(ptr: i32)` (standard C/libc).
  The runtime strictly pairs `alloc` with `dealloc` and `malloc` with `free`.
- **Input Arguments (`(ptr: i32, len: i32)` or `ptr_only`)**:
  - For `text`, `bytea`, and variable-length custom types (`internallength: -1`), each argument is UTF-8 encoded (for `text`) or copied as raw bytes (for `bytea`/custom types) into linear memory via `alloc(len + 1)` (with a trailing NUL byte) and passed as two `i32` parameters `(ptr, len)` (unless `"arg_mode": "ptr_only"` is set).
  - For fixed-size custom base types (`internallength > 0`, e.g., `8` for a 2D `int4` vector), operator and helper functions that have 1 WebAssembly parameter per SQL argument automatically receive `ptr: i32` pointing to the `internallength`-byte representation in linear memory.
  - After the call returns, the runtime frees every input buffer via `dealloc(ptr, len + 1)`.
- **Return Value Conventions**:
  1. **Length-prefixed `i32` pointer (`-> i32`)**:
     Return a pointer `ptr` to linear memory where `[ptr .. ptr+4]` holds the 32-bit little-endian byte length `N` and `[ptr+4 .. ptr+4+N]` holds the `N` payload bytes. Returning `0` produces SQL `NULL`.
     - **Important**: The host copies `[ptr+4 .. ptr+4+N]` out of linear memory immediately and does **not** free the return pointer. Reuse a static buffer in guest memory (or return a packed `i64` pointing to a static buffer) to avoid leaking guest heap memory across calls.
  2. **Fixed-size custom base type `i32` pointer (`-> i32` when `internallength > 0`)**:
     When returning a fixed-size custom base type (`internallength > 0`), returning `ptr: i32` reads `internallength` raw bytes directly from `[ptr .. ptr + internallength]`. Returning `0` produces SQL `NULL`.
  3. **Packed `i64` (`-> i64`)**:
     - Default (`"return_mode": "auto"`): `((len as u64) << 32) | (ptr as u32 as u64)` (upper 32 bits = byte length, lower 32 bits = pointer).
     - `"return_mode": "ptr_len"`: `((ptr as u32 as u64) << 32) | (len as u32 as u64)` (upper 32 bits = pointer, lower 32 bits = byte length).
     Returning `0` produces SQL `NULL`.

### 2.3 Host Imports (`"postgres"` Namespace)

| Import Name | Signature | Description |
| :--- | :--- | :--- |
| `postgres.spi_exec` | `(sql_ptr: i32, sql_len: i32, out_ptr: i32, out_cap: i32) -> i32` | Executes SQL via `plv8.execute(sql)`, UTF-8 encodes the JSON array result into `out_ptr[0..out_cap]`, and returns the total UTF-8 byte length (if the return value exceeds `out_cap`, resize `out_ptr` and re-query or pass `out_cap = 0` first to probe the length). |
| `postgres.spi_exec` (2-arg) | `(sql_ptr: i32, sql_len: i32) -> i64` | Executes SQL via `plv8.execute(sql)`, allocates `len` result bytes in guest memory via `alloc(len, 1)`/`malloc(len)`, and returns packed `(len << 32) \| ptr`. |
| `postgres.elog_notice` | `(msg_ptr: i32, msg_len: i32)` | Emits a PostgreSQL `NOTICE` message. |
| `postgres.elog_error` | `(msg_ptr: i32, msg_len: i32)` | Raises a PostgreSQL `ERROR` exception. |

---

## 3. Manifest Schema (`manifest.json`)

Both `pg_tle/tools/tle-pack wasm` and `pgtle.install_wasm_extension()` accept the following JSON manifest structure (supporting both canonical keys and shorthand aliases):

```json
{
  "types": [
    {
      "name": "rust_vec2i",
      "internallength": 8,
      "alignment": "int4",
      "storage": "plain",
      "input_export": "vec2i_in",
      "output_export": "vec2i_out",
      "operators": [
        {
          "name": "rust_vec2i_add",
          "export": "vec2i_add",
          "leftarg": "rust_vec2i",
          "rightarg": "rust_vec2i",
          "returns": "rust_vec2i",
          "operator": "+"
        },
        {
          "name": "rust_vec2i_eq",
          "export": "vec2i_eq",
          "leftarg": "rust_vec2i",
          "rightarg": "rust_vec2i",
          "returns": "boolean",
          "operator": "=",
          "commutator": "=",
          "negator": "<>"
        }
      ]
    }
  ],
  "functions": [
    {
      "name": "rust_add_i32",
      "export": "add_i32",
      "args": [
        {"name": "a", "type": "integer"},
        {"name": "b", "type": "integer"}
      ],
      "returns": "integer",
      "volatility": "IMMUTABLE",
      "strict": true
    }
  ]
}
```

Supported key aliases in `manifest.json`:
- Type input/output exports: `input_export` / `input_func` / `input` and `output_export` / `output_func` / `output`.
- Function & operator return types: `returns` / `return_type` / `result`.
- Operator symbol: `operator` / `symbol`.

---

## 4. Quick Reference: Compiler & Packaging Commands

### 4.1 Rust (`wasm32-unknown-unknown` `cdylib` or `wasm-bindgen`)

```bash
# Raw cdylib workflow:
cargo build --target wasm32-unknown-unknown --release
pg_tle/tools/tle-pack wasm \
  --name rust_wasm_ext \
  --version 1.0.0 \
  --wasm target/wasm32-unknown-unknown/release/rust_wasm_ext.wasm \
  --manifest manifest.json \
  --output rust_wasm_ext.sql

# wasm-bindgen workflow:
python3 pg_tle/tools/pgwasm_pack.py \
  --crate-dir ./my_rust_udf \
  --module-name my_rust_udf \
  --version 1.0.0 \
  --as-extension \
  --out my_rust_udf.sql
```

### 4.2 Standard Go (`GOOS=wasip1 GOARCH=wasm`)

```bash
GOOS=wasip1 GOARCH=wasm go build \
  -buildmode=c-shared \
  -ldflags="-s -w" \
  -o stdgo_ext.wasm \
  main.go

pg_tle/tools/tle-pack wasm \
  --name stdgo_ext \
  --version 1.0.0 \
  --wasm stdgo_ext.wasm \
  --manifest manifest.json \
  --output stdgo_ext.sql
```

### 4.3 TinyGo (`-target=wasi` or `-target=wasm-unknown`)

```bash
# Option A: TinyGo WASI Reactor (~200 KB)
tinygo build -target=wasi -buildmode=c-shared -no-debug -o tinygo_wasi.wasm main.go

# Option B: TinyGo Freestanding (~80 KB, zero WASI imports)
tinygo build -target=wasm-unknown -no-debug -o tinygo_unknown.wasm main.go

pg_tle/tools/tle-pack wasm \
  --name tinygo_ext \
  --version 1.0.0 \
  --wasm tinygo_unknown.wasm \
  --manifest manifest.json \
  --output tinygo_ext.sql
```
