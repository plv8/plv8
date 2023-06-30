# PLV8

PLV8 is a _trusted_ Javascript language extension for PostgreSQL.  It can be
used for _stored procedures_, _triggers_, etc.

PLV8 works with most versions of Postgres, but works best with `9.1` and above,
including `10.0`, `11`, `12`, and `13`.

## Installing PLV8

If the PLV8 extension has been installed to your system, the PLV8 extension can
be installed into your PostgreSQL database by running:

```
=# CREATE EXTENSION plv8;
```

### Verifying Your Installation

You can verify the installation in two ways.  As of PLV8 `2.0.0`, you can
execute a stored procedure:

```
=# SELECT plv8_version();
```

Alternately, you can run the following on all versions of PLV8:

```
=# DO $$ plv8.elog(NOTICE, plv8.version); $$ LANGUAGE plv8;
```

## Updating PLV8

As of PLV8 version `2.3.3`, you can use upgrade scripts to upgrade your
installation from any version higher than `1.5.0`:

```
=# ALTER EXTENSION plv8 UPDATE TO `3.0.0`;
```

Note that until the database has been restarted, the old version of PLV8 will
still be loaded, though `SELECT plv8_version()` will return the new version.
This is an artifact of how Postgres manages extensions.

### Updating Older PLV8 Installs

Updating PL/v8 is usually straightforward as it is a small and stable extension
- it only contains a handful of objects that need to be added to PostgreSQL
when installing the extension.

The procedure that is responsible for invoking this installation script
(generated during compile time based on plv8.sql.common), is controlled by
PostgreSQL and runs when CREATE EXTENSION is executed only. After building, it
takes the form of plv8--<version>.sql and is usually located under
`/usr/share/postgresql/<PG_MAJOR>/extension`, depending on the OS.

When this command is executed, PostgreSQL tracks which objects belong to the
extension and conversely removes them upon uninstallation, i.e., whenever
`DROP EXTENSION` is called.

You can explore some of the objects that PL/v8 stores under PostgreSQL:

```
=# SELECT lanname FROM pg_catalog.pg_language WHERE lanname = 'plv8';
=# SELECT proname FROM pg_proc p WHERE p.proname LIKE 'plv8%';
=# SELECT typname FROM pg_catalog.pg_type WHERE typname LIKE 'plv8%';
```

To update PostgreSQL, you can `DROP` the existing extension:

```
=# DROP EXTENSION plv8;
```

Install the new version, and `CREATE` the extension:

```
=# CREATE EXTENSION plv8;
```

Alternately, you can backup and restore your database.

## Runtime Environment Separation

In PLV8, each session has one global JS runtime context. This enables function
invocations at low cost, and sharing common object among the functions. However,
for the security reasons, if the user switches to another with SET ROLE command,
a new JS runtime context is initialized and used separately. This prevents the
risk of unexpected information leaking.

Each `plv8` function is invoked as if the function is the property of other
object. This means `this` in each function is a Javascript `object` that is created
every time the function is executed in a query. In other words, the life time and
the visibility of the `this` object in a function is only a series of function calls in
a query. If you need to share some value among different functions, keep it in the
global `plv8` object because each function invocation has a different `this` object.

## Start-up Procedure

PLV8 provides a start up facility, which allows you to call a `plv8` runtime
environment initialization function specified in the GUC variable.  This can
only be set by someone with administrator access to the database you are
accessing.

```
SET plv8.start_proc = 'plv8_init';
SELECT plv8_test(10);
```

If this variable is set when the runtime is initialized, before the function
call of `plv8_test()` another `plv8` function `plv8_init()` is invoked. In such
initialization function, you can add any properties to `plv8` object to expose
common values or assign them to the this property. In the initialization function,
the receiver this is specially pointing to the global object, so the variables
that are assigned to the this property in this initialization are visible from
any subsequent function as global variables.

Remember `CREATE FUNCTION` also starts the `plv8` runtime environment, so make
sure to `SET` this GUC before any `plv8` actions including `CREATE FUNCTION`.

## Stored procedures

There are a couple of utility procedures that will get installed.

These are useful for long-running connections from various backend services.

### plv8_info

Get information about all the current running environments 
on a specific connection from all users.

Can be run by superuser only.

```sql
SELECT plv8_info();
```

Outputs JSON

```json
[
  {
    "user":"user1",
    "total_heap_size":1327104,
    "total_physical_size":474336,
    "used_heap_size":386680,
    "heap_size_limit":270008320,
    "external_memory":0,
    "number_of_native_contexts":2,
    "contexts":[]
  },
  {
    "user":"user2",
    "total_heap_size":1327104,
    "total_physical_size":474336,
    "used_heap_size":386680,
    "heap_size_limit":270008320,
    "external_memory":0,
    "number_of_native_contexts":3,
    "contexts":["my context"]
  }
]
```

_Note: "number_of_native_contexts" = "contexts".length + 2_

### plv8_reset

Reset user isolate or context

To reset a specific context run:
```sql
SELECT plv8_reset('my context');
```

Will reset the context and re-create `globalThis` on a next invocation of that context.

To reset all contexts and reboot the whole environment of a specfifc user:
```sql
SELECT plv8_reset();
```

Superusers can kill a specific user environment by impersonating the user:

```sql
SET ROLE "some_user";
SELECT plv8_reset();
RESET ROLE;
```
