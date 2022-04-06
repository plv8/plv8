PL/v8
=====
PL/v8 is a trusted procedural language that is safe to use, fast to run and
easy to develop, powered by V8 JavaScript Engine. The PL/v8 project is maintained
at [https://github.com/plv8/plv8](https://github.com/plv8/plv8).

## Table of Contents
The documentation covers the following implemented features:

- [Requirements](#requirements)
- [Installing PL/v8](#installing-plv8)
- [Install the PL/v8 Extensions on a Database](#install-the-plv8-extensions-on-a-database)
- [Scalar function calls](#scalar-function-calls)
- [Set returing function calls](#set-returning-function-calls)
- [Trigger function calls](#trigger-function-calls)
- [Inline statement calls](#inline-statement-calls)
- [Auto mapping between JS and database built-in types](#auto-mapping-between-js-and-database-built-in-types)
- [Database access via SPI including prepared statements and cursors](#database-access-via-spi-including-prepared-statements-and-cursors)
- [Subtransaction](#subtransaction)
- [Utility functions](#utility-functions)
- [Window function API](#window-function-api)
- [Typed array](#typed-array)
- [ES6 Language Features](#es6-language-features)
- [Runtime environment separation across users in the same session](#runtime-environment-separation-across-users-in-the-same-session)
- [Start-up procedure](#start-up-procedure)
- [Update procedure](#update-procedure)
- [Dialects](#dialects)

## Requirements:
PL/v8 is tested with:

- PG: version 9.2, 9.3, 9.4 and 9.5 (maybe older/newer are allowed)
- V8: version 4.4 to 5.4
- g++: version 4.8.2
- clang++

Here is an example of dependencies that will likely be used when building on Ubuntu 20.04 LTS (change postgresql-server-dev-12 to your version of postgres)
```bash
   sudo apt install g++ python build-essential autoconf automake gdb git libffi-dev zlib1g-dev libssl-dev apt-file libtinfo5 postgresql-server-dev-12 ninja-build
```

To install the V8 pre-requisites, see https://github.com/v8/v8 . Be aware that it can take a long time to build V8 from source.

Also all tools that PostgreSQL and V8 require to be built are required if you
are building those from source.

## Installing PL/v8

### Build from source:
Determine the [PL/v8 release](https://github.com/plv8/plv8/releases or https://github.com/plv8/plv8/tags) you want to download and use it's version and path below.

    $ wget https://github.com/plv8/plv8/archive/v3.X.X.tar.gz
    $ tar -xvzf v3.X.X.tar.gz
    $ cd plv8-3.X.X
    $ sudo make

This will build PL/v8 for you linking to Google's v8 as a static library by
downloading the v8 source at a specific version and building it along with
PL/v8. The build will be for the highest PostgreSQL version you have installed
on the system. You can alternatively run `make -f Makefile.shared` and it will
build PL/v8 dynamically linking to Google's `libv8` library on your system.
There are some issues with this as several linux distros ship a very old version
of `libv8`. The `3.x` versions of v8 will work with the `1.4.x` versions of PL/v8,
but to build the later versions of PL/v8 you need a v8 minimum version of
`4.4.63.31`, but can also use v8 version `6.4.388.40`. PGXN
install will use the statically linked `libv8` library.

If you would like to use `make -f Makefile.shared` and your system does not have
a new enough version of `libv8` installed, see the `Makefile` file in the repo
to see how to build v8 natively.

> Note: If you have multiple versions of PostgreSQL installed like 9.5 and 9.6,
PL/v8 will only be built for PostgreSQL 9.6. This is because `make` calls
`pg_config` to get the version number, which will always be the latest version
installed. If you need to build PL/v8 for PostgreSQL 9.5 while you have 9.6
installed pass `make` the `PG_CONFIG` variable to your 9.5 version of
`pg_config`. This works for `make`, `make -f Makefile.shared`, `make install`. For example
in Ubuntu:

    $ make PG_CONFIG=/usr/lib/postgresql/9.5/bin/pg_config

> Note: You may run into problems with your C++ complier version. You can pass
`make` the `CUSTOM_CC` variable to change the complier. For example, to use
`g++` version 4.9:

    $ make CUSTOM_CC g++-4.9

> Note: In `mingw64`, you may have difficulty in building PL/v8. If so, try to
make the following changes in Makefile. For more detail, please refer to
https://github.com/plv8/plv8/issues/29

    CUSTOM_CC = gcc
    SHLIB_LINK := $(SHLIB_LINK) -lv8 -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -lm

### Building with Execution Timeout

PL/v8 allows you to optionally build with an execution timeout for Javascript
functions, when enabled at compile-time.

    $ make -DEXECUTION_TIMEOUT

By default, the execution timeout is not compiled, but when configured it has
a timeout of `300 seconds` (5 minutes).  You can override this by setting the
`plv8.execution_timeout` variable.  It can be set between `1 second` and
`65536` seconds, but cannot be disabled.

### Installing the build:
After running `make` or `make static` the following files must be copied to the
correct location for PostgreSQL to find them:
#### PL/v8 JavaScript Extension:
- `plv8.so`
- `plv8.control`
- `plv8--{plv8-build-version-here}.sql`

By default, PL/v8 will not compile v8's ICU support.  If you need ICU support,
you will need to specify it at build time:

    $ make -DUSE_ICU

The following files will also be built and can be optionally installed if you
need the CoffeeScript or LiveScript versions:
#### CoffeeScript Extension:
- plcoffee.control
- plcoffee--{plv8-build-version-here}.sql

#### LiveScript Extension:
- plls.control
- plls--{plv8-build-version-here}.sql

### Automatically Install the Build
You can install the build for your system by running:

    $ make install

> Note: You should do this a root/admin. `sudo make install`

> Note: If you need to install PL/v8 for a different version of PostgreSQL, pass
the `PG_CONFIG` variable. See above.

### Test the Install
PL/v8 supports installcheck test.  Make sure to set `custom_variable_classes = 'plv8'`
in your postgresql.conf (before 9.2) and run:

    $ make installcheck

### Debian/Ubuntu 14.04 and 16.04:
You can install PL/v8 using `apt-get`, but it will be version `v1.4.8`
(As of 2016-12-16).

    $ apt-get install postgresql-{your-postgresql-version-here}-plv8
    # e.g.
    $ apt-get install postgresql-9.1-plv8
    # OR up to
    $ apt-get install postgresql-9.6-plv8

### Redhat/CentOS:

This guide assumes you are using the [pgdg yum repository](https://yum.postgresql.org/repopackages.php).

    $ yum install postgresql(your-postgresql-version-here)-server postgresql(your-postgresql-version-here)-devel
    $ make static PG_CONFIG=/usr/pgsql-(your-postgresql-version-here)/bin/pg_config
    $ sudo make install

### MacOS:

TODO

### Windows:

    $ bootstrap.bat
    $ cmake . -G "Visual Studio 15 2017 Win64" -DCMAKE_INSTALL_PREFIX="C:\Program Files\PostgreSQL\9.6" -DPOSTGRESQL_VERSION=9.6
    $ cmake --build . --config Release --target Package

Unzip it, and copy to PostgreSQL directories.


## Install the PL/v8 Extensions on a Database:
Once the PL/v8 extensions have been added to the server, you should restart the
PostgreSQL service. Then you can connect to the server and install the extensions
on a database by running the following SQL queries on PostgreSQL version 9.1 or
later:

    CREATE EXTENSION plv8;
    CREATE EXTENSION plls;
    CREATE EXTENSION plcoffee;

Make sure to set `custom_variable_classes = 'plv8'` in your `postgresql.conf` file
for PostgreSQL versions before 9.2.

In the versions prior to 9.1 run the following to create database objects:

    $ psql -f plv8.sql

### Testing PL/v8 on a database:
Below are some example queries to test if the extension is working:

    DO $$
      plv8.elog(WARNING, 'plv8.version = ' + plv8.version); // Will output the PL/v8 installed as a PostgreSQL `WARNING`.
    $$ LANGUAGE plv8;

As of 2.0.0, there is a function to determine which version of PL/v8 you have
installed:

    SELECT plv8_version();

#### JavaScript Example

    CREATE OR REPLACE FUNCTION plv8_test(keys text[], vals text[])
    RETURNS text AS $$
      var o = {};
      for(var i=0; i<keys.length; i++){
        o[keys[i]] = vals[i];
      }
      return JSON.stringify(o);
    $$ LANGUAGE plv8 IMMUTABLE STRICT;

    SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
             plv8_test
    ---------------------------
     {"name":"Tom","age":"29"}
    (1 row)

#### CoffeeScript Example

    CREATE OR REPLACE FUNCTION plcoffee_test(keys text[], vals text[])
    RETURNS text AS $$
      return JSON.stringify(keys.reduce(((o, key, idx) ->
        o[key] = vals[idx]; return o), {}), {})
    $$ LANGUAGE plcoffee IMMUTABLE STRICT;

    SELECT plcoffee_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
           plcoffee_test
    ---------------------------
     {"name":"Tom","age":"29"}
    (1 row)

#### LiveScript Example

    CREATE OR REPLACE FUNCTION plls_test(keys text[], vals text[])
    RETURNS text AS $$
      return JSON.stringify { [key, vals[idx]] for key, idx in keys }
    $$ LANGUAGE plls IMMUTABLE STRICT;

    SELECT plls_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
             plls_test
    ---------------------------
     {"name":"Tom","age":"29"}
    (1 row)

## Scalar function calls
In PL/v8, you can write your SQL invoked function in JavaScript. Use the usual
`CREATE FUNCTION` statement with a JS function body. Here is an example of a
scalar function call.

    CREATE FUNCTION plv8_test(keys text[], vals text[]) RETURNS text AS $$
        var o = {};
        for(var i=0; i<keys.length; i++){
            o[keys[i]] = vals[i];
        }
        return JSON.stringify(o);
    $$ LANGUAGE plv8 IMMUTABLE STRICT;

    SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
    SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
             plv8_test
    ---------------------------
     {"name":"Tom","age":"29"}
    (1 row)

The function will be internally defined such that:

    (function(arg1, arg2, ..){
       $funcbody$
    })

Where `$funcbody$` is the script source you specify in the `CREATE FUNCTION AS`
clause. The argument names are inherited from the `CREATE FUNCTION` statement
or they will be named as `$1`, `$2` if the names are omitted.

## Set returning function calls
PL/v8 supports `SET` returning function calls.

    CREATE TYPE rec AS (i integer, t text);
    CREATE FUNCTION set_of_records() RETURNS SETOF rec AS
    $$
        // plv8.return_next() stores records in an internal tuplestore,
        // and return all of them at the end of function.
        plv8.return_next( { "i": 1, "t": "a" } );
        plv8.return_next( { "i": 2, "t": "b" } );

        // You can also return records with an array of JSON.
        return [ { "i": 3, "t": "c" }, { "i": 4, "t": "d" } ];
    $$
    LANGUAGE plv8;

    SELECT * FROM set_of_records();
     i | t
    ---+---
     1 | a
     2 | b
     3 | c
     4 | d
    (4 rows)

If the function is declared as `RETURNS SETOF`, PL/v8 prepares a tuplestore every
time called. You can call `plv8.return_next()` function to add as many results as
you like to return rows from this function. Alternatively, you can just return
a JS array to add set of records, a JS object to add a record, or a scalar value
to add a scalar to the tuplestore. Unlike other PLs, PL/v8 does not support
the per-value return strategy, but it always uses the tuplestore strategy.
If the argument object has extra properties that are not defined by the argument,
`return_next` raises an error.

## Trigger function calls
PL/v8 supports trigger function calls.

    CREATE FUNCTION test_trigger() RETURNS trigger AS
    $$
        plv8.elog(NOTICE, "NEW = ", JSON.stringify(NEW));
        plv8.elog(NOTICE, "OLD = ", JSON.stringify(OLD));
        plv8.elog(NOTICE, "TG_OP = ", TG_OP);
        plv8.elog(NOTICE, "TG_ARGV = ", TG_ARGV);
        if (TG_OP == "UPDATE") {
            NEW.i = 102;
            return NEW;
        }
    $$
    LANGUAGE "plv8";

    CREATE TRIGGER test_trigger
        BEFORE INSERT OR UPDATE OR DELETE
        ON test_tbl FOR EACH ROW
        EXECUTE PROCEDURE test_trigger('foo', 'bar');

If the trigger type is an `INSERT` or `UPDATE`, you can assign properties of `NEW`
variable to change the actual tuple stored by this operation.

A PL/v8 trigger function will have the following special arguments that contain
the trigger state:

- `NEW`
- `OLD`
- `TG_NAME`
- `TG_WHEN`
- `TG_LEVEL`
- `TG_OP`
- `TG_RELID`
- `TG_TABLE_NAME`
- `TG_TABLE_SCHEMA`
- `TG_ARGV`

For each variable semantics, see the [trigger section in PostgreSQL manual](https://www.postgresql.org/docs/current/static/plpgsql-trigger.html).

## Inline statement calls
PL/v8 supports `DO` block with PostgreSQL 9.0 and above.

    DO $$ plv8.elog(NOTICE, 'this', 'is', 'inline', 'code') $$ LANGUAGE plv8;

## Auto mapping between JS and database built-in types
For the result and arguments, database types and JS types are mapped
automatically. If the desired database type is one of:

- `oid`
- `bool`
- `int2`
- `int4`
- `int8`
- `float4`
- `float8`
- `numeric`
- `date`
- `timestamp`
- `timestamptz`
- `bytea`
- `json (>= 9.2)`
- `jsonb (>= 9.4)`

and the JS value looks compatible, then the conversion succeeds. Otherwise,
PL/v8 tries to convert them via cstring representation. An array type is
supported only if the dimention is one. A JS object will be mapped to
a tuple when applicable. In addition to these types, PL/v8 supports
polymorphic types such like `anyelement` and `anyarray`. Conversion of `bytea` is
a little different story. See the [`TypedArray` section](#typed-array).

## Database access via SPI including prepared statements and cursors
### `plv8.execute( sql [, args] )`
Executes SQL statements and retrieves the results. The `args` is an optional
argument that replaces `$n` placeholders in `sql`. For `SELECT` queries, the
returned value is an array of objects. Each hash represents each record.
Column names are mapped to object properties. For non-SELECT commands, the
returned value is an integer that represents number of affected rows.

    var json_result = plv8.execute( 'SELECT * FROM tbl' );
    var num_affected = plv8.execute( 'DELETE FROM tbl WHERE price > $1', [ 1000 ] );

Note this function and similar are not allowed outside of transaction.

### `plv8.prepare( sql, [, typenames] )`
Opens a prepared statement. The `typename` parameter is an array where
each element is a string to indicate database type name for bind parameters.
Returned value is an object of `PreparedPlan`. This object must be freed by
`plan.free()` before leaving the function.

    var plan = plv8.prepare( 'SELECT * FROM tbl WHERE col = $1', ['int'] );
    var rows = plan.execute( [1] );
    var sum = 0;
    for (var i = 0; i < rows.length; i++) {
      sum += rows[i].num;
    }
    plan.free();

    return sum;

### `PreparedPlan.execute( [args] )`
Executes the prepared statement. The `args` parameter is as `plv8.execute()`, and
can be omitted if the statement does not have parameters at all. The result
of this method is also as described in `plv8.execute()`.

### `PreparedPlan.cursor( [args] )`
Opens a cursor from the prepared statement. The `args` parameter is as
`plv8.execute()`, and can be omitted if the statement does not have parameters
at all. The returned object is of `Cursor`. This must be closed by `Cursor.close()`
before leaving the function.

    var plan = plv8.prepare( 'SELECT * FROM tbl WHERE col = $1', ['int'] );
    var cursor = plan.cursor( [1] );
    var sum = 0, row;
    while (row = cursor.fetch()) {
        sum += row.num;
    }
    cursor.close();
    plan.free();

    return sum;

### `PreparedPlan.free()`
Frees the prepared statement.

### `Cursor.fetch( [nrows] )`
When `nrows` parameter is omitted, fetches a row from the cursor and return it
as an object (note: not an array.) If specified, fetches as many rows as
the parameters up to exceeding, and returns an array of objects. A negative
value for this parameter will fetch backwards.

### `Cursor.move( [nrows] )`
Move the cursor `nrows` rows. A negative value will move backwards.

### `Cursor.close()`
Closes the cursor.

## Subtransaction
### `plv8.subtransaction( func )`
`plv8.execute()` creates a subtransaction every time. If you need an atomic
operation, you will need to call `plv8.subtransaction()` to create a subtransaction
block.

    try{
      plv8.subtransaction(function(){
        plv8.execute("INSERT INTO tbl VALUES(1)"); // should be rolled back!
        plv8.execute("INSERT INTO tbl VALUES(1/0)"); // occurs an exception
      });
    } catch(e) {
      ... do fall back plan ...
    }

If one of the SQL execution in the subtransaction block fails, all of operation
within the block is rolled back. If the process in the block throws a JS
exception, it is transported to the outside. So use a `try ... catch` block to
capture it and do alternative operations when it happens.

## Utility functions
PL/v8 provides the following utility built-in functions.

- `plv8.elog(elevel, msg1[, msg2, ...])`
- `plv8.quote_literal(str)`
- `plv8.nullable(str)`
- `plv8.quote_ident(str)`
- `plv8.version`

### plv8.elog
`plv8.elog` emits message to the client or the log file. The `elevel` is one of:

- `DEBUG5`
- `DEBUG4`
- `DEBUG3`
- `DEBUG2`
- `DEBUG1`
- `LOG`
- `INFO`
- `NOTICE`
- `WARNING`
- `ERROR`

    var msg = 'world';
    plv8.elog(DEBUG1, 'Hello',`${msg}!`);


See the [PostgreSQL manual for each error level](https://www.postgresql.org/docs/current/static/runtime-config-logging.html#RUNTIME-CONFIG-SEVERITY-LEVELS).


### plv8.quote_literal, plv8.nullable, and plv8.quote_ident
Each functionality for quote family is identical to the built-in SQL function
with the same name.

### plv8.find_function
PL/v8 provides a function to access other `plv8` functions that have been registered in the database.

    CREATE FUNCTION callee(a int) RETURNS int AS $$ return a * a $$ LANGUAGE plv8;
    CREATE FUNCTION caller(a int, t int) RETURNS int AS $$
      var func = plv8.find_function("callee");
      return func(a);
    $$ LANGUAGE plv8;

With `plv8.find_function()`, you can look up other `plv8` functions. If they are
not a `plv8` function, it errors out. The function signature parameter to
`plv8.find_function()` is either of `regproc` (function name only) or `regprocedure`
(function name with argument types). You can make use of the internal type for
arguments and void type for return type for the pure JavaScript function to
make sure any invocation from SQL statements should not happen.

### plv8.version
The `plv8` object provides version string as `plv8.version`. This string
corresponds to `plv8` module version. Note this is not the extension version.

## Window function API
You can define user-defined window functions with PL/v8. It wraps the C-level
window function API to support full functionality. To create one, first obtain a
window object by calling `plv8.get_window_object()`, which provides the following
interfaces:

### `WindowObject.get_current_position()`
Returns the current position in the partition, starting from 0.

### `WindowObject.get_partition_row_count()`
Returns the number of rows in the partition.

### `WindowObject.set_mark_position( pos )`
Set mark at the specified row. Rows above this position will be gone and
not be accessible later.

### `WindowObject.rows_are_peers( pos1, pos2 )`
Returns `true` if the rows at `pos1` and `pos2` are peers.

### `WindowObject.get_func_arg_in_partition( argno, relpos, seektype, mark_pos )`

### `WindowObject.get_func_arg_in_frame( argno, relpos, seektype, mark_pos )`
Returns the value of the argument in `argno` (starting from 0) to this
function at the `relpos` row from `seektype` in the current partition or
frame. `seektype` can be either of `WindowObject.SEEK_HEAD`,
`WindowObject.SEEK_CURRENT`, or `WindowObject.SEEK_TAIL`. If `mark_pos` is true,
the row the argument is fetched from is marked. If the specified row is
out of the partition/frame, the returned value will be `undefined`.

### `WindowObject.get_func_arg_in_current( argno )`
Returns the value of the argument in `argno` (starting from 0) to this
function at the current row. Note that the returned value will be the
same as the argument variable of the function.

### `WindowObject.get_partition_local( [size] )`
Returns partition-local value, which is released at the end of the current
partition. If nothing is stored, `undefined` is returned. `size` argument
(default 1000) is the byte size of the allocated memory in the first call.
Once the memory is allocated, the size will not change.

### `WindowObject.set_partition_local( obj )`
Stores the partition-local value, which you can retrieve later with
`get_partition_local()`. This function internally uses `JSON.stringify()` to
serialize the object, so if you pass a value that is not able to be serialized
it may end up being an unexpected value. If the size of a serialized value is
more than the allocated memory, it will throw an exception.

You can also learn more on how to use these API in the `sql/window.sql` regression
test, which implements most of the native window functions. For general
information on the user-defined window function, see the [`CREATE FUNCTION`
page of the PostgreSQL manual](https://www.postgresql.org/docs/current/static/sql-createfunction.html).

## Typed array
The typed array is something v8 provides to allow fast access to native memory,
mainly for the purpose of their canvas support in browsers. PL/v8 uses this
to map `bytea` and various array types to JavaScript `Array`. In the case of `bytea`,
you can access each byte as an array of unsigned bytes. For
`int2`/`int4`/`float4`/`float8` array types, PL/v8 provides direct access to each
element by using PL/v8 domain types.

- `plv8_int2array` maps `int2[]`
- `plv8_int4array` maps `int4[]`
- `plv8_float4array` maps `float4[]`
- `plv8_float8array` maps `float8[]`

These are only annotations that tell PL/v8 to use the fast access method instead of
the regular one. For these typed arrays, only 1-dimensional array without `NULL`
element. Also, there is currently no way to create such typed array inside
PL/v8 functions. Only arguments can be typed array. You can modify the element
and return the value. An example for these types are as follows.

    CREATE FUNCTION int4sum(ary plv8_int4array) RETURNS int8 AS $$
      var sum = 0;
      for (var i = 0; i < ary.length; i++) {
        sum += ary[i];
      }
      return sum;
    $$ LANGUAGE plv8 IMMUTABLE STRICT;

    SELECT int4sum(ARRAY[1, 2, 3, 4, 5]);
     int4sum
    ---------
          15
    (1 row)

## ES6 Language Features
PL/v8 enables all shipping feature of the used V8 version. So with V8 4.1+
many ES6 features, like block scoping, collections, generators and string
templates, are enabled by default.

Additional features can be enabled by setting the GUC `plv8.v8_flags`
(e.g. `SET plv8.v8_flags = '--es_staging';`).

These flags are honoured once per user session when the V8 runtime is
initialized. Compared to [Dialects (see below)](#dialects), which can be set on a
per function base, the V8 flags cannot be changed once the runtime is
initialized. So normally this setting should rather be set per database,
and not per session.

## Runtime environment separation across users in the same session
In PL/v8, each session has one global JS runtime context. This enables function
invocations at low cost, and sharing common object among the functions. However,
for the security reasons, if the user switches to another with `SET ROLE` command,
a new JS runtime context is initialized and used separately. This prevents the
risk of unexpected information leaking.

Each `plv8` function is invoked as if the function is the property of other object.
This means `this` in each function is a JS object that is created every time
the function is executed in a query. In other words, the life time and the
visibility of `this` object in a function is only a series of function calls in
a query. If you need to share some value among different functions, keep it in
the global `plv8` object because each function invocation has a different `this`
object.

## Start-up procedure
PL/v8 provides a start up facility, which allows you to call a `plv8` runtime
environment initialization function specified in the `GUC` variable.

    SET plv8.start_proc = 'plv8_init';
    SELECT plv8_test(10);

If this variable is set when the runtime is initialized, before the function
call of `plv8_test()` another `plv8` function `plv8_init()` is invoked. In such
initialization function, you can add any properties to `plv8` object to expose
common values or assign them to the `this` property. In the initialization
function, the receiver `this` is specially pointing to the global object, so
the variables that are assigned to the `this` property in this initialization are
visible from any subsequent function as global variables.

Remember `CREATE FUNCTION` also starts the `plv8` runtime environment, so make sure
to `SET` this `GUC` before any plv8 actions including `CREATE FUNCTION`.

## Update procedure
Updating PL/v8 is usually straightforward as it is a small and stable extension
- it only contains a handful of objects that need to be added to PostgreSQL when
installing the extension.

The procedure that is responsible for invoking this installation script
(generated during compile time based on `plv8.sql.common`), is controlled by
PostgreSQL and runs when `CREATE EXTENSION` is executed only. After building,
it takes the form of `plv8--<version>.sql` and is usually located under
`/usr/share/postgresql/<PG_MAJOR>/extension`, depending on the OS.

When this command is executed, PostgreSQL tracks which objects belong to the
extension and conversely removes them upon uninstallation, i.e., whenever
`DROP EXTENSION` is called.

You can explore some of the objects that PL/v8 stores under PostgreSQL:

    SELECT lanname FROM pg_catalog.pg_language WHERE lanname = 'plv8';
    SELECT proname FROM pg_proc p WHERE p.proname LIKE 'plv8%';
    SELECT typname FROM pg_catalog.pg_type WHERE typname LIKE 'plv8%';

__When__ and __if__ these objects change, extensions may provide upgrade scripts
which contemplate different upgrade paths (e.g. going from 1.5 to 2.0 or from
1.5.0 to 1.5.1). This allows using the special
`ALTER EXTENSION <extension> UPDATE [ TO <new_version> ]` syntax instead of
having to manually execute `DROP EXTENSION` followed by `CREATE EXTENSION`.

This is particularly useful when a large number of user-owned objects depend on
the extension, as it would mean dropping all of them and re-creating them after
the extension is created again.

Currently, PL/v8 does not ship with upgrade scripts as there haven't been
updates to these objects since the early builds. This may change in 2.0.0 with
the introduction of the `plv8_version` function, which was added as a function
object as part of the extension install script.

If there are no changes to these objects, there is no need to `DROP EXTENSION`
/ `CREATE EXTENSION` as PostgreSQL is able to automatically read the new the
control file (`plv8.control`) and load the binary into memory (`plv8.so`) as
soon as a new connection is established. Don't be fooled by
`SELECT pg_available_extensions()` returning the new version as that function
actually re-reads the extension directory and returns the version value of the
new control file, which may not represent the current PL/v8 version in memory.
Also note that running `DROP EXTENSION` / `CREATE EXTENSION` has no effect
whatsoever on loading the new PL/v8 version, although new scripts will be picked
up.

The best way of finding out which PL/v8 version you're running is by executing:

    DO $$ plv8.elog(WARNING, plv8.version) $$ LANGUAGE plv8;

Even when using PL/v8 2.0.0, `SELECT plv8_version();` is only indicative of the
upgrade scripts being ran, as mentioned earlier, not of the current PL/v8
extension version in memory.

In conclusion, for now it is safe to simply copy the new control and binary files
to the correct paths. This can be either `make install` or by installing a newer
package like `postgresql-9.5-plv8`. Then, make sure the new binary is loaded
immediately by all users by forcing a server restart (a reload won't suffice) or
simply prepare your code to deal with the fact that only newer connections will
get access to the PL/v8 version.

## Dialects
This module also contains some dialect supports. Currently, we have two dialects
that are supported:

- CoffeeScript (plcoffee)
- LiveScript (plls)

With PostgreSQL 9.1 or above, you are able to load those dialects via `CREATE EXTENSION` command.
