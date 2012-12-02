CREATE FUNCTION conv(o json) RETURNS json AS $$
if (o instanceof Array) {
	o[1] = 10;
} else if (typeof(o) == 'object') {
	o.i = 10;
}
return o;
$$ LANGUAGE plv8;

SELECT conv('{"i": 3, "b": 20}');
SELECT conv('[1, 2, 3]');
