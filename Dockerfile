ARG PG_MAJOR=18

FROM postgres:$PG_MAJOR-trixie AS build

WORKDIR /work
COPY . .
RUN apt-get update && apt-get install -y --no-install-recommends \
    postgresql-server-dev-$PG_MAJOR \
    libcurl4-openssl-dev \
    uuid-dev \
    make \
    cmake \
    libssl-dev \
    g++

RUN make && make install DESTDIR=/dest

FROM postgres:$PG_MAJOR-trixie

# Install dependencies
RUN apt-get update && apt-get install -y --no-install-recommends libcurl4t64 uuid \
    && apt-get clean \
    && rm -rf /var/cache/apt/* /var/lib/apt/lists/*

# Install extension files.
COPY --chmod=644 --from=build \
    /dest/usr/share/postgresql/$PG_MAJOR/extension/*.* \
    /usr/share/postgresql/$PG_MAJOR/extension/

# Install shared libraries.
COPY --chmod=755 --from=build \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/*.so* \
    /usr/lib/postgresql/$PG_MAJOR/lib/

# Install bitcode files.
COPY --chmod=644 --from=build \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/bitcode/*.bc \
    /usr/lib/postgresql/$PG_MAJOR/lib/bitcode/
COPY --chmod=644 --from=build \
    /dest/usr/lib/postgresql/$PG_MAJOR/lib/bitcode/clickhouse_fdw/src/*.bc \
    /usr/lib/postgresql/$PG_MAJOR/lib/bitcode/clickhouse_fdw/src/
