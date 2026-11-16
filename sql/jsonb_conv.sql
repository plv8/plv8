CREATE FUNCTION convb(o jsonb) RETURNS jsonb AS $$
if (o instanceof Array) {
	o[1] = 10;
} else if (typeof(o) == 'object') {
	o.i = 10;
}
return o;
$$ LANGUAGE plv8;

SELECT convb('{"i": 3, "b": 20}');
SELECT convb('[1, 2, 3]');

CREATE FUNCTION get_keyb(key text, json_raw jsonb) RETURNS jsonb
LANGUAGE plv8 IMMUTABLE STRICT
AS $$
  var val = json_raw[key];
  var ret = {};
  ret[key] = val;
  return ret;
$$;

CREATE TABLE jsonbonly (
    data jsonb
);

COPY jsonbonly (data) FROM stdin;
{"ok": true}
\.

-- Call twice to test the function cache.
SELECT get_keyb('ok', data) FROM jsonbonly;
SELECT get_keyb('ok', data) FROM jsonbonly;

CREATE FUNCTION jsonb_cat(data jsonb) RETURNS jsonb LANGUAGE plv8
AS $$
  return data;
$$;

SELECT jsonb_cat('[{"a": 1},{"b": 2},{"c": 3}]'::jsonb);

CREATE TABLE test_infinity_tbl (
  id INT,
  version_actual_period_start timestamp,
  version_actual_period_end timestamp
);

INSERT INTO test_infinity_tbl VALUES(1, '2019-03-01', 'infinity'::timestamp);

CREATE OR REPLACE FUNCTION test_plv8_with_infinite_date() RETURNS JSONB AS
$$
  var data = [];
  data = plv8.execute("SELECT * FROM test_infinity_tbl");
  return data[0];
$$
LANGUAGE plv8 STABLE;

SELECT test_plv8_with_infinite_date();

CREATE FUNCTION jsonb_undefined(data jsonb) RETURNS jsonb AS
$$
  return Object.assign({}, data, { key: undefined });
$$
LANGUAGE plv8;

SELECT jsonb_undefined('{"foo": "bar"}'::jsonb);

-- large jsonb string values must not overflow the stack
CREATE FUNCTION jsonb_big_string(j jsonb) RETURNS int AS $$
  return j.k.length;
$$ LANGUAGE plv8;
SELECT jsonb_big_string(jsonb_build_object('k', repeat('a', 1000000)));
SELECT jsonb_big_string(jsonb_build_object('k', repeat('a', 64000000)));

-- the largest string jsonb can hold (268435455 bytes), generated at test time
SELECT jsonb_big_string(jsonb_build_object('k', repeat('a', 268000000)));

-- dates must be formatted like Date.prototype.toISOString(), including
-- dates before 1970 and the extremes of the Date range
CREATE FUNCTION jsonb_date(ms float8) RETURNS jsonb AS $$
  return {d: new Date(ms)};
$$ LANGUAGE plv8;
CREATE FUNCTION jsonb_date_matches(ms float8) RETURNS boolean AS $$
  return JSON.stringify({d: new Date(ms)}) === JSON.stringify(plv8.execute('select jsonb_date($1) as j', [ms])[0].j);
$$ LANGUAGE plv8;
SELECT jsonb_date(0);
SELECT jsonb_date(1500);
SELECT jsonb_date(-1500);
SELECT jsonb_date(-1);
SELECT jsonb_date(-86400000);
SELECT jsonb_date(951782400000); -- leap day
SELECT jsonb_date(253402300799999); -- 9999-12-31
SELECT jsonb_date(253402300800000); -- first extended year
SELECT jsonb_date(-62167219200000); -- year 0
SELECT jsonb_date(-62167219200001); -- year -1
SELECT jsonb_date(-8.64e15);
SELECT jsonb_date(8.64e15);
SELECT bool_and(jsonb_date_matches(ms)) FROM unnest(ARRAY[0, 1500, -1500, -1, 951782400000, 253402300799999, 253402300800000, -62167219200000, -62167219200001, -8.64e15, 8.64e15, 1e12, -1e12, 4102444800123, -2208988800001]::float8[]) ms;
