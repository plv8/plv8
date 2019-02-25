# use docker build --build-arg PG_VERSION=xxx to support other versions of postgres,
# e.g. older pg versions, different OS distros, etc.
ARG PG_VERSION=11
FROM postgres:$PG_VERSION

# use docker build --build-arg PLV8_VERSION=xxx to support older versions of plv8
ARG PLV8_VERSION=2.3.9

RUN buildDependencies="build-essential \
    ca-certificates \
    python \
    curl \
    git-core \
    pkg-config \
    libc++-dev \
    libc++abi-dev \
    postgresql-server-dev-$PG_MAJOR" \
  && apt-get update \
  && apt-get install -y --no-install-recommends ${buildDependencies} \
  && mkdir -p /tmp/build \
  && echo "https://github.com/plv8/plv8/archive/v$PLV8_VERSION.tar.gz" \
  && curl -o /tmp/build/plv8-$PLV8_VERSION.tar.gz -SL "https://github.com/plv8/plv8/archive/v$PLV8_VERSION.tar.gz" \
  && cd /tmp/build \
  && tar -xzf /tmp/build/plv8-$PLV8_VERSION.tar.gz -C /tmp/build/ \
  && cd /tmp/build/plv8-$PLV8_VERSION \
  && make static \
  && make install \
  && strip /usr/lib/postgresql/${PG_MAJOR}/lib/plv8-$PLV8_VERSION.so \
  && cd / \
  && apt-get clean \
  && apt-get remove -y  ${buildDependencies} \
  && apt-get autoremove -y \
  && rm -rf /tmp/build /var/lib/apt/lists/*

# note: based on an older Dockerfile from https://github.com/shady77/plv8-docker/
LABEL plv8_version $PLV8_VERSION
LABEL postgres_version $PG_VERSION
LABEL postgres_major_version ${PG_MAJOR}
