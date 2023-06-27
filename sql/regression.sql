-- https://github.com/plv8/plv8/issues/527
DO $$
  plv8.prepare('SELECT * FROM (VALUES (1)) AS foo').cursor().fetch.apply(null)
$$ language plv8;

-- https://github.com/plv8/plv8/issues/540
CREATE FUNCTION test_array_buffer_int8_offset() RETURNS bytea AS $$
	let ab = new ArrayBuffer(3);
	let view = new Uint8Array(ab);
	view[0] = 6;
	view[1] = 9;
  view[2] = 12;
	
	let r = new Uint8Array(ab, 1);
	return r; 
$$ LANGUAGE plv8;

SELECT test_array_buffer_int8_offset();

CREATE FUNCTION test_array_buffer_int16_offset() RETURNS bytea AS $$
	let ab = new ArrayBuffer(6);
	let view = new Uint16Array(ab);
	view[0] = 6;
	view[1] = 9;
  view[2] = 12;
	
	let r = new Uint16Array(ab, 2);
	return r; 
$$ LANGUAGE plv8;

SELECT test_array_buffer_int16_offset();

CREATE FUNCTION test_array_buffer_int32_offset() RETURNS bytea AS $$
	let ab = new ArrayBuffer(12);
	let view = new Uint32Array(ab);
	view[0] = 6;
	view[1] = 9;
  view[2] = 12;
	
	let r = new Uint32Array(ab, 4);
	return r; 
$$ LANGUAGE plv8;

SELECT test_array_buffer_int32_offset();
