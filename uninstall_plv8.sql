SET search_path = public;

DROP LANGUAGE plv8;
DROP FUNCTION plv8_call_handler();
DROP FUNCTION plv8_inline_handler(internal);
DROP FUNCTION plv8_call_validator(oid);

DROP DOMAIN plv8_int2array;
DROP DOMAIN plv8_int4array;
DROP DOMAIN plv8_float4array;
DROP DOMAIN plv8_float8array;
