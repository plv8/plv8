CREATE TYPE acomp AS (x int, y text, z timestamptz);
DO LANGUAGE PLV8 $$
  var jres = plv8.execute("select $1::acomp[]", [ [ { "x": 2, "z": null, "y": null } ] ]);
  plv8.elog(NOTICE,JSON.stringify(jres));
$$;
