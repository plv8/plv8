CREATE FUNCTION crash_test() RETURNS boolean
LANGUAGE plv8 IMMUTABLE STRICT
AS $$
  var a = new ArrayBuffer(8);
  return true;
$$;

SELECT crash_test();
