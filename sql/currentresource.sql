CREATE OR REPLACE FUNCTION test_resource_owner()
RETURNS jsonb
AS $$
  return plv8.execute("SELECT 1")[0]
$$ LANGUAGE plv8;
CREATE TABLE resource_table (col TEXT);

SELECT test_resource_owner();
SELECT test_resource_owner() FROM resource_table;
INSERT INTO resource_table (col) VALUES ('123abc');

SELECT test_resource_owner() FROM resource_table;
