# Runtime Configuration

PLV8 has some configuration variables that can be set either in `postgresql.conf`
or at runtime using `SET`.

|Variable|Description|Default|
|--------|-----------|-------|
|`plv8.start_proc`|PLV8 function to run once when PLV8 is first used|_none_|
|`plv8.icu_data`|ICU data file directory (when compiled with ICU support)|_none_|
|`plv8.v8_flags`|V8 engine initialization flags (e.g. --harmony for all current harmony features)|_none_|
|`plv8.execution_timeout`|V8 execution timeout (when compiled with EXECUTION_TIMEOUT)|300 seconds|
|`plv8.boot_proc`|Like `start_proc` above, but can be set by superuser only|_none_|
|`plv8.memory_limit`|Memory limit for the per-user heap usage on each connection, in **MB**|256|
|`plv8.context`|Users can switch to a different global object (`globalThis`) by using an arbitrary context string|_none_|
|`plv8.context_cache_size`|Size of the per-user LRU cache for custom contexts|8|
|`plv8.max_eval_size`|Control how `eval()` can be used, -1 = no limits, 0 = `eval()` disabled, any other number = max length of the eval-able string in **bytes**|2MB|
