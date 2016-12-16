A Procedural Language in JavaScript powered by V8
=================================================

plv8 is a shared library that provides a PostgreSQL procedural language powered
by V8 JavaScript Engine.  With this program you can write in JavaScript your
function that is callable from SQL.

# Requirements:
plv8 is tested with:

- PG: version 9.2, 9.3, 9.4 and 9.5 (maybe older/newer are allowed)
- V8: version 4.4 to 4.10
- g++: version 4.8.2
- clang++

Also all tools that PostgreSQL and V8 require to be built are required if you
are building those from source.

# Installing plv8

## Build from source:
```shell
$ git clone git@github.com:plv8/plv8.git
$ cd ./plv8
$ git checkout tags/v1.5.4 -b 1.5.4
$ make static
```
This will build plv8 for you linking to Google's v8 as a static library by 
downloading the v8 source at a specific version and building it along with 
plv8. The build will be for the highest PostgreSQL version you have installed 
on the system. You can alternatively run just `make` and it will build plv8 
dynamically linking to Google's `libv8` library on your system. There are 
some issues with this as several linux distros ship a very old version of 
`libv8`. The `3.x` versions of v8 will work with the `1.4.x` versions of plv8, 
but to build the later versions of plv8 you need a v8 minimum version of 
`4.4.x`, but can also use v8 version `5.x.x` (TODO: what version???). PGXN 
install will use the dynamically linked `libv8` library.

> Note: If you have multiple versions of PostgreSQL installed like 9.5 and 9.6, 
plv8 will only be built for PostgreSQL 9.6. This is because `make static` calls 
`pg_config` to get the version number, which will always be the latest version 
installed. If you need to build plv8 for PostgreSQL 9.5 while you have 9.6 
installed, you can "hide" 9.6 from `pg_config`. One way to do this in Ubuntu 
is to move the 9.6 directory:
TODO: (Is there a better way?)
```shell
$ mv /usr/lib/postgresql/9.6 /usr/lib/
# Then build pv8.
$ make static
# Then move it back.
$ mv /usr/lib/9.6 /usr/lib/postgresql/
```

> Note: In `mingw64`, you may have difficulty in building plv8. If so, try to 
make the following changes in Makefile. For more detail, please refer to 
https://github.com/plv8/plv8/issues/29
```
  CUSTOM_CC = gcc
  SHLIB_LINK := $(SHLIB_LINK) -lv8 -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic -lm
```

## Test the Build
plv8 supports installcheck test.  Make sure to set `custom_variable_classes = 'plv8'` 
in your postgresql.conf (before 9.2) and run:
```shell
$ make installcheck
```

## Installing the build:
After running `make` or `make static` the following files must be copied to the 
correct location for PostgreSQL to find them:
#### plv8 JavaScript Extension:
- `plv8.so`
-- On Ubuntu: `$ cp ./plv8.so /usr/lib/postgres/{your-postgresql-version-here}/lib`
-- e.g. `$ cp ./plv8.so /usr/lib/postgres/9.6/lib`
- `plv8.control`
-- On Ubuntu: `$ cp ./plv8.control/usr/share/postgres/{your-postgresql-version-here}/extension`
-- e.g. `$ cp ./plv8.control/usr/share/postgres/9.6/extension`
- `plv8--{plv8-build-version-here}.sql`
-- On Ubuntu: `$ cp ./plv8--{plv8-build-version-here}.sql /usr/share/postgres/{your-postgresql-version-here}/extension`
-- e.g. `$ cp ./plv8--1.5.4.sql /usr/share/postgres/9.6/extension`

The following files will also be build and can be optionally installed if you 
need the CoffeeScript or LiveScript versions:
#### CoffeeScript Extension:
- `plcoffee.so`
-- On Ubuntu: `$ cp ./plcoffee.so /usr/lib/postgres/{your-postgresql-version-here}/lib`
-- e.g. `$ cp ./plcoffee.so /usr/lib/postgres/9.6/lib`
- plcoffee.control
-- On Ubuntu: `$ cp ./plcoffee.control/usr/share/postgres/{your-postgresql-version-here}/extension`
-- e.g. `$ cp ./plcoffee.control/usr/share/postgres/9.6/extension`
- plcoffee--{plv8-build-version-here}.sql
-- On Ubuntu: `$ cp ./plcoffee--{plv8-build-version-here}.sql /usr/share/postgres/{your-postgresql-version-here}/extension`
-- e.g. `$ cp ./plcoffee--1.5.4.sql /usr/share/postgres/9.6/extension`

#### LiveScript Extension:
- `plls.so`
-- On Ubuntu: `$ cp ./plls.so /usr/lib/postgres/{your-postgresql-version-here}/lib`
-- e.g. `$ cp ./plls.so /usr/lib/postgres/9.6/lib`
- plls.control
-- On Ubuntu: `$ cp ./plls.control/usr/share/postgres/{your-postgresql-version-here}/extension`
-- e.g. `$ cp ./plls.control/usr/share/postgres/9.6/extension`
- plls--{plv8-build-version-here}.sql
-- On Ubuntu: `$ cp ./plls--{plv8-build-version-here}.sql /usr/share/postgres/{your-postgresql-version-here}/extension`
-- e.g. `$ cp ./plls--1.5.4.sql /usr/share/postgres/9.6/extension`

## Debian/Ubuntu 14.04 and 16.04:
You can install plv8 using `apt-get`, but it will be version `v1.4.8` 
(As of 2016-12-16).
```shell
$ apt-get install postgresql-{your-postgresql-version-here}-plv8
# e.g.
$ apt-get install postgresql-9.1-plv8
# OR up to
$ apt-get install postgresql-9.6-plv8
```

## Redhat/CentOS:
TODO

## MacOS:
TODO

## Windows:
TODO

# Install the plv8 Extensions on a Database:
Once the plv8 extensions have been added to the server, you should restart the 
PostgreSQL service. Then you can connect to the server and install the extensions 
on a database by running the following SQL queries in PostgreSQL version 9.1 or 
later:
```sql
  CREATE EXTENSION plv8;
  CREATE EXTENSION plls;
  CREATE EXTENSION plcoffee;
```

Make sure to set `custom_variable_classes = 'plv8'` in your `postgresql.conf` file 
for PostgreSQL versions before 9.2.

In the versions prior to 9.1 run the following to create database objects:
```shell
$ psql -f plv8.sql
```

## Testing plv8 on a database:
Below are some example queries to test if the extension is working:
```sql
  DO $$
    plv8.elog(WARNING, 'plv8.version = ' + plv8.version); -- Will output the plv8 installed as a PostgreSQL `WARNING`.
  $$ LANGUAGE plv8;
```
As of 2.0.0, there is a function to determine which version of plv8 you have
installed:
```sql
  SELECT plv8_version();
```

### JavaScript Example
```sql
  CREATE OR REPLACE FUNCTION plv8_test(keys text[], vals text[])
  RETURNS text AS $$
    var o = {};
    for(var i=0; i<keys.length; i++){
      o[keys[i]] = vals[i];
    }
    return JSON.stringify(o);
  $$ LANGUAGE plv8 IMMUTABLE STRICT;

  SELECT plv8_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
           plv8_test
  ---------------------------
   {"name":"Tom","age":"29"}
  (1 row)
```

### CoffeeScript Example
```sql
  CREATE OR REPLACE FUNCTION plcoffee_test(keys text[], vals text[])
  RETURNS text AS $$
    return JSON.stringify(keys.reduce(((o, key, idx) ->
      o[key] = vals[idx]; return o), {}), {})
  $$ LANGUAGE plcoffee IMMUTABLE STRICT;

  SELECT plcoffee_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
         plcoffee_test
  ---------------------------
   {"name":"Tom","age":"29"}
  (1 row)
```

### LiveScript Example
```sql
  CREATE OR REPLACE FUNCTION plls_test(keys text[], vals text[])
  RETURNS text AS $$
    return JSON.stringify { [key, vals[idx]] for key, idx in keys }
  $$ LANGUAGE plls IMMUTABLE STRICT;

  SELECT plls_test(ARRAY['name', 'age'], ARRAY['Tom', '29']);
           plls_test
  ---------------------------
   {"name":"Tom","age":"29"}
  (1 row)
```

# Notes:
plv8 is hosted on github at:
https://github.com/plv8/plv8

plv8 is distributed by PGXN.  For more detail, see:
http://pgxn.org/dist/plv8/doc/plv8.html

