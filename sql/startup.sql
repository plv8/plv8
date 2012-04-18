
set plv8.start_proc = startup;

do $$ print(NOTICE, 'foo = ' + foo) $$ language plv8;

update    plv8_modules set code = 'foo=98765;' where modname = 'startup';

-- startup code should not be reloaded
do $$ print(NOTICE, 'foo = ' + foo) $$ language plv8;

do $$ load_module('testme'); print (NOTICE,'bar = ' + bar);$$ language plv8;


   
