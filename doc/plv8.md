PL/v8
=====

PL/v8 is a trusted procedural language that is safe to use, fast to run and
easy to develop, powered by V8 JavaScript Engine.

Contents
--------

Implemented features are as follows.

- Scalar function calls
- Set returing function calls
- Trigger function calls
- Inline statement calls
- Auto mapping between JS and database built-in types
- Database access via SPI including prepared statements and cursors
- Subtransaction
- Utility functions
- Runtime environment separation across users in the same session
- Start-up procedure

Scalar function calls
---------------------

In PL/v8, you can write your SQL invoked function in JavaScript.  Use usual
CREATE FUNCTION statement with a JS function body.  Here is an example of a
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

The function will be internally defined such that

    (function(arg1, arg2, ..){
       $funcbody$
    })

where $funcbody$ is the script source you specify in CREATE FUNCTION AS clause.
The argument names are inherited from the CREATE FUNCTION statement, or they will
be named as $1, $2 if the names are omitted.

Set returning function calls
----------------------------

PL/v8 supports set returning function calls.

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

If the function is declared as RETURNS SETOF, PL/v8 prepares a tuplestore every
time called.  You can call plv8.return_next() function to add as many result as
you like to return rows from this function.  Alternatively, you can just return
a JS array to add set of records, a JS object to add a record, or a scalar value
to add a scalar to the tuplestore.  Unlike other PLs, PL/v8 does not support
per-value return strategy, but it always uses tuplestore strategy.

Trigger function calls
----------------------

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

If the trigger type is an INSERT or UPDATE, you can assign properties of NEW
variable to change the actual tuple stored by this operation.

A plv8 trigger function will have special arguments to pass the trigger state as
following

- NEW
- OLD
- TG_NAME
- TG_WHEN
- TG_LEVEL
- TG_OP
- TG_RELID
- TG_TABLE_NAME
- TG_TABLE_SCHEMA
- TG_ARGV

For each variable semantics, see also the trigger section in PostgreSQL manual.

Inline statement calls
----------------------

PL/v8 supports DO block with PostgreSQL 9.0 and above.

    DO $$ plv8.elog(NOTICE, 'this', 'is', 'inline', 'code') $$ LANGUAGE plv8;

Auto mapping between JS and database built-in types
---------------------------------------------------

Between the result type and argument types, database types and JS types are
mapped automatically.  If the desired database type is one of

- oid
- bool
- int2
- int4
- int8
- float4
- float8
- numeric
- date
- timestamp
- timestamptz

and the JS value looks compatible, then the conversion succeeds.  Otherwise,
PL/v8 tries to convert them via cstring representation.  An array type is
supported only if the dimention is one.  A JS object will be mapped to
a tuple when applicable.

Database access via SPI including prepared statements and cursors
-----------------------------------------------------------------

### plv8.execute( sql [, args] ) ###

Execute SQL statements and retrieve the result. `args` is an optional argument
that replaces $n placeholders in `sql`.  For SELECT queries, the returned value
is an array of objects. Each hash represents each record.  Column names are
mapped to object properties.  For non-SELECT commands, the returned value is
an integer that represents number of affected rows.

    var json_result = plv8.execute( 'SELECT * FROM tbl' );
    var num_affected = plv8.execute( 'DELETE FROM tbl WHERE price > $1', [ 1000 ] );

### plv8.prepare( sql, [, typenames] ) ###

Opens a prepared statement.  The `typename` parameter is an array where
each element is a string to indicate database type name for bind parameters.
Returned value is an object of PreparedPlan.  This object must be freed by
plan.free() before leaving the function.

    var plan = plv8.prepare( 'SELECT * FROM tbl WHERE col = $1', ['int'] );
    var rows = plan.execute( [1] );
    var sum = 0;
    for (var i = 0; i < rows.length; i++) {
      sum += rows[i].num;
    }
    plan.free();
    
    return sum;

### PreparedPlan.execute( [args] ) ###

Executes the prepared statement.  The args parameter is as plv8.execute(), and
can be omitted if the statement does not have parameters at all.  The result
of this method is also as described in plv8.execute().

### PreparedPlan.cursor( [args] ) ###

Opens a cursor from the prepared statement. The args parameter is as
plv8.execute(), and can be omitted if the statement does not have parameters
at all.  The returned object is of Cursor.  This must be closed by Cursor.close()
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

### PreparedPlan.free() ###

Frees the prepared statement.

### Cursor.fetch() ###

Fetches a row from the cursor and return as an object (note: not an array.)
Fetching more than one row, and move() method are currently not implemented.

### Cursor.close() ###

Closes the cursor.

Subtransaction
--------------

### plv8.subtransaction( func ) ###

plv8.execute() creates subtransaction every time.  If you need to atomic operation,
you will need to call plv8.subtransaction() to create a transaction block.

    try{
      plv8.subtransaction(function(){
        plv8.execute("INSERT INTO tbl VALUES(1)");  -- should be rolled back!
        plv8.execute("INSERT INTO tbl VALUES(1/0)");-- occurs an exception
      });
    } catch(e) {
      ... do fall back plan ...
    }

If one of the SQL execution in the subtransaction block, all of operation within
the block is rolled back.  If the process in the block throws JS exception,
it is transported to the outside.  So use try ... catch block to capture it and
do alternative operations when it happens.

Utility functions
-----------------

PL/v8 provides the following utility built-in functions.

- plv8.elog(elevel, msg1[, msg2, ...])
- plv8.quote_literal(str)
- plv8.nullable(str)
- plv8.quote_ident(str)

plv8.elog emits message to the client or the log file.  The elevel is one of

- DEBUG5
- DEBUG4
- DEBUG3
- DEBUG2
- DEBUG1
- LOG
- INFO
- NOTICE
- WARNING
- ERROR

See the PostgreSQL manual for each error level.

Each functionality for quote family is identical to the built-in SQLfunction
with the same name.

In addition, PL/v8 provides a function to access other plv8 functions that have
been registered in the database.

    CREATE FUNCTION callee(a int) RETURNS int AS $$ return a * a $$ LANGUAGE plv8;
    CREATE FUNCTION caller(a int, t int) RETURNS int AS $$
      var func = plv8.find_function("callee");
      return func(a);
    $$ LANGUAGE plv8;

With plv8.find_function(), you can look up other plv8 functions.  If they are
not the plv8 function, it errors out.  The function signature parameter to
plv8.find_function() is either of regproc (function name only) or regprocedure
(function name with argument types).

Runtime environment separation across users in the same session
---------------------------------------------------------------

In PL/v8, each session has one global JS runtime context.  This enables function
invokations at low cost, and sharing common object among the functions.  However,
for the security reasons, if the user switches to another with SET ROLE command,
the JS runtime context is initialized and used separately.  This prevents
unexpected information leak risk.

Each plv8 function is invoked as if the function is the property of other object.
This means "this" in each function is a JS object that is created every time
the function is executed in a query.  In other words, the life time and the
visibility of "this" object in a function is only a series of function calls in
a query.  If you need to share some value among different functions, keep it in
plv8 object because each function invocation has different "this" object.

Start-up procedure
------------------

PL/v8 provides start up facility, which allows to call initialization plv8
function specified in the GUC variable.

    SET plv8.start_proc = 'plv8_init';
    SELECT plv8_test(10);

If this variable is set when the runtime is initialized, before the function call
of plv8_test() another plv8 function plv8_init() is invoked.  In such initialization
function, you can add any property to plv8 object to expose common values or
assign them to "this" property.  In the initialization function, "this" is specially
pointing to the global object, not the function receiver, so the variables that is
assigned to "this" property in this initialization are visible from any subsequent
function as global variables.

Remember CREATE FUNCTION also starts plv8 runtime environment, so make sure to SET
this GUC before any plv8 actions including CREATE FUNCTION.

