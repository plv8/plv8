PL/v8 - A Procedural Language in JavaScript powered by V8
=================================================

PL/v8 is a shared library that provides a PostgreSQL procedural language powered
by V8 JavaScript Engine.  With this program you can write in JavaScript your
function that is callable from SQL.

## Documentation
The documentation covers the following implemented features:

- [Requirements](/doc/plv8.md#requirements)
- [Installing PL/v8](/doc/plv8.md#installing-plv8)
- [Install the PL/v8 Extensions on a Database](/doc/plv8.md#install-the-plv8-extensions-on-a-database)
- [Scalar function calls](/doc/plv8.md#scalar-function-calls)
- [Set returing function calls](/doc/plv8.md#set-returning-function-calls)
- [Trigger function calls](/doc/plv8.md#trigger-function-calls)
- [Inline statement calls](/doc/plv8.md#inline-statement-calls)
- [Auto mapping between JS and database built-in types](/doc/plv8.md#auto-mapping-between-js-and-database-built-in-types)
- [Database access via SPI including prepared statements and cursors](/doc/plv8.md#database-access-via-spi-including-prepared-statements-and-cursors)
- [Subtransaction](/doc/plv8.md#subtransaction)
- [Utility functions](/doc/plv8.md#utility-functions)
- [Window function API](/doc/plv8.md#window-function-api)
- [Typed array](/doc/plv8.md#typed-array)
- [ES6 Language Features](/doc/plv8.md#es6-language-features)
- [Remote debugger](/doc/plv8.md#remote-debugger)
- [Runtime environment separation across users in the same session](/doc/plv8.md#runtime-environment-separation-across-users-in-the-same-session)
- [Start-up procedure](/doc/plv8.md#start-up-procedure)
- [Update procedure](/doc/plv8.md#update-procedure)
- [Dialects](/doc/plv8.md#dialects)

## Notes:
PL/v8 is hosted on github at:
https://github.com/plv8/plv8

PL/v8 is distributed by PGXN.  For more detail, see:
http://pgxn.org/dist/plv8/doc/plv8.html

Mailing List:
https://groups.google.com/forum/#!forum/plv8js
## Installing PL/v8 for Windows(MSVC)

```shell
bootstrap.bat
cmake . -G "Visual Studio 14 2015 Win64" -DCMAKE_INSTALL_PREFIX="C:\Program Files\PostgreSQL\9.6" -DPOSTGRESQL_VERSION=9.6
cmake --build . --config Release --target Package
```
