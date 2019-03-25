-- test find_function permissions failure
CREATE FUNCTION perm() RETURNS void AS $$ plv8.elog(NOTICE, 'nope'); $$ LANGUAGE plv8;

CREATE ROLE someone_else;

REVOKE EXECUTE ON FUNCTION perm() FROM public;

SET ROLE TO someone_else;
DO $$ const func = plv8.find_function('perm') $$ LANGUAGE plv8;
DO $$ const func = plv8.find_function('perm()') $$ LANGUAGE plv8;

RESET ROLE;
DROP ROLE someone_else;

DROP FUNCTION perm();
