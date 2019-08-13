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
