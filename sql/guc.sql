-- enable strict mode with GUC, which allows usage of ES6 let and const keywords
SET plv8.v8_flags = '--use_strict';
CREATE OR REPLACE FUNCTION let_test()
   RETURNS json AS $$
      let result = ['Hello, World!'];
      return result;
   $$ LANGUAGE plv8 STABLE STRICT;
SELECT let_test();
DROP FUNCTION let_test();

-- set and show variables
SET plv8.start_proc = 'foo';
SHOW plv8.start_proc;

SET plv8.icu_data = 'bar';
SHOW plv8.icu_data;

SET plv8.v8_flags = 'baz';
SHOW plv8.v8_flags;
