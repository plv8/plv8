FROM postgres:14.5-bullseye as builder

RUN set -ex \
  && apt-get update \
  && apt-get install -y build-essential curl python3 ninja-build git wget postgresql-server-dev-14 libtinfo5 pkg-config clang \
  && git clone https://github.com/plv8/plv8 \
  && cd plv8 \
  && git checkout r3.1 \
  && make DOCKER=1 install \
  && strip /usr/lib/postgresql/14/lib/plv8-3.1.4.so


FROM postgres:14.5-bullseye

COPY --from=builder /usr/lib/postgresql/14/lib/plv8* /usr/lib/postgresql/14/lib/
COPY --from=builder /usr/share/postgresql/14/extension/plv8* /usr/share/postgresql/14/extension/
COPY --from=builder /usr/share/postgresql/14/extension/plls* /usr/share/postgresql/14/extension/
COPY --from=builder /usr/share/postgresql/14/extension/plcoffee* /usr/share/postgresql/14/extension/
COPY --from=builder /usr/lib/postgresql/14/lib/bitcode/plv8* /usr/lib/postgresql/14/bitcode/


RUN mkdir -p /var/log/postgres \
  && touch /var/log/postgres/log /var/log/postgres/log.csv \
  && chown -R postgres /var/log/postgres

USER postgres

RUN ln -fs /dev/stderr /var/log/postgres/log