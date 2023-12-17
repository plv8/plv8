CREATE OR REPLACE FUNCTION jsonb_scalar_cat(data jsonb) RETURNS jsonb LANGUAGE plv8 AS
$$ plv8.elog(NOTICE, 'arg = ' + JSON.stringify(data));  return data; $$;

SELECT jsonb_scalar_cat(to_jsonb('asd'::TEXT));

SELECT jsonb_scalar_cat(to_jsonb(19450509::INT));

SELECT jsonb_scalar_cat(to_jsonb(false::BOOL));

SELECT jsonb_scalar_cat('{"key":[null, true, false, 19450509, "string"]}'::JSONB);

SELECT jsonb_scalar_cat('{"key":true}'::JSONB);

SELECT jsonb_scalar_cat('{"key":"true"}'::JSONB);

SELECT jsonb_scalar_cat('{"key":19450509}'::JSONB);

SELECT jsonb_scalar_cat('{"key":null}'::JSONB);


--SELECT jsonb_scalar_cat('null'::TEXT::JSONB);
