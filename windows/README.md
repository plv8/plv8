# Building for Windows

Building PLV8 for Windows has some specific requirements:

* Git
* MSVC 2013, 2015, or 2017
* CMake - available as part of MSVC
* Postgres 9.5+ (it will work in 9.3 and 9.4, but will involve extra work)

Additional requirements to build V8:

* Python 2
* unzip.exe
* patch.exe - part of the Git install

## Patching Postgres

Currently, Postgres requires a patch of one or more `include` files in order to
compile PLV8.

First, find the directory that contains the `include` files.  This will typically
be inside something like `C:\Program Files\PostgreSQL\10\include`, where the `10`
is your version number.  Inside of the `include` directory:

```
PS> cd server\port\atomics
PS> copy \plv8\windows\generic-msvc.h.patch .
PS> patch < generic-msvc.h.patch
```

## Bootstrapping

Bootstrapping will the build environment, download, and compile `v8`.  Watch for
any errors:

```
PS> bootstrap.bat
```

## Configuring

Once `v8` has been built, you can configure your build environment.  This involves
specifying the path to your Postgres install, the version of Postgres you are
running, as well as the build target.  Build targets will typically be one of the
following:

* `Visual Studio 15 2017` - 32 bit, MSVC 2017
* `Visual Studio 15 2017 Win64` - 64 bit, MSVC 2017
* `Visual Studio 14 2015` - 32 bit, MSVC 2015
* `Visual Studio 14 2015 Win64` - 64 bit, MSVC 2015
* `Visual Studio 12 2013` - 32 bit, MSVC 2013
* `Visual Studio 12 2013 Win64` - 64 bit, MSVC 2013

```
PS> cmake . -G "Visual Studio 15 2017 Win64" -DCMAKE_INSTALL_PREFIX="C:\Program Files\PostgreSQL\10" -DPOSTGRESQL_VERSION=10
```

## Compiling

After successfully configuring your build environment, compiling should be easy:

```
PS> cmake --build . --config Release --target Package
```

This will build and package the extension for installation.

## Installing

To install, you simply need to `unzip` the file created.  The name will depend
on the version of PLV8 and the version of Postgres.  An example is
`plv8-2.3.1-postgresql-10-x64.zip`.

## TODO

* Generate configuration files
* Generate control files
* Generate sql files
