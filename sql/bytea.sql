CREATE FUNCTION valid_arraybuffer_bytea(len integer) RETURNS bytea
LANGUAGE plv8 IMMUTABLE STRICT
AS $$
  return new ArrayBuffer(len);
$$;

SELECT length(valid_arraybuffer_bytea(20));

CREATE FUNCTION valid_int8array_bytea(len integer) RETURNS bytea
LANGUAGE plv8 IMMUTABLE STRICT
AS $$
  return new Int8Array(len);
$$;

SELECT length(valid_int8array_bytea(20));

CREATE FUNCTION valid_int16array_bytea(len integer) RETURNS bytea
LANGUAGE plv8 IMMUTABLE STRICT
AS $$
  return new Int16Array(len);
$$;

SELECT length(valid_int16array_bytea(20));
