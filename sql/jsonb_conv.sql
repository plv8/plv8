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
