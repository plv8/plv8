# Contributed files

## ICU

Data files for ICU's i18n support when compiling with `make`.  Copy one
of the files, depending on your platform, and set the `plv8.icu_data` variable
in `postgresql.conf`.

* icudtl.dat - Little Endian architectures (Intel)
* icudtb.dat - Big Endian architectures (Sparc)

For ARM, you will need to figure out which Endianess your hardware and OS is
configured for.

NOTE: it is important that the user that Postgres is started with has read
access to the file.

## Web

Node.js scripts to create and maintain the PLV8 web site.
