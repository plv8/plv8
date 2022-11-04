FROM postgres:14.5-bullseye as builder

ENV PLV8_VERSION=3.1.4
ENV PLV8_BRANCH=r3.1
RUN set -ex \
  && apt-get update \
  && apt-get install -y build-essential curl python3 ninja-build git wget postgresql-server-dev-${PG_MAJOR} libtinfo5 pkg-config clang binutils \
  && git clone https://github.com/plv8/plv8 \
  && cd plv8 \
  && git checkout ${PLV8_BRANCH} \
  && make DOCKER=1 generate_upgrades \
  && make DOCKER=1 install \
  && strip /usr/lib/postgresql/${PG_MAJOR}/lib/plv8-${PLV8_VERSION}.so


FROM postgres:14.5-bullseye

ENV PLV8_VERSION=3.1.4
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/plv8* /usr/lib/postgresql/${PG_MAJOR}/lib/
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/bitcode/plv8-${PLV8_VERSION}.index.bc /usr/lib/postgresql/${PG_MAJOR}/lib/bitcode/
COPY --from=builder /usr/lib/postgresql/${PG_MAJOR}/lib/bitcode/plv8-${PLV8_VERSION}/* /usr/lib/postgresql/${PG_MAJOR}/lib/bitcode/plv8-${PLV8_VERSION}/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/plv8* /usr/share/postgresql/${PG_MAJOR}/extension/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/plls* /usr/share/postgresql/${PG_MAJOR}/extension/
COPY --from=builder /usr/share/postgresql/${PG_MAJOR}/extension/plcoffee* /usr/share/postgresql/${PG_MAJOR}/extension/


RUN mkdir -p /var/log/postgres \
  && touch /var/log/postgres/log /var/log/postgres/log.csv \
  && chown -R postgres /var/log/postgres

USER postgres

RUN ln -fs /dev/stderr /var/log/postgres/log
