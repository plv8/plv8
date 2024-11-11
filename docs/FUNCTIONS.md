# Function Calls

PLV8 has the ability to execute multiple types of function calls inside of
PostgreSQL.

## Scalar Function Calls

In PLV8, you can write your invoked function call in Javascript, using the usual
`CREATE FUNCTION` statement. Here is an example of a `scalar` function call:

```
CREATE FUNCTION plv8_test(keys TEXT[], vals TEXT[]) RETURNS JSON AS $$
    var o = {};
    for(var i=0; i<keys.length; i++){
        o[keys[i]] = vals[i];
    }
    return o;
$$ LANGUAGE plv8 IMMUTABLE STRICT;

=# SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);

plv8_test
---------------------------
{"name":"Tom","age":"29"}
(1 row)
```

Internally, the function will defined such as:

```
(function(keys, vals) {
  var o = {};
  for(var i=0; i<keys.length; i++){
      o[keys[i]] = vals[i];
  }
  return o;
})
```

Where `keys` and `vals` are type checked and validated inside of PostgreSQL,
called as arguments to the function, and `o` is the object that is returned as
the `JSON` type back to PostgreSQL. If argument names are omitted in the creation
of the function, they will be available in the function as `$1`, `$2`, etc.

## Set-returning Function Calls

PLV8 supports returning `SET` from function calls:

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
```

Running this gives you a `SETOF` result:

```
=# SELECT * FROM set_of_records();

i | t
---+---
1 | a
2 | b
3 | c
4 | d
(4 rows)
```

Internally, if the function is declared as `RETURNS SETOF`, PLV8 prepares a
`tuplestore` every time every time it is called. You can call the
`plv8.return_next()` function as many times as you need to return a row. In
addition, you can also return an `array` to add a set of records.

If the argument object to `return_next()` has extra properties that are not
defined by the argument, `return_next()` raises an error.

## Trigger Function Calls

PLV8 supports trigger function calls:

```
CREATE FUNCTION test_trigger() RETURNS TRIGGER AS
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
```

If the trigger type is an `INSERT` or `UPDATE`, you can assign properties of
`NEW` variable to change the actual tuple stored by this operation. A PLV8
trigger function will have the following special arguments that contain the
trigger state:

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

For more information see the [trigger section in PostgreSQL manual](https://www.postgresql.org/docs/current/static/plpgsql-trigger.html).

## Inline Statement Calls

PLV8 supports the `DO` block when using PostgreSQL 9.0 and above:

```
DO $$ plv8.elog(NOTICE, 'this', 'is', 'inline', 'code'); $$ LANGUAGE plv8;
```

## Auto Mapping Between Javascript and PostgreSQL Built-in Types

For the result and arguments, PostgreSQL types and Javascript types are mapped
automatically, if the desired PostgreSQL type is one of:

- `OID`
- `bool`
- `INT3 `
- `INT4`
- `INT8`
- `FLOAT4`
- `FLOAT8`
- `NUMERIC`
- `DATE`
- `TIMESTAMP`
- `TIMESTAMPTZ`
- `BYTEA`
- `JSON` (>= 9.2)
- `JSONB` (>= 9.4)

and the Javascript value looks compatible, then the conversion succeeds.
Otherwise, PLV8 tries to convert them via the `cstring` representation. An
`array` type is supported only if the dimension is one. A Javascript `object`
will be mapped to a `tuple` when applicable. In addition to these types, PLV8
supports polymorphic types such like `ANYELEMENT` and `ANYARRAY`. Conversion of
`BYTEA` is a little different story. See the [TypedArray section](#Typed%20Array).

## Typed Array

The `typed array` is something `v8` provides to allow fast access to native
memory, mainly for the purpose of their canvas support in browsers. PLV8 uses
this to map `BYTEA` and various array types to a Javascript `array`. In the case
of `BYTEA`, you can access each byte as an array of unsigned bytes. For
`int2`/`int4`/`float4`/`float8` array types, PLV8 provides direct access to each
element by using PLV8 domain types.

- `plv8_int2array` maps `int2[]`
- `plv8_int4array` maps `int4[]`
- `plv8_float4array` maps `float4[]`
- `plv8_float8array` maps `float8[]`

These are only annotations that tell PLV8 to use the fast access method instead
of the regular one. For these typed arrays, only 1-dimensional arrays without
any `NULL` elements. There is currently no way to create such typed array inside
PLV8 functions, only arguments can be typed array. You can modify the element and
return the value. An example for these types are as follows:

```
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
```

## Records

A `RECORD` is fundamental in Postgres, and can be used as both input and
output for functions.

```
CREATE TYPE record_type AS (i integer, t text);
CREATE FUNCTION mutate_record(rec record_type) RETURNS record_type AS
$$
        return { i: i * 2, t: `t => ${t}` };
$$
LANGUAGE plv8;

SELECT * FROM mutate_record('(17, skidoo)'::record_type);
 i  |      t
----+--------------
 34 | t =>  skidoo
(1 row)
```

In addition, a function can simply return a `RECORD`.

```
CREATE FUNCTION return_record(i integer, t text) RETURNS record AS
$$
	return { "i": i, "t": t };
$$
LANGUAGE plv8;
```

But it is required that the expected response is returned as part of the
query.

```
SELECT * FROM return_record(17, 'skidoo') AS ( i INT, t TEXT );
 i  |   t
----+--------
 17 | skidoo
(1 row)
```

## IN/OUT/INOUT Handling

There are some specific function declarations that PLV8 handles differently
than some other procedural languages.

```
CREATE FUNCTION inout_test(IN t1 TEXT, INOUT i1 INTEGER, OUT o1 TEXT) AS
$$
  return { i1: 23, o1: t1 + i1, foo: 'bar' };
$$
LANGUAGE plv8;
```

When we execute this we call it with only parameters that are inputs to the
function, in this case `t1` and `i1`. Note that only named parameters in
the function definition get returned.

```
SELECT * FROM inout_test('hello', 5);
 i1 |   o1
----+--------
 23 | hello5
(1 row)
```

When only one variable occurs in the function definition that as an output,
then the return type must be a scalar value.

```
CREATE FUNCTION scalar_test(INOUT i1 INTEGER) AS
$$
  return i1 + 5;
$$
LANGUAGE plv8;

SELECT * FROM scalar_test(23);
i1
----
28
(1 row)
```

Procedures work similarly to functions.

```
CREATE PROCEDURE procedure_inout_test(IN t1 TEXT, INOUT i1 INTEGER, OUT o1 TEXT) AS
$$
  return { i1: 23, o1: t1 + i1, foo: 'bar' };
$$
LANGUAGE plv8;
```

The main difference is that `OUT` arguments _must_ be explicitly used as
part of the `CALL`.

```
CALL procedure_inout_test('hello', 5, 'foo');
 i1 |   o1
----+--------
 23 | hello5
(1 row)
```

Again, extraneous output is ignored.
