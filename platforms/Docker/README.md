# Docker

While PLV8 does not currently provide docker images, we do have a `Dockerfile` which is currently able to build PLV8 in top of an official PostgreSQL image.

## PostgreSQL Versions

There are `Dockerfile`s for Debian `bullseys` and `bookworm`, along with `alpine`.

## Building

```shell
docker build -t plv8:latest .
```

Note that you can pass `PG_CONTAINER_VERSION`, `PLV8_VERSION`, and `PLV8_BRANCH` as arguments to the `docker build`.
