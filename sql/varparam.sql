-- parameter type deduction in 9.0+
do language plv8 $$
  plv8.execute("SELECT count(*) FROM pg_class WHERE oid = $1", ["1259"]);
  var plan = plv8.prepare("SELECT * FROM pg_class WHERE oid = $1");
  var res = plan.execute(["1259"]).shift().relname;
  plv8.elog(INFO, res);
  var cur = plan.cursor(["2610"]);
  var res = cur.fetch().relname;
  plv8.elog(INFO, res);
  cur.close();
  plan.free();
$$;
