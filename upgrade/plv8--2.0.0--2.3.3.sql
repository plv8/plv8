CREATE OR REPLACE FUNCTION plv8_version ( )
RETURNS TEXT AS
$$
	return "2.3.3";
$$ LANGUAGE plv8;
