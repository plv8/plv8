# Runtime Configuration

PLV8 has some configuration variables that can be set either in `postgresql.conf`
or at runtime using `SET`.

|Variable|Description|Default|
|--------|-----------|-------|
|`plv8.start_proc`|PLV8 function to run once when PLV8 is first used|_none_|
|`plv8.icu_data`|ICU data file directory (when compiled with ICU support)|_none_|
|`plv8.v8_flags`|V8 engine initialization flags (e.g. --harmony for all current harmony features)|_none_|
|`plv8.execution_timeout`|V8 execution timeout (when compiled with EXECUTION_TIMEOUT)|300 seconds|
