#!/bin/bash
idir=/home/`whoami`/Repository/plv8js
mkdir -p $idir
cd $idir
#hg clone https://plv8js.googlecode.com/hg/ .
#hg pull https://plv8js.googlecode.com/hg/
#hg push # for push to googlecode
#hg commit -u username@gmail.com -m "commit message"
g++ -O2 -Wall -fPIC -I ../postgres/pgsql/src/include -I ../v8/include -o plv8.o -c plv8.cc
g++ -lv8 -shared -o plv8.so plv8.o
ifile=/home/`whoami`/Software/PostgreSQL-9/lib/plv8.so
sudo rm -f $ifile
sudo ln -s $idir/plv8.so $ifile

echo '';
echo '======= testing PLV8'
echo 'DROP FUNCTION IF EXISTS plv8_call_handler() CASCADE; DROP FUNCTION plv8_call_validator(Oid) CASCADE;' | psql
echo "CREATE FUNCTION plv8_call_handler() RETURNS language_handler AS '/home/`whoami`/Software/PostgreSQL-9/lib/plv8' LANGUAGE C; CREATE FUNCTION plv8_call_validator(Oid) RETURNS void AS '/home/`whoami`/Software/PostgreSQL-9/lib/plv8' LANGUAGE C; CREATE LANGUAGE plv8 HANDLER plv8_call_handler VALIDATOR plv8_call_validator;" | psql

echo '';
echo '======= testing Function'
echo 'CREATE OR REPLACE FUNCTION plv8_test(keys text[], vals text[]) RETURNS
text AS $$
var o = {};
for(var i=0; i<keys.length; i++){
 o[keys[i]] = vals[i];
}
return JSON.stringify(o);
$$ LANGUAGE plv8 IMMUTABLE STRICT; ' | psql
echo "SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);" | psql

echo '';
echo '======= create test TABLE';
echo 'DROP TABLE IF EXISTS test CASCADE;' | psql
echo 'CREATE TABLE test ( za BIGSERIAL PRIMARY KEY, ztext TEXT );' | psql
echo "INSERT INTO test(ztext) VALUES( 'a'),( 'b'),( 'c');" | psql

echo '';
echo '======= testing SPI'
echo '
CREATE OR REPLACE FUNCTION testing123() RETURNS text AS $____$
var x = new SPI();
//return x.query("INSERT INTO test (ztext) VALUES( $$x$$ ) RETURNING * ");
return x.query("SELECT ztext FROM test");
$____$ LANGUAGE "plv8";
' | psql
echo 'SELECT testing123();' | psql

echo '';
echo '======= testing Trigger'
echo '
CREATE OR REPLACE FUNCTION testingT() RETURNS trigger AS $____$
var x = new TRIG();
var y = new LOG();
y.notice( "called from " + x.event() );
return; // harusnya mengembalikan sesuatu.. apa ya?
$____$ LANGUAGE "plv8";
' | psql
echo 'CREATE TRIGGER onDel1 AFTER DELETE ON test EXECUTE PROCEDURE testingT();' | psql
echo 'DELETE FROM test;' | psql

