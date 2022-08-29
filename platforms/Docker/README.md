# Docker

While PLV8 does not currently provide docker images, we do have a `Dockerfile`
which is currently able to build PLV8 in top of an official PostgreSQL image.

## PostgreSQL Versions

Currently, the `Dockerfile` only supports building on Debian `bullseye` due to
`v8` build requirements.

You are welcome to use this as a starting point or use it as-is.

The current `Dockerfile` is set up for PostgreSQL 14.5.

## Building

Building `v8` inside of docker requires a lot of RAM allocated per CPU. It is recommended that you have 1.5GB of RAM allocated per CPU core.

If you build, and it fails with a `Segmentation Fault`, it is likely due to lack of RAM available.
