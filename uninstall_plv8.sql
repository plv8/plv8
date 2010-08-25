SET search_path = public;

DELETE FROM pg_pltemplate WHERE tmplname = 'plv8';

DROP LANGUAGE plv8;
DROP FUNCTION plv8_call_handler();
DROP FUNCTION plv8_inline_handler(internal);
DROP FUNCTION plv8_call_validator(oid);
