# PLV8 Built-ins

PLV8 includes a number of built-in functions bound to the `plv8` object for you
to use.

## Utility Functions

### `plv8.elog`

`plv8.elog` emits a message to the client or the PostgreSQL log file.  The
emit level is one of:

* `DEBUG5`
* `DEBUG4`
* `DEBUG3`
* `DEBUG2`
* `DEBUG1`
* `LOG`
* `INFO`
* `NOTICE`
* `WARNING`
* `ERROR`

```
var msg = 'world';

plv8.elog(DEBUG1, 'Hello', `${msg}!`);
```

See the [PostgreSQL manual](https://www.postgresql.org/docs/current/static/runtime-config-logging.html#RUNTIME-CONFIG-SEVERITY-LEVELS) for information on each error level.

### `plv8.quote_literal`, `plv8.nullable`, `plv8.quote_ident`

Each function for the quote family is identical to the built-in SQL function
with the same name.

### `plv8.find_function`

PLV8 provides a function to access other functions defined as `plv8` functions
that have been registered in the database.

```
CREATE FUNCTION callee(a int) RETURNS int AS $$ return a * a $$ LANGUAGE plv8;
CREATE FUNCTION caller(a int, t int) RETURNS int AS $$
  var func = plv8.find_function("callee");
  return func(a);
$$ LANGUAGE plv8;
```

With `plv8.find_function()`, you can look up other PLV8 functions. If they
are not a PLV8 function, and error is thrown. The function signature parameter
to `plv8.find_function()` is either of `regproc` (function name only) or
`regprocedure` (function name with argument types). You can make use of the
internal type for arguments and void type for return type for the pure Javascript
function to make sure any invocation from SQL statements should not occur.

### `plv8.version`

The `plv8` object provides a version string as `plv8.version`.  This string
corresponds to the `plv8` module version.

### `plv8.memory_usage`

You can get your own memory usage by calling `plv8.memory_usage()` with no params.
The resulting object looks like this:
```json
{
  "total_heap_size":1327104,
  "total_physical_size":472712,
  "used_heap_size":381748,
  "heap_size_limit":270008320,
  "external_memory":0,
  "number_of_native_contexts":2
}
```

See nodejs [v8.getHeapStatistics()](https://nodejs.org/api/v8.html#v8_v8_getheapstatistics)

### `plv8.run_script`

Run a script from source code, it's like `eval()` but takes a second argument: script name  
Can be pretty useful for debugging

Can be used like this
```js
const sourceCode = `globalThis.myFunc = () => 42`
try {
    plv8.run_script(sourceCode, 'myScript.js')
    myFunc()
} catch (e) {
    plv8.elog(NOTICE, e.message)
}
```

## Database Access via SPI

PLV8 provides functions for database access, including prepared statements,
and cursors.

### `plv8.execute`

`plv8.execute(sql [, args])`

Executes SQL statements and retrieves the results.  The `sql` argument is
required, and the `args` argument is an optional `array` containing any arguments
passed in the SQL query.  For `SELECT` queries, the returned value is an `array`
of `objects`.  Each `object` represents one row, with the `object` properties
mapped as column names.  For non-`SELECT` queries, the return result is the
number of rows affected.

```
var json_result = plv8.execute('SELECT * FROM tbl');
var num_affected = plv8.execute('DELETE FROM tbl WHERE price > $1', [ 1000 ]);
```

### `plv8.prepare`

`plv8.prepare(sql [, typenames])`

Opens or creates a prepared statement.  The `typename` parameter is an `array`
where each element is a `string` that corresponds to the PostgreSQL type name
for each `bind` parameter.  Returned value is an object of the `PreparedPlan` type.
This object must be freed by `plan.free()` before leaving the function.

```
var plan = plv8.prepare('SELECT * FROM tbl WHERE col = $1', [ 'int' ]);
var rows = plan.execute([ 1 ]);
var sum = 0;
for (var i = 0; i < rows.length; i++) {
  sum += rows[i].num;
}
plan.free();

return sum;
```

### `PreparedPlan.execute`

`PreparedPlan.execute([ args ])`

Executes the prepared statement.  The `args` parameter is the same as what would be
required for `plv8.execute()`, and can be omitted if the statement does not have
any parameters.  The result of this method is also the same as `plv8.execute()`.

### `PreparedPlan.cursor`

`PreparedPlan.cursor([ args ])`

Opens a cursor form the prepared statement.  The `args` parameter is the same as
what would be required for `plv8.execute()` and `PreparedPlan.execute()`.  The
returned object is of type `Cursor`.  This must be closed by `Cursor.close()`
before leaving the function.

```
var plan = plv8.prepare('SELECT * FROM tbl WHERE col = $1', [ 'int' ]);
var cursor = plan.cursor([ 1 ]);
var sum = 0, row;
while (row = cursor.fetch()) {
    sum += row.num;
}
cursor.close();
plan.free();

return sum;
```

### `PreparedPlan.free`

Frees the prepared statement.

### `Cursor.fetch`

`Cursor.fetch([ nrows ])`

When the `nrows` parameter is omitted, fetches a row from the cursor and returns
it as an `object` (note: not as an `array`).  If specified, fetches as many rows
as the `nrows` parameter, up to the number of rows available, and returns an
`array` of `objects`.  A negative value will fetch backward.

### `Cursor.move`

`Cursor.move(nrows)`

Moves the cursor `nrows`.  A negative value will move backward.

### `Cursor.close`

Closes the `Cursor`.

### `plv8.subtransaction`

`plv8.subtransaction(func)`

`plv8.execute()` creates a subtransaction each time it executes.  If you need
an atomic operation, you will need to call `plv8.subtransaction()` to create
a subtransaction block.

```
try{
  plv8.subtransaction(function(){
    plv8.execute("INSERT INTO tbl VALUES(1)"); // should be rolled back!
    plv8.execute("INSERT INTO tbl VALUES(1/0)"); // occurs an exception
  });
} catch(e) {
  ... execute fall back plan ...
}
```

If one of the SQL execution in the subtransaction block fails, all of operations
within the block are rolled back. If the process in the block throws a Javascript
exception, it is carried forward. So use a `try ... catch` block to capture it and
do alternative operations if it occurs.

## Window Function API

You can define user-defined window functions with PLV8. It wraps the C-level
window function API to support full functionality. To create one, first obtain a
window object by calling `plv8.get_window_object()`, which provides the following
interfaces:

### `WindowObject.get_current_position`

Returns the current position in the partition, starting from `0`.

### `WindowObject.get_partition_row_count`

Returns the number of rows in the partition.

### `WindowObject.set_mark_position`

`WindowObject.set_mark_position(pos)`

Sets the mark at the specified row.  Rows above this position will be gone and
no longer accessible later.

### `WindowObject.rows_are_peers`

`WindowObject.rows_are_peers(pos1, pos1)`

Returns `true` if the rows at `pos1` and `pos2` are peers.

### `WindowObject.get_func_arg_in_partition`

`WindowObject.get_func_arg_in_partition(argno, relpos, seektype, mark_pos)`

### `WindowObject.get_func_arg_in_frame`

`WindowObject.get_func_arg_in_frame(argno, relpos, seektype, mark_pos)`

Returns the value of the argument in `argno` (starting from 0) to this function
at the `relpos` row from `seektype` in the current partition or frame.
`seektype` can be either of `WindowObject.SEEK_HEAD`, `WindowObject.SEEK_CURRENT`,
or `WindowObject.SEEK_TAIL`. If `mark_pos` is `true`, the row the argument is
fetched from is marked. If the specified row is out of the partition/frame, the
returned value will be undefined.


### `WindowObject.get_func_arg_in_current`

`WindowObject.get_func_arg_in_current(argno)`

Returns the value of the argument in `argno` (starting from 0) to this function
at the current row. Note that the returned value will be the same as the argument
variable of the function.

### `WindowObject.get_partition_local`

`WindowObject.get_partition_local([ size ])`

Returns partition-local value, which is released at the end of the current
partition. If nothing is stored, undefined is returned. size argument (default
1000) is the byte size of the allocated memory in the first call. Once the memory
is allocated, the size will not change.

### `WindowObject.set_partition_local`

`WindowObject.set_partition_local(obj)`

Stores the partition-local value, which you can retrieve later with
`get_partition_local()`. This function internally uses `JSON.stringify()` to serialize the object, so if you pass a value that is not able to be serialized
it may end up being an unexpected value. If the size of a serialized value is
more than the allocated memory, it will throw an exception.

You can also learn more on how to use these API in the `sql/window.sql`
regression test, which implements most of the native window functions. For
general information on the user-defined window function, see the [CREATE FUNCTION page of the PostgreSQL manual](https://www.postgresql.org/docs/current/static/sql-createfunction.html).
