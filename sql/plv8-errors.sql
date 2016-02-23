CREATE FUNCTION test_sql_error() RETURNS void AS $$ plv8.execute("ERROR") $$ LANGUAGE plv8;
SELECT test_sql_error();

CREATE FUNCTION catch_sql_error() RETURNS void AS $$
try {
	plv8.execute("throw SQL error");
	plv8.elog(NOTICE, "should not come here");
} catch (e) {
	plv8.elog(NOTICE, e);
}
$$ LANGUAGE plv8;
SELECT catch_sql_error();

CREATE FUNCTION catch_sql_error_2() RETURNS text AS $$
try {
	plv8.execute("throw SQL error");
	plv8.elog(NOTICE, "should not come here");
} catch (e) {
	plv8.elog(NOTICE, e);
	return plv8.execute("select 'and can execute queries again' t").shift().t;
}
$$ LANGUAGE plv8;
SELECT catch_sql_error_2();

-- subtransaction()
CREATE TABLE subtrant(a int);
CREATE FUNCTION test_subtransaction_catch() RETURNS void AS $$
try {
	plv8.subtransaction(function(){
		plv8.execute("INSERT INTO subtrant VALUES(1)");
		plv8.execute("INSERT INTO subtrant VALUES(1/0)");
	});
} catch (e) {
	plv8.elog(NOTICE, e);
	plv8.execute("INSERT INTO subtrant VALUES(2)");
}
$$ LANGUAGE plv8;
SELECT test_subtransaction_catch();
SELECT * FROM subtrant;

TRUNCATE subtrant;
CREATE FUNCTION test_subtransaction_throw() RETURNS void AS $$
plv8.subtransaction(function(){
	plv8.execute("INSERT INTO subtrant VALUES(1)");
	plv8.execute("INSERT INTO subtrant VALUES(1/0)");
});
$$ LANGUAGE plv8;
SELECT test_subtransaction_throw();
SELECT * FROM subtrant;

-- exception handling
CREATE OR REPLACE FUNCTION v8_test_throw() RETURNS float AS
$$ 
throw new Error('Error');
$$
LANGUAGE plv8;

CREATE OR REPLACE FUNCTION v8_test_catch_throw() RETURNS text AS
$$
 try{
   var res = plv8.execute('select * from  v8_test_throw()');
 } catch(e) {
   return JSON.stringify({"result": "error"});
 }
$$
language plv8;

SELECT v8_test_catch_throw();

CREATE FUNCTION catch_elog_error() RETURNS void AS $$
function f (){
}
f.prototype.toString = function(){
        throw 'custom exception';
}
try {
	plv8.elog(NOTICE, (new f));
	plv8.elog(NOTICE, "should not come here");
} catch (e) {
	plv8.elog(NOTICE, 'catch result:'+ e);
}
$$ LANGUAGE plv8;
SELECT catch_elog_error();

CREATE FUNCTION catch_elog_error2() RETURNS void AS $$
function f (){
}
f.prototype.toString = function(){
        throw 'custom exception';
}

plv8.elog(NOTICE, (new f));
plv8.elog(NOTICE, "should not come here");

$$ LANGUAGE plv8;
SELECT catch_elog_error2();

