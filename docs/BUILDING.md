# Building

## Building for MacOS/Linux

Building PLV8 for MacOS or Linux has some specific requirements:

* Git
* g++ or clang++
* Python 2 (for v8)
* pkg-config (linux only for v8)
* libc++-dev (linux only)
* libc++abi-dev (linux only)

### Downloading Source

Downloading the source code is very straightforward:

```
$ wget https://github.com/plv8/plv8/archive/v2.3.8.tar.gz
$ tar -xvzf v2.3.8.tar.gz
$ cd plv8-2.3.8
$ make
```

### Building

Building is simple:

```
$ make
```

This will download `v8` and compile it as well.

| Note: If you have multiple versions of PostgreSQL installed like 9.5 and 9.6, Plv8 will only be built for PostgreSQL 9.6. This is because make calls pg_config to get the version number, which will always be the latest version installed. If you need to build Plv8 for PostgreSQL 9.5 while you have 9.6 installed pass make the PG_CONFIG variable to your 9.5 version of pg_config. This works for `make`, and `make install`. For example in Ubuntu:

```
$ make PG_CONFIG=/usr/lib/postgresql/9.5/bin/pg_config
```

### Building with Execution Timeout

Plv8 allows you to optionally build with an execution timeout for Javascript
functions, when enabled at compile-time.

```
$ make EXECUTION_TIMEOUT=1
```

By default, the execution timeout is not compiled, but when configured it has a
timeout of 300 seconds (5 minutes). You can override this by setting the
`plv8.execution_timeout` variable. It can be set between `1` second and `65536`
seconds, but cannot be disabled.

### Building with ICU

Building with ICU requires you to enable ICU in your build process:

```
$ make USE_ICU=1
```

If you build with ICU, you will need to install the correct ICU file, located in
`contrib/icu`.

* icudtl.dat - Little Endian architectures (Intel)
* icudtb.dat - Big Endian architectures (Sparc)

For ARM, you will need to figure out which Endianess your hardware and OS is
configured for.

NOTE: it is important that the user that Postgres is started with has read
access to the file.

### Installing

Once PLV8 has been built, you need to install it for PostgreSQL to be able to use
it:

```
$ make install
```

This might require `root` access, depending on how PostgreSQL is installed:

```
$ sudo make install
```


### Testing

Once PLV8 is installed, you can verify the install by running:

```
$ make installcheck
```

## Building for Windows

Building PLV8 for Windows has some specific requirements:

* Git
* MSVC 2013, 2015, or 2017
* CMake - available as part of MSVC
* Postgres 9.5+ (it will work in 9.3 and 9.4, but will involve extra work)

Additional requirements to build V8:

* Python 2
* unzip.exe
* patch.exe - part of the Git install

### Patching Postgres

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

### Bootstrapping

Bootstrapping will the build environment, download, and compile `v8`.  Watch for
any errors:

```
PS> bootstrap.bat
```

### Configuring

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

### Compiling

After successfully configuring your build environment, compiling should be easy:

```
PS> cmake --build . --config Release --target Package
```

This will build and package the extension for installation.

### Installing

To install, you simply need to `unzip` the file created.  The name will depend
on the version of PLV8 and the version of Postgres.  An example is
`plv8-2.3.1-postgresql-10-x64.zip`.

# Build FAQ

## pg_config build error / mb.py returns non-zero exit status

If you get this error, you need to install pkg-config (e.g. on Ubuntu, apt-get install pkg-config)
```
Traceback (most recent call last):
  File "tools/dev/v8gen.py", line 304, in <module>
    sys.exit(gen.main())
  File "tools/dev/v8gen.py", line 298, in main
    return self._options.func()
  File "tools/dev/v8gen.py", line 166, in cmd_gen
    gn_outdir,
  File "tools/dev/v8gen.py", line 208, in _call_cmd
    stderr=subprocess.STDOUT,
  File "/usr/lib/python2.7/subprocess.py", line 223, in check_output
    raise CalledProcessError(retcode, cmd, output=output)
subprocess.CalledProcessError: Command '['/usr/bin/python', '-u', 'tools/mb/mb.py', 'gen', '-f', 'infra/mb/mb_config.pyl', '-m', 'developer_default', '-b', 'x64.release', 'out.gn/x64.release']' returned non-zero exit status 1
Makefile:35: recipe for target 'build/v8' failed
make: *** [build/v8] Error 1
ERROR: command returned 2: make PG_CONFIG=/usr/bin/pg_config all
```
