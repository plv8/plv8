CREATE OR REPLACE FUNCTION test_resource_owner()
RETURNS INT
AS $$
  return plv8.execute("SELECT 1")[0]['?column?']
$$ LANGUAGE plv8;
CREATE TABLE resource_table (col TEXT);

SELECT test_resource_owner();
SELECT test_resource_owner() FROM resource_table;
