# Runtime Configuration

PLV8 has some configuration variables that can be set either in `postgresql.conf`
or at runtime using `SET`.

| Variable                     | Context           | Description                                                                                                              | Default     |
| ---------------------------- | ----------------- | ------------------------------------------------------------------------------------------------------------------------ | ----------- |
| `plv8.start_proc`            | `PGC_USERSET`     | PLV8 function to run once when PLV8 is first used                                                                        | _none_      |
| `plv8.icu_data`              | `PGC_USERSET`     | ICU data file directory (when compiled with ICU support)                                                                 | _none_      |
| `plv8.v8_flags`              | `PGC_USERSET`     | V8 engine initialization flags (e.g. --harmony for all current harmony features)                                         | _none_      |
| `plv8.execution_timeout`     | `PGC_USERSET`     | V8 execution timeout (when compiled with EXECUTION_TIMEOUT)                                                              | 300 seconds |
| `plv8.memory_limit`          | `PGC_USERSET`     | Memory limit for the per-user heap usage on each connection, in **MB**                                                   | 256         |
| `plv8.allow_raw_user_types`  | `PGC_SUSET`       | Superuser-only toggle allowing superuser-owned language handlers (`pass_user_types_as_bytes`) to pass/return raw `TYPCATEGORY_USER` varlena bytes | `on`        |
| `plv8.wasm_cache_size`       | `PGC_POSTMASTER`  | Maximum shared memory size (in **MB**) for the cross-backend compiled WebAssembly native code cache (`shared_preload_libraries`) | 64          |
| `plv8.snapshot_file`         | `PGC_POSTMASTER`  | Path to a pre-built V8 startup snapshot file (`PLV8SN02`) loaded in the postmaster (`shared_preload_libraries`)          | _none_      |
| `plv8.boot_script_file`      | `PGC_POSTMASTER`  | Path to a JS boot script compiled into a V8 startup snapshot in the postmaster (`shared_preload_libraries`)              | _none_      |
