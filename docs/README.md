# PLV8

PLV8 is a _trusted_ Javascript language extension for PostgreSQL.  It can be
used for _stored procedures_, _triggers_, etc.

PLV8 works with most versions of Postgres, but works best with `9.1` and above,
including `10.0` and `10.1`.

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
installation from any verion higher than `1.5.0`:

```
=# ALTER EXTENSION plv8 UPDATE TO `2.3.3`;
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
