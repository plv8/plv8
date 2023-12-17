ARG PG_CONTAINER_VERSION=14.5
FROM docker.io/library/postgres:${PG_CONTAINER_VERSION}-bullseye as builder

ARG DEBIAN_FRONTEND=noninteractive
RUN set -ex \
  && apt-get update \
  && apt-get install -y build-essential git postgresql-server-dev-${PG_MAJOR} libtinfo5 pkg-config clang binutils libstdc++-10-dev cmake \
  && apt-get clean
ARG PLV8_BRANCH=r3.2
ENV PLV8_BRANCH=${PLV8_BRANCH}
ARG PLV8_VERSION=3.2.1
ENV PLV8_VERSION=${PLV8_VERSION}
RUN set -ex \
  && git clone --branch ${PLV8_BRANCH} --single-branch --depth 1 https://github.com/plv8/plv8 \
  && cd plv8 \
  && make install \
  && strip /usr/lib/postgresql/${PG_MAJOR}/lib/plv8-${PLV8_VERSION}.so


FROM docker.io/library/postgres:${PG_CONTAINER_VERSION}-bullseye

ARG PLV8_VERSION=3.2.1
ENV PLV8_VERSION=${PLV8_VERSION}
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/plv8* /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/bitcode/plv8-${PLV8_VERSION}/* /usr/lib/postgresql/${PG_MAJOR}/lib/bitcode/plv8-${PLV8_VERSION}/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/plv8* /usr/share/postgresql/${PG_MAJOR}/extension/


RUN mkdir -p /var/log/postgres \
  && touch /var/log/postgres/log /var/log/postgres/log.csv \
  && chown -R postgres /var/log/postgres

USER postgres

RUN ln -fs /dev/stderr /var/log/postgres/log
