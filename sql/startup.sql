-- test startup failure
set plv8.start_proc = foo;
do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

\c
set plv8.start_proc = startup;

do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

update    plv8_modules set code = 'foo=98765;' where modname = 'startup';

-- startup code should not be reloaded
do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

do $$ load_module('testme'); plv8.elog (NOTICE,'bar = ' + bar);$$ language plv8;

CREATE ROLE someone_else;
SET ROLE to someone_else;

reset plv8.start_proc;
-- should fail because of a reference error
do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

RESET ROLE;
DROP ROLE someone_else;
