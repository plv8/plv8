FROM ubuntu:14.04
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update -y && \
    apt-get dist-upgrade -y
RUN apt-get install -y \
        git curl build-essential

# Clone Postgres Source
RUN git clone https://github.com/postgres/postgres.git /opt/postgres && \
    cd /opt/postgres && \
    git fetch

# Checkout desired Postgres version
RUN cd /opt/postgres && \
    git checkout -b REL9_4_5 REL9_4_5

# Build and Install Postgres
RUN apt-get install -y \
        libreadline-dev \
        zlib1g-dev \
        bison \
        flex \
        && \
    cd /opt/postgres && \
    ./configure && \
    make && \
    make install

ENV PATH "/usr/local/pgsql/bin:$PATH"

# Initialize Postgres
RUN adduser --disabled-password --gecos "" postgres && \
    mkdir /usr/local/pgsql/data && \
    chown postgres /usr/local/pgsql/data && \
    sudo -u postgres /usr/local/pgsql/bin/initdb -D /usr/local/pgsql/data

# Add and build plv8
COPY . /opt/plv8
RUN cd /opt/plv8 && \
    apt-get install -y \
        python \
        && \
    bash -c 'make clean && make static && make install'

# Prepare test
WORKDIR /opt/plv8
RUN echo 'export PATH="/usr/local/pgsql/bin:$PATH"' >> test.sh && \
    echo "/usr/local/pgsql/bin/postgres -D /usr/local/pgsql/data >logfile 2>&1 &" >> test.sh && \
    echo "sleep 1" >> test.sh && \
    echo "make installcheck" >> test.sh && \
    chown -R postgres /opt/plv8

CMD [ "sudo", "-u", "postgres", "bash", "test.sh" ]

