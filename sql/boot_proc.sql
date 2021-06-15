-- test startup failure
set plv8.boot_proc = foo;
do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

\c
set plv8.boot_proc = startup;

do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

update plv8_modules set code = 'foo=98765;' where modname = 'startup';

-- startup code should not be reloaded
do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

do $$ load_module('testme'); plv8.elog (NOTICE,'bar = ' + bar);$$ language plv8;

reset plv8.boot_proc;

CREATE ROLE someone_else;
SET ROLE to someone_else;

-- should fail because of a reference error
do $$ plv8.elog(NOTICE, 'foo = ' + foo) $$ language plv8;

-- should fail on permission
set plv8.boot_proc = startup;

set plv8.start_proc = startup;

RESET ROLE;

DROP ROLE someone_else;
