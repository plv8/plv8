# PLV8 - A Procedural Language in Javascript powered by V8

PLV8 is a shared library that provides a PostgreSQL procedural language powered
by V8 Javascript Engine. With this program you can write in Javascript your
function that is callable from SQL.

## Support

There is a [Discord](https://discord.gg/5fJN52Se) available for general questions and support. Please
ask there before opening an issue.

## Build Requirements

Note that as PLV8 3.2, build requirements have again changed.

### Linux

The following packages must be installed to build on Ubuntu or Debian:

- `libtinfo5`
- `build-essential`
- `pkg-config`
- `libstdc++-12-dev` (depending on version, may be 10 instead of 12)
- `cmake`
- `git`

The following packages must be installed to build on EL9 or EL8:

- 'development tools' - via groupinstall
- `cmake`
- `git`

### MacOS

The following packages must be install to build on MacOS:

- `XCode` - and the command line tools
- `cmake`

## Building

Building plv8 needs to have all build requirements fulfilled before building.
You must make sure that `pg_config` is in your path. It should share the same
installation directory as `psql` and `postgres`.

```sh
make
```

### Installing

This should install plv8 as an available extension into Postgres.

```sh
make install
```

### Run Tests

Postgres features a test runner, and plv8 includes a number of tests that can be
run.

```sh
make installcheck
```

## Running

    =# CREATE EXTENSION plv8;

This will install PLV8 into your database if it exists as an available extension.

## Testing

To test, you can execute:

    =# DO $$ plv8.elog(NOTICE, "hello there!"); $$ LANGUAGE plv8;

For full documentation, see [https://plv8.github.io/](https://plv8.github.io/).

## Docker

For Docker support, see [./platforms/Docker/README.md](./platforms/Docker/README.md)
