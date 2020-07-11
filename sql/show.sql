DO $$
    const res = plv8.execute('SHOW search_path');
    plv8.elog(INFO, JSON.stringify(res));
$$ language plv8;

DO $$
    const res = plv8.execute('EXPLAIN (FORMAT JSON, COSTS OFF) SELECT 1');
    plv8.elog(INFO, JSON.stringify(res));
$$ language plv8;
