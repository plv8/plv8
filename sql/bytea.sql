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

DO $$
  const test = 'string test'
  const bytea = plv8.execute(`select $1::bytea`, [test])[0].bytea;
  const result = String.fromCharCode.apply(null, bytea);
  if (result === test) {
    plv8.elog(INFO, 'OK');
  } else {
    plv8.elog(WARN, 'FAIL');
  }
$$ language plv8;

-- typed arrays created in JavaScript must not crash the conversion to bytea
CREATE FUNCTION bytea_from_typed_array(kind text) RETURNS bytea AS $$
  switch (kind) {
    case 'f64': return new Float64Array([1]);
    case 'f32': return new Float32Array([1, 2]);
    case 'u16': return new Uint16Array([257, 514]);
    case 'i32': return new Int32Array([16843009]);
    case 'sub': return new Uint8Array([1, 2, 3, 4, 5]).subarray(1, 4);
    case 'dv':  return new DataView(new ArrayBuffer(3));
  }
$$ LANGUAGE plv8;
SELECT length(bytea_from_typed_array('f64'));
SELECT length(bytea_from_typed_array('f32'));
-- bytes are copied in native order, so only use values that read the same either way
SELECT bytea_from_typed_array('u16');
SELECT length(bytea_from_typed_array('i32'));
SELECT bytea_from_typed_array('sub');
SELECT length(bytea_from_typed_array('dv'));

-- ... nor to an array type
CREATE FUNCTION int_array_from_typed_array() RETURNS int[] AS $$
  return new Float64Array(1);
$$ LANGUAGE plv8;
SELECT int_array_from_typed_array();
