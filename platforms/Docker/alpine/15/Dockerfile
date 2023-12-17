ARG PG_CONTAINER_VERSION=15
FROM docker.io/library/postgres:${PG_CONTAINER_VERSION}-alpine as builder

RUN set -ex \
  && apk --no-cache add git python3 build-base linux-headers clang15 llvm15 llvm16 cmake make pkgconfig postgresql-dev
ARG PLV8_BRANCH=r3.2
ENV PLV8_BRANCH=${PLV8_BRANCH}
ARG PLV8_VERSION=3.2.1
ENV PLV8_VERSION=${PLV8_VERSION}
RUN set -ex \
  && git clone --branch ${PLV8_BRANCH} --single-branch --depth 1 https://github.com/plv8/plv8 \
  && cd plv8 \
  && make \
  && strip plv8-${PLV8_VERSION}.so \
  && make install


FROM docker.io/library/postgres:${PG_CONTAINER_VERSION}-alpine

ARG PLV8_VERSION=3.2.1
ENV PLV8_VERSION=${PLV8_VERSION}
COPY --from=builder /usr/local/lib/postgresql/plv8* /usr/local/lib/postgresql/
COPY --from=builder /usr/local/lib/postgresql/bitcode/plv8-${PLV8_VERSION}/* /usr/local/lib/postgresql/bitcode/plv8-${PLV8_VERSION}/
COPY --from=builder /usr/local/share/postgresql/extension/plv8* /usr/local/share/postgresql/extension/


RUN mkdir -p /var/log/postgres \
  && touch /var/log/postgres/log /var/log/postgres/log.csv \
  && chown -R postgres /var/log/postgres

USER postgres

RUN ln -fs /dev/stderr /var/log/postgres/log
