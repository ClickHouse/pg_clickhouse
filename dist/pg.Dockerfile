ARG PG_MAJOR=19
ARG BASE=postgres

FROM $BASE AS build

WORKDIR /work
COPY . .
RUN apt-get update && apt-get install -y --no-install-recommends \
    postgresql-server-dev-$PG_MAJOR \
    libcurl4-openssl-dev \
    uuid-dev \
    make \
    libssl-dev \
    liblz4-dev \
    libzstd-dev \
    libre2-dev \
    libicu-dev \
    pgxnclient \
    unzip \
    g++

RUN make && make install DESTDIR=/dest

# Build the latest stable version of the re2 extension.
RUN cd /tmp && pgxn download re2 && unzip re2-*.zip && rm re2-*.zip && cd re2-* && make && make install DESTDIR=/dest

##############################################################################
# Main image with Postgres, pg_clickhouse, and pg_re2.
FROM $BASE

# Install dependencies
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4t64 uuid libre2-11 liblz4-1 libzstd1 ca-certificates \
    && apt-get clean \
    && rm -rf /var/cache/apt/* /var/lib/apt/lists/*

# Install extension files.
COPY --chmod=644 --from=build \
    /dest/usr/share/postgresql/$PG_MAJOR/extension/*.* \
    /dest/usr/share/postgresql/$PG_MAJOR/extension/*.* \
    /usr/share/postgresql/$PG_MAJOR/extension/

# Install shared libraries.
COPY --chmod=755 --from=build \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/*.so* \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/*.so* \
    /usr/lib/postgresql/$PG_MAJOR/lib/

# Install bitcode files.
COPY --chmod=644 --from=build \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/bitcode/ \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/bitcode/ \
    /usr/lib/postgresql/$PG_MAJOR/lib/bitcode/
