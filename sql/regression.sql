-- https://github.com/plv8/plv8/issues/527
DO $$
  plv8.prepare('SELECT * FROM (VALUES (1)) AS foo').cursor().fetch.apply(null)
$$ language plv8;
