# Building and Adding plv8 to a Postgres installation

For technical support, please file a [github issue](https://github.com/plv8/plv8/issues) and we'll try to respond quickly.

## Cloud Hosted Postgres (as of 2019-02-25)

Generally, cloud hosted database services (e.g. Amazon RDS) either have plv8 include or not, and if it's not included, then (for good security reasons!) you can't add it. Instead, you must run your own database instance(s) using the lower-level virtual machine services, such as Docker/Kubernetes (preferred) or individual instances.

Obviously, cloud providers offer the default build-settings - see below for custom plv8 builds including optional features.

note: this section mentions commercial services - feel free to submit pull requests if we got anything wrong or there's updated info.

[Heroku](https://heroku.com): plv8 is pre-installed on [Heroku Postgres](https://www.google.com/search?q=heroku+plv8).

[Amazon Web Services (AWS)](https://aws.amazon.com): plv8 is pre-installed on [Postgres RDS](https://www.google.com/search?q="amazon+rds"+"plv8+extension") and [Postgres Aurora](https://www.google.com/search?q="amazon+aurora"+"plv8+extension"), _not_ available and *cannot* be added to 
[Amazon RedShift](https://www.google.com/search?q="redshift"+"plv8"). You can of course run your own Postgres instances using AWS EC2 or AWS EKS (Kubernetes) - see below.

[Google Cloud](https://cloud.google.com): plv8 is _not_ available and *cannot* be installed on [GooEC2gle Cloud PostgreSQL](https://cloud.google.com/sql/docs/postgres/extensions#language). Instead, you must install your own database instance, either using individual Compute Engine instances, or Kubernetes Engine instances. COMING SOON: detailed instructions.

[Citus DB](http://citusdata.com): Citus Cloud does not include plv8 by default. As a regular Postgres extension itself, Citus  works well with plv8 when self-hosted. 

[Azure](https://azure.microsoft.com/) and others: documentation coming soon!


## Self-Hosted Postgres Instances (cloud, on-premise, laptops, etc)

"Self-hosting" refers to running your own instance(s) of Postgres, which carries the burden of arranging for monitoring, scaling, upgrades, security patching, etc.

### Pre-built/pre-compiled plv8 (recommended)

Building plv8 is slow, has a number of dependencies and can fail with obscure error messages. For this reasons, we offer pre-built plv8 binaries for selected platforms. If you'd like to contribute additional platforms, please submit a pull request with a Dockerfile for your platform.

Obviously, pre-built binaries use the default build-settings - see below for custom plv8 builds including optional features.

#### pre-built plv8 for Debian/Ubuntu Linux

...

#### pre-built plv8 for Docker - Debian/Ubuntu Linux

For best results, use [Docker multi-stage builds](https://www.google.com/search?q=docker+multi-stage+builds) to include just the files you need.  Here is an example:

```
# also available: pg10 and pg9.6
FROM plv8user/plv8:2.3.9-pg11 as plv8

# postgres:11 is an example - in fact, you can also start from ubuntu:18.04 etc and
# apt-get postgres manually. If you do, note that the filepaths below may change.
FROM postgres:11 as base

COPY --from=plv8 /usr/share/postgresql/11/extension/plcoffee* /usr/share/postgresql/11/extension/
COPY --from=plv8 /usr/share/postgresql/11/extension/plls* /usr/share/postgresql/11/extension/
COPY --from=plv8 /usr/share/postgresql/11/extension/plv8* /usr/share/postgresql/11/extension/
COPY --from=plv8 /usr/lib/postgresql/11/lib/plv8*.so /usr/lib/postgresql/11/lib/
RUN chmod 644 /usr/share/postgresql/11/extension/plcoffee* \
    && chmod 644 /usr/share/postgresql/11/extension/plls* \
    && chmod 644 /usr/share/postgresql/11/extension/plv8* \
    && chmod 755 /usr/lib/postgresql/11/lib/plv8*.so
# plv8 is dynamically linked and needs libc++
RUN apt update && apt install libc++1
```

#### pre-built Docker - Alpine)

Sorry! We don't have plv8 compiling (yet) for Alpine Linux. Contributions welcome!  (please share a Dockerfile)

### Building from source

OK, so your database vendor doesn't pre-install plv8 and you can't use a pre-compiled plv8... be aware that plv8 is a slow build with many large dependencies - 4+ cores and broadband are recommended.

#### Building plv8 on Debian/Ubuntu Linux

See [Dockerfile](/Dockerfile) for more details, but roughly:

```
export POSTGRES_MAJOR_VERSION=11   # replace postgresql-server-dev-11 with your version of postgres
export PLV8_VERSION=2.3.9          # replace as needed
sudo apt-get update
sudo apt-get install -y build-essential ca-certificates python curl git pkg-config libc++-dev libc++abi-dev postgresql-server-dev-$POSTGRES_MAJOR_VERSION
curl https://github.com/plv8/plv8/archive/v$PLV8_VERSION.tar.gz -O - | tar zxf -
cd plv8-$PLV8_VERSION
make static   # takes 15+ minutes...
make install
```

#### Building from source (MacOS and other Linux)

### Pre-requisites

Plv8 has some specific requirements:

* git
* g++ or clang++
* Python 2 (for v8)
* pkg-config (linux only for v8)
* libc++-dev (linux only)

See above for the specific apt-get command for Debian/Ubuntu.

### Downloading and Building

To download and build plv8:

```
$ wget https://github.com/plv8/plv8/archive/v2.3.9.tar.gz
$ tar -xvzf v2.3.9.tar.gz
$ cd plv8-2.3.9
$ make
```

note: this downloads the v8 engine and compiles it as well. It is rather large.

note: this only builds for the default version of Postgres that you have installed on your system - see below for how to build against other Postgres installations.



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


#### Building from source (Microsoft Windows)

Building PLV8 for Windows has some specific requirements:

* Git
* MSVC 2013, 2015, or 2017
* CMake - available as part of MSVC
* Postgres 9.5+ (it will work in 9.3 and 9.4, but will involve extra work)

Additional requirements to build V8 (i.e. if you're not using make -f Makefile.shared)

* Python 2
* unzip.exe
* patch.exe - part of the Git install

##### Patching Postgres for Windows

Currently, Postgres-on-Windows requires a patch of one or more `include` files in order to
compile PLV8.

First, find the directory that contains the `include` files.  This will typically
be inside something like `C:\Program Files\PostgreSQL\10\include`, where the `10`
is your version number.  Inside of the `include` directory:

```
PS> cd server\port\atomics
PS> copy \plv8\windows\generic-msvc.h.patch .
PS> patch < generic-msvc.h.patch
```

##### Bootstrapping for Windows

Bootstrapping will the build environment, download, and compile `v8`.  Watch for
any errors:

```
PS> bootstrap.bat
```

##### Configuring for Windows

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

##### Compiling for Windows

After successfully configuring your build environment, compiling should be easy:

```
PS> cmake --build . --config Release --target Package
```

This will build and package the extension for installation.

##### Installing

To install, you simply need to `unzip` the file created.  The name will depend
on the version of PLV8 and the version of Postgres.  An example is
`plv8-2.3.1-postgresql-10-x64.zip`.


#### Optional custom build features

Sometimes, you need to customize the features of plv8 during the build process

##### Building with Execution Timeout

Plv8 allows you to optionally build with an execution timeout for Javascript
functions, when enabled at compile-time.

```
$ make EXECUTION_TIMEOUT=1
```

By default, the execution timeout is not compiled, but when configured it has a
timeout of 300 seconds (5 minutes). You can override this by setting the
`plv8.execution_timeout` variable. It can be set between `1` second and `65536`
seconds, but cannot be disabled.

##### Building with ICU Unicode internationalization support

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

##### Building against a pre-built v8 engine instance

The v8 engine is the largest and trickiest component of plv8 to build, so if you have a pre-built v8 engine library, you can save a lot of time and hassle by compiling plv8 to use this existing v8 instance with this command:

```
$ make -f Makefile.shared
```

##### Building against a specific Postgres installation

If you have multiple versions of PostgreSQL installed, plv8 will only be built for the default version in your PATH, as reported by [pg_config](https://www.google.com/search?q=pg_config). If you need to build Plv8 for other versions you have installed, call make with the PG_CONFIG variable set to the path to your pg_config executable, for example: `make PG_CONFIG=/usr/lib/postgresql/9.5/bin/pg_config`.  This also works for `make -f Makefile.shared` (see below).


