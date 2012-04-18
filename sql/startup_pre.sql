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
    var rows = executeSql("SELECT code from plv8_modules where modname = $1", [modname]);
    for (var r = 0; r < rows.length; r++)
    {
        var code = rows[r].code;
        eval("(function() { " + code + "})")();
        print (NOTICE, 'loaded module: ' + modname);
    }
};

$$;
