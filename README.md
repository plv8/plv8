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
`4.4.63.31`, but can also use v8 version `5.1.281.14`. PGXN 
install will use the dynamically linked `libv8` library.

If you would like to use `make` and you system does not have a new enough 
version of `libv8` installed, see the `.travis.yml` file in the repo to see 
how out CI test servers build v8 natively.

> Note: If you have multiple versions of PostgreSQL installed like 9.5 and 9.6, 
plv8 will only be built for PostgreSQL 9.6. This is because `make static` calls 
`pg_config` to get the version number, which will always be the latest version 
installed. If you need to build plv8 for PostgreSQL 9.5 while you have 9.6 
installed pass `make` the `PG_CONFIG` variable to your 9.5 version of 
`pg_config`. This works for `make`, `make static`, `make install`. For example 
in Ubuntu:
```shell
$ make PG_CONFIG=/usr/lib/postgresql/9.5/bin/pg_config
```

> Note: You may run into problems with your C++ complier version. You can pass 
`make` the `CUSTOM_CC` variable to change the complier. For example, to use 
`g++` version 4.9:
```shell
$ make CUSTOM_CC g++-4.9
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
- `plv8.control`
- `plv8--{plv8-build-version-here}.sql`

The following files will also be build and can be optionally installed if you 
need the CoffeeScript or LiveScript versions:
#### CoffeeScript Extension:
- plcoffee.control
- plcoffee--{plv8-build-version-here}.sql

#### LiveScript Extension:
- plls.control
- plls--{plv8-build-version-here}.sql

### Automatically Install the Build
You can install the build for your system by running:
```shell
$ make install
```

> Note: You should do this a root/admin. `sudo make install`

> Note: If you need to install plv8 for a different version of PostgreSQL, pass 
the `PG_CONFIG` variable. See above.

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

