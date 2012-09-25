CREATE EXTENSION plcoffee;
DO LANGUAGE plcoffee $$ plv8.elog(INFO, "foo") $$;
CREATE EXTENSION plls;
DO LANGUAGE plls $$ plv8.elog(INFO, "foo") $$;

CREATE FUNCTION v8func(a int) RETURNS int[] AS $$
return [a, a, a];
$$ LANGUAGE plv8;

CREATE FUNCTION coffeefunc(a int) RETURNS int[] AS $$
return plv8.find_function('v8func')(a);
$$ LANGUAGE plcoffee;

SELECT coffeefunc(10);

CREATE FUNCTION lsfunc(a int) RETURNS int[] AS $$
return plv8.find_function('v8func')(a);
$$ LANGUAGE plls;

SELECT lsfunc(11);
