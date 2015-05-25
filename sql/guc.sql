-- enable strict mode with GUC, which allows usage of ES6 let and const keywords
SET plv8.v8_flags = '--use_strict';
CREATE OR REPLACE FUNCTION let_test()
   RETURNS json AS $$
      let result = ['Hello, World!'];
      return result;
   $$ LANGUAGE plv8 STABLE STRICT;
SELECT let_test();
DROP FUNCTION let_test();
