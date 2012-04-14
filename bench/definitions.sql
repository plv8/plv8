create or replace function plbench(query text, n int) returns float as $$
declare
	t0 timestamp with time zone;
	e float;
begin
	t0 := clock_timestamp();
	for i in 1 .. n loop
		execute query;
	end loop;
	e = extract(microseconds from clock_timestamp()) -
			extract(microseconds from t0);
	return e / 1000;
end;
$$ language plpgsql;

create or replace function js_add(a int, b int) returns int as $$
	return a + b;
$$ language plv8 immutable strict;

create or replace function py_add(a int, b int) returns int as $$
	return a + b;
$$ language plpythonu immutable strict;

create or replace function pg_add(a int, b int) returns int as $$
begin
	return a + b;
end;
$$ language plpgsql immutable strict;

create or replace function callee(i int) returns int as $$
	return i * i;
$$ language plv8;

create or replace function caller_naive(i int) returns int as $$
	var func = plv8.find_function("callee");
	return func(i) + func(i * i);
$$ language plv8;

create or replace function caller_cache(i int) returns int as $$
	if(!this.func){
		this.func = plv8.find_function("callee");
	}
	return this.func(i) + this.func(i * i);
$$ language plv8;
