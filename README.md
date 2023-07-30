# PLV8 - A Procedural Language in Javascript powered by V8

PLV8 is a shared library that provides a PostgreSQL procedural language powered
by V8 Javascript Engine. With this program you can write in Javascript your
function that is callable from SQL.

## Support

There is a [Discord](https://discord.gg/5fJN52Se) available for general questions and support. Please
ask there before opening an issue.

## Building

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

## Installing

    =# CREATE EXTENSION plv8;

This will install PLV8 into your database if it exists as an extension.

## Testing

To test, you can execute:

    =# DO $$ plv8.elog(NOTICE, "hello there!"); $$ LANGUAGE plv8;

For full documentation, see [https://plv8.github.io/](https://plv8.github.io/).

## Docker

For preliminary Docker support, see [./platforms/Docker/README.md](./platforms/Docker/README.md)
