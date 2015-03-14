#PLV8

## OVERVIEW ##

PLV8 is a shared library that provides a PostgreSQL procedual language
powered by V8 JavaScript Engine. With this program you can write in
JavaScript your function that is callable from SQL.

Supported features are:
  * Functions can receive any arguments, and return a value or set of records.
  * Execute any SQL commands with built-in plv8.execute() function.
  * Automatic data conversion between PostgreSQL and JavaScript, including string, numbers, timestamps, arrays, and records.
    * Records are converted to JSON which keys are column names.
    * Other types are converted to strings.
  * Partial support to write TRIGGER handlers.
  * Inline commands with DO statement for PG 9.0 or newer versions.
  * EXTENSION support for PG 9.1 or newer versions.


## REQUIREMENT ##

plv8 is tested with:
  * PG: version 8.4, 9.0, 9.1 and 9.2, 9.3, 9.4dev (maybe older are allowed)
  * V8: version 3.6.2
  * g++: version 4.5.1

Also all tools that PG and V8 requires to be built are required.

## INSTALL ##

Get PostgreSQL and V8 source
  * PG: http://www.postgresql.org/ftp/source/
  * v8: http://code.google.com/p/v8/wiki/Source

Build both of them.

### PostgreSQL ###

```
cd ~/build
tar xvjf ../tarballs/postgresql-9.2.0.tar.bz2
cd postgresql-9.2.0
./configure --prefix=$HOME/local CFLAGS='-O2 -pthread'
make
make install
```

In some platform (we've seen this in FreeBSD), -pthread is required.  If plv8 hangs, try this option.

### V8 ###

You need to specify library=shared.
```
cd ~/build
svn checkout http://v8.googlecode.com/svn/trunk/ v8
cd v8
export GYPFLAGS="-D OS=freebsd"
make dependencies
make native.check -j 4 library=shared strictaliasing=off console=readline
```

After v8 library is built, copy the .so file to /usr/lib or /usr/lib64.

Add pg\_config to you $PATH.  Normally pg\_config exists in $PGHOME/bin.

And build and install PLV8 with the following commands.
plv8.so and scripts will be installed your $PGHOME/lib and $PGHOME/share/contrib (or $PGHOME/share/extension).

```
cd ~/build
git clone https://code.google.com/p/plv8js/
cd plv8js
    
make 
make install
```

You can pass v8 source directory via V8\_SRCDIR if include files are not found automatically.  V8\_OUTDIR is for library path, which is typically $(V8\_SRCDIR)/out/native or similar.  If you prefer to build plv8 with v8 statically linked, you may want to try V8\_STATIC\_SNAPSHOT=1 or V8\_STATIC\_NOSNAPSHOT=1.

After installation, you can register the plv8 stored procedure with
  * (PG 9.1 or newer) psql -d _dbname_ -c "CREATE EXTENSION plv8"
  * (PG 9.0 or older) psql -d _dbname_ -f $PGHOME/share/contrib/plv8.sql

And enable plv8 language in your database.
  * createlang -d _dbname_ plv8
  * psql -d _dbname_ -c "CREATE LANGUAGE plv8"

You can uninstall plv8 with
  * psql -d _dbname_ -f $PGHOME/share/contrib/uninstall\_plv8.sql

## TEST ##
PostgreSQL server should be started before the command.
PLV8 supports installcheck command to test its features.

  * make installcheck

## EXAMPLE ##
  * create js function and test
```
CREATE OR REPLACE FUNCTION plv8_test(keys text[], vals text[]) RETURNS
text AS $$
var o = {};
for(var i=0; i<keys.length; i++){
 o[keys[i]] = vals[i];
}
return JSON.stringify(o);
$$ LANGUAGE plv8 IMMUTABLE STRICT;

SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
```

  * SETOF record function with plv8.return\_next() function
```
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
```

  * inline statement
```
DO $$ plv8.elog(NOTICE, 'this', 'is', 'inline', 'code') $$ LANGUAGE plv8;
```

## BUILT-IN FUNCTIONS AND VARIABLES ##
  * plv8.elog( elevel, ... )
The function print messages to server and/or client logs just like as RAISE in PL/pgSQL or elog/ereport.
Acceptable elevels are DEBUG[1-5], LOG, INFO, NOTICE, WARNING and ERROR.

Example:
```
plv8.elog(NOTICE, 'notice', 'message');
```

  * plv8.execute( sql `[, args]` )

Execute SQL statements and retrieve the result.
"args" is an optional argument that replaces $n placeholders in "sql".

For SELECT queries, the returned value is an array of hashes.
Each hash represents each record.
Column names are mapped to hash keys.

For non-SELECT commands, the returned value is an integer that represents number of affected rows.

Example:
```
var json_result = plv8.execute( 'SELECT * FROM tbl' );
var num_affected = plv8.execute( 'DELETE FROM tbl WHERE price > $1', [ 1000 ] );
```

  * plv8.prepare( sql, `[, typenames]` )

Create a prepared statement.  The _typename_ parameter is an array where each element is a string to indicate PostgreSQL type name for bind parameters.
Returned value is an object of PreparedPlan.  This object must be freed by plan.free() before leaving the function.

Example:
```
var plan = plv8.prepare( 'SELECT * FROM tbl WHERE col = $1', ['int'] );
var rows = plan.execute( [1] );
var sum = 0;
for (var i = 0; i < rows.length; i++) {
  sum += rows[i].num;
}
plan.free();

return sum;
```

  * PreparedPlan.execute( `[args]` )

Execute the prepared statement.  The _args_ parameter is as plv8.execute(), and can be omitted if the statement doesn't have parameters at all.  The result of this method is also as described in plv8.execute().

  * PreparedPlan.cursor( `[args]` )

Open a cursor from the prepared statement.  The _args_ parameter is as plv8.execute(), and can be omitted if the statement doesn't have parameters at all.  The returned object is of Cursor.  This must be closed by Cursor.close() before leaving the function.

Example:
```
var plan = plv8.prepare( 'SELECT * FROM tbl WHERE col = $1', ['int'] );
var cursor = plan.cursor( [1] );
var sum = 0, row;
while (row = cursor.fetch()) {
  sum += row.num;
}
cursor.close();
plan.free();

return sum;
```

  * PreparedPlan.free()

Free the prepared statement.

  * Cursor.fetch()

Fetch a row from the cursor and return as an object (note: not an array.)  Fetching more than one row, and move() method will be considered to implement.

  * Cursor.close()

Close the cursor.

  * plv8.subtransaction( func )

This function runs the argument function within a sub-transaction.  This is needed when you want multiple executeSql run atomic. If one of the statements fails then everything which is run in this function will be rolled back.  Note that if an exception is thrown from the subtransaction function, the exception goes out of subtransaction(), so you'll typically need another try-catch block outside.

Example:
```
try{
  plv8.subtransaction(function(){
    plv8.execute("INSERT INTO tbl VALUES(1)");  -- should be rolled back!
    plv8.execute("INSERT INTO tbl VALUES(1/0)");-- occurs an exception
  });
} catch(e) {
  ... do fall back plan ...
}
```

  * plv8.find\_function( signature )

This function returns a JavaScript function which has been created as SQL function via CREATE FUNCTION.  This is limited to plv8 functions and attempts to find functions in other languages will fail.  The _signature_ parameter is a string which represents a SQL function in either of regproc (_jsfunc_) or regprocedure style (_jsfunc(int, int)_).

```
CREATE FUNCTION callee(a int) RETURNS int AS $$
  returns a * a;
$$ LANGUAGE plv8;

CREATE FUNCTION caller(a int) RETURNS int AS $$
  var func = plv8.find_function("callee");
  return func(a);
$$ LANGUAGE plv8;

SELECT caller(2);
 caller 
--------
      4
(1 row)
```
  * Old built-in functions

Before introducing plv8 prefix object, there were built-in functions almost equivalent to the list above.  These functions are NOT working anymore.  The main reason of the new plv8 prefix object is that JavaScript is too fragile to pollute global name space and it is the industry trend that adding such name space object is a good practice.  See also CommonJS.  Also, if you really want to call the old style functions, here is an example to get compatible.

```
CREATE OR REPLACE FUNCTION plv8_compat() RETURNS VOID AS
$$
	this.print = function(){
		plv8.elog.apply(plv8, Array.apply(null, arguments));
	}
	this.executeSql = function(){
		return plv8.execute(plv8, Array.apply(null, arguments));
	}
	this.subtransaction = function(func){
		return plv8.subtransaction(func);
	}
$$ LANGUAGE plv8;
SET plv8.start_proc = plv8_compat;
```

  * Trigger conditions in trigger functions

PLV8 functions can be invoked by triggers.
Then, trigger conditions can be retrieved as pre-defined variables:
NEW, OLD, TG\_NAME, TG\_WHEN, TG\_LEVEL, TG\_OP, TG\_RELID, TG\_TABLE\_NAME, TG\_TABLE\_SCHEMA, and TG\_ARGV.

## Separate Context ##

PLV8 is security aware;  the global JavaScript execution context is associated with user id and in case the user is switched by SET ROLE in the same session, the context is newly created (if there has not been) and entered.

## CAVEATS and TODO ITEMS ##
  * This is WIP version.
  * Tested on only Fedora 13 (x86\_64).
  * 1-dim array can be put as arguments. Multi dimension array is not supported.

Any question and comment appreciated.