-- INSTALL
SET client_min_messages = warning;
\set ECHO none
\i plv8.sql
\set ECHO all
RESET client_min_messages;

CREATE LANGUAGE plv8;

-- CREATE FUNCTION
CREATE FUNCTION plv8_test(keys text[], vals text[]) RETURNS text AS
$$
	var o = {};
	for (var i = 0; i < keys.length; i++)
		o[keys[i]] = vals[i];
	return JSON.stringify(o);
$$
LANGUAGE plv8 IMMUTABLE STRICT;
SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);

CREATE FUNCTION unnamed_args(text[], text[]) RETURNS text[] AS
$$
	var array1 = arguments[0];
	var array2 = $2;
	return array1.concat(array2);
$$
LANGUAGE plv8 IMMUTABLE STRICT;
SELECT unnamed_args(ARRAY['A', 'B'], ARRAY['C', 'D']);

CREATE FUNCTION concat_strings(VARIADIC args text[]) RETURNS text AS
$$
    var result = "";
    for (var i = 0; i < args.length; i++)
    	if (args[i] != null)
	    	result += args[i];
    return result;
$$
LANGUAGE plv8 IMMUTABLE STRICT;
SELECT concat_strings('A', 'B', NULL, 'C');

CREATE FUNCTION return_void() RETURNS void AS $$ $$ LANGUAGE plv8;
SELECT return_void();

CREATE FUNCTION return_null() RETURNS text AS $$ return null; $$ LANGUAGE plv8;
SELECT r, r IS NULL AS isnull FROM return_null() AS r;

-- TYPE CONVERTIONS
CREATE FUNCTION int2_to_int4(x int2) RETURNS int4 AS $$ return x; $$ LANGUAGE plv8;
SELECT int2_to_int4(24::int2);
CREATE FUNCTION int4_to_int2(x int4) RETURNS int2 AS $$ return x; $$ LANGUAGE plv8;
SELECT int4_to_int2(42);
CREATE FUNCTION int4_to_int8(x int4) RETURNS int8 AS $$ return x; $$ LANGUAGE plv8;
SELECT int4_to_int8(48);
CREATE FUNCTION int8_to_int4(x int8) RETURNS int4 AS $$ return x; $$ LANGUAGE plv8;
SELECT int8_to_int4(84);
CREATE FUNCTION float8_to_numeric(x float8) RETURNS numeric AS $$ return x; $$ LANGUAGE plv8;
SELECT float8_to_numeric(1.5);
CREATE FUNCTION numeric_to_int8(x numeric) RETURNS int8 AS $$ return x; $$ LANGUAGE plv8;
SELECT numeric_to_int8(1234.56);
CREATE FUNCTION int4_to_text(x int4) RETURNS text AS $$ return x; $$ LANGUAGE plv8;
SELECT int4_to_text(123);
CREATE FUNCTION text_to_int4(x text) RETURNS int4 AS $$ return x; $$ LANGUAGE plv8;
SELECT text_to_int4('123');
SELECT text_to_int4('abc'); -- error
CREATE FUNCTION int4array_to_textarray(x int4[]) RETURNS text[] AS $$ return x; $$ LANGUAGE plv8;
SELECT int4array_to_textarray(ARRAY[123, 456]::int4[]);
CREATE FUNCTION textarray_to_int4array(x text[]) RETURNS int4[] AS $$ return x; $$ LANGUAGE plv8;
SELECT textarray_to_int4array(ARRAY['123', '456']::text[]);

CREATE FUNCTION timestamptz_to_text(t timestamptz) RETURNS text AS $$ return t.toUTCString() $$ LANGUAGE plv8;
SELECT timestamptz_to_text('23 Dec 2010 12:34:56 GMT');
CREATE FUNCTION text_to_timestamptz(t text) RETURNS timestamptz AS $$ return new Date(t) $$ LANGUAGE plv8;
SELECT text_to_timestamptz('23 Dec 2010 12:34:56 GMT') AT TIME ZONE 'GMT';
CREATE FUNCTION date_to_text(t date) RETURNS text AS $$ return t.toUTCString() $$ LANGUAGE plv8;
SELECT date_to_text('23 Dec 2010');
CREATE FUNCTION text_to_date(t text) RETURNS date AS $$ return new Date(t) $$ LANGUAGE plv8;
SELECT text_to_date('23 Dec 2010 GMT');

CREATE FUNCTION oidfn(id oid) RETURNS oid AS $$ return id $$ LANGUAGE plv8;
SELECT oidfn('pg_catalog.pg_class'::regclass);

-- RECORD TYPES
CREATE TYPE rec AS (i integer, t text);
CREATE FUNCTION scalar_to_record(i integer, t text) RETURNS rec AS
$$
	return { "i": i, "t": t };
$$
LANGUAGE plv8;
SELECT scalar_to_record(1, 'a');

CREATE FUNCTION record_to_text(x rec) RETURNS text AS
$$
	return JSON.stringify(x);
$$
LANGUAGE plv8;
SELECT record_to_text('(1,a)'::rec);

CREATE FUNCTION set_of_records() RETURNS SETOF rec AS
$$
	yield( { "i": 1, "t": "a" } );
	yield( { "i": 2, "t": "b" } );
	yield( { "i": 3, "t": "c" } );
$$
LANGUAGE plv8;
SELECT * FROM set_of_records();

CREATE FUNCTION set_of_integers() RETURNS SETOF integer AS
$$
	yield( 1 );
	yield( 2 );
	yield( 3 );
$$
LANGUAGE plv8;
SELECT * FROM set_of_integers();

-- print()
CREATE FUNCTION test_print(arg text) RETURNS void AS
$$
	print(NOTICE, 'args =', arg);
	print(WARNING, 'warning');
	print(20, 'ERROR is not allowed');
$$
LANGUAGE plv8;
SELECT test_print('ABC');

-- executeSql()
CREATE TABLE test_tbl (i integer, s text);
CREATE FUNCTION test_sql() RETURNS integer AS
$$
	var rows = executeSql("SELECT i, 's' || i AS s FROM generate_series(1, 4) AS t(i)");
	for (var r = 0; r < rows.length; r++)
	{
		var result = executeSql("INSERT INTO test_tbl VALUES(" + rows[r].i + ",'" + rows[r].s + "')");
		print(NOTICE, JSON.stringify(rows[r]), result);
	}
	return rows.length;
$$
LANGUAGE plv8;
SELECT test_sql();
SELECT * FROM test_tbl;

CREATE FUNCTION return_sql() RETURNS SETOF test_tbl AS
$$
	return executeSql(
		"SELECT i, $1 || i AS s FROM generate_series(1, $2) AS t(i)",
		[ 's', 4 ]
	);
$$
LANGUAGE plv8;
SELECT * FROM return_sql();

CREATE FUNCTION test_sql_error() RETURNS void AS $$ executeSql("ERROR") $$ LANGUAGE plv8;
SELECT test_sql_error();

-- TRIGGER
CREATE FUNCTION test_trigger() RETURNS trigger AS
$$
	print(NOTICE, "NEW = ", JSON.stringify(NEW));
	print(NOTICE, "OLD = ", JSON.stringify(OLD));
	print(NOTICE, "TG_OP = ", TG_OP);
	print(NOTICE, "TG_ARGV = ", TG_ARGV);
$$
LANGUAGE "plv8";

CREATE TRIGGER test_trigger
  BEFORE INSERT OR UPDATE OR DELETE
  ON test_tbl FOR EACH ROW
  EXECUTE PROCEDURE test_trigger('foo', 'bar');

INSERT INTO test_tbl VALUES(100, 'ABC');
UPDATE test_tbl SET i = 101, s = 'DEF' WHERE i = 1;
DELETE FROM test_tbl WHERE i >= 100;
SELECT * FROM test_tbl;

-- ERRORS
CREATE FUNCTION syntax_error() RETURNS text AS '@' LANGUAGE plv8;

CREATE FUNCTION reference_error() RETURNS text AS 'not_defined' LANGUAGE plv8;
SELECT reference_error();

CREATE FUNCTION throw() RETURNS void AS $$throw new Error("an error");$$ LANGUAGE plv8;
SELECT throw();
