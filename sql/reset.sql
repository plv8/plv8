CREATE OR REPLACE FUNCTION test_context_value() RETURNS text as $V8$
    return ctx_value;    
$V8$ LANGUAGE plv8;

CREATE OR REPLACE FUNCTION set_context_value(val TEXT) RETURNS void as $V8$
    ctx_value = val;    
$V8$ LANGUAGE plv8;

SELECT set_context_value('test');
SELECT test_context_value();

SELECT plv8_reset();
SELECT test_context_value();
