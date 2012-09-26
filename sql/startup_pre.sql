SET client_min_messages = ERROR;
CREATE TABLE public.plv8_modules (
   modname name primary key,
   code    text not null
);


insert into plv8_modules values ('testme','bar=98765;');

create function startup()
   returns void
   language plv8 as
$$

foo=14378;

load_module = function(modname) 
{
    var rows = plv8.execute("SELECT code from plv8_modules where modname = $1", [modname]);
    for (var r = 0; r < rows.length; r++)
    {
        var code = rows[r].code;
        eval("(function() { " + code + "})")();
        plv8.elog (NOTICE, 'loaded module: ' + modname);
    }
};

$$;
