CREATE SCHEMA plv8;
CREATE FUNCTION get_key(key text, json_raw jsonb) RETURNS jsonb
LANGUAGE plv8 IMMUTABLE STRICT
AS $$
  var val = json_raw[key];
  var ret = {};
  ret[key] = val;
  return ret;
$$;

CREATE TABLE jsononly (
    data jsonb
);

COPY jsononly (data) FROM stdin;
{"ok": true}
\.

-- Call twice to test the function cache.
SELECT get_key('ok', data) FROM jsononly;
SELECT get_key('ok', data) FROM jsononly;
