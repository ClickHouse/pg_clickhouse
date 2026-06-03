pg_clickhouse Postgres Extension
================================

[![PGXN]][⚙️] [![Docker]][🐳] [![GitHub]][🐙] [![Postgres]][🐘] [![ClickHouse]][🏠]

This library contains `pg_clickhouse`, a PostgreSQL extension that runs
analytics queries on ClickHouse right from PostgreSQL without rewriting any
SQL. It supports PostgreSQL 13 and later and ClickHouse v23 and later.

## Getting Started

The simplest way to try pg_clickhouse is the [Docker image][🐳], which
contains the standard PostgreSQL Docker image with the pg_clickhouse and [re2]
extensions:

```sh
docker run --name pg_clickhouse -e POSTGRES_PASSWORD=my_pass \
       -d ghcr.io/clickhouse/pg_clickhouse:18
docker exec -it pg_clickhouse psql -U postgres -c 'CREATE EXTENSION pg_clickhouse'
```

See the [tutorial](doc/tutorial.md) to get started importing ClickHouse tables
and pushing down queries.

## Documentation

*   [Reference](doc/pg_clickhouse.md)
*   [Tutorial](doc/tutorial.md)

## Test Case: TPC-H

This table compares [TPC-H] query performance between regular PostgreSQL
tables and pg_clickhouse connected to ClickHouse, both loaded at scaling
factor 1; ✔︎ indicates full pushdown, while a dash indicates a query
cancellation after 1m. All tests run on a MacBook Pro M4 Max with 36 GB of
memory.

<!-- cd dev/tpch && make ch && make pg && make run -->

|    Query   | PostgreSQL | pg_clickhouse | Pushdown |
| ----------:| ----------:| -------------:|:--------:|
|  [Query 1] |    4693 ms |        268 ms |     ✔︎    |
|  [Query 2] |     458 ms |       3446 ms |          |
|  [Query 3] |     742 ms |        111 ms |     ✔︎    |
|  [Query 4] |     270 ms |        130 ms |     ✔︎    |
|  [Query 5] |     337 ms |       1460 ms |     ✔︎    |
|  [Query 6] |     764 ms |         53 ms |     ✔︎    |
|  [Query 7] |     619 ms |         96 ms |     ✔︎    |
|  [Query 8] |     342 ms |        156 ms |     ✔︎    |
|  [Query 9] |    3094 ms |        298 ms |     ✔︎    |
| [Query 10] |     581 ms |        197 ms |     ✔︎    |
| [Query 11] |     212 ms |         24 ms |          |
| [Query 12] |    1116 ms |         84 ms |     ✔︎    |
| [Query 13] |     958 ms |       1368 ms |          |
| [Query 14] |     181 ms |         73 ms |     ✔︎    |
| [Query 15] |    1118 ms |        557 ms |          |
| [Query 16] |     497 ms |       1714 ms |          |
| [Query 17] |    1846 ms |      32709 ms |          |
| [Query 18] |    5823 ms |      10649 ms |          |
| [Query 19] |      53 ms |        206 ms |     ✔︎    |
| [Query 20] |     421 ms |             - |          |
| [Query 21] |    1349 ms |       4434 ms |          |
| [Query 22] |     258 ms |       1415 ms |          |

### Compile From Source

#### General Unix

The PostgreSQL and curl development packages include `pg_config` and
`curl-config` in the path, so you should be able to just run `make` (or
`gmake`), then `make install`, then in your database
`CREATE EXTENSION pg_clickhouse`.

#### Debian / Ubuntu / APT

See [PostgreSQL Apt] for details on pulling from the PostgreSQL Apt repository.

```sh
sudo apt install \
  postgresql-server-18 \
  libcurl4-openssl-dev \
  uuid-dev \
  liblz4-dev \
  libzstd-dev \
  libssl-dev \
  make \
  gcc
```

#### RedHat / CentOS / Yum

```sh
sudo yum install \
  postgresql-server \
  libcurl-devel \
  libuuid-devel \
  lz4-devel \
  libzstd-devel \
  openssl-libs \
  automake \
  gcc
```

See [PostgreSQL Yum] for details on pulling from the PostgreSQL Yum repository.

#### Install From PGXN

With the above dependencies satisfied use the [PGXN client] (available as
[Homebrew], [Apt] and Yum packages named `pgxnclient`) to download, compile,
and install `pg_clickhouse`:


```sh
pgxn install pg_clickhouse
```

#### Compile and Install

To build and install the ClickHouse library and `pg_clickhouse`, run:

```sh
make
sudo make install
```

If your host has several PostgreSQL installations, you might need to specify
the appropriate version of `pg_config`:

```sh
export PG_CONFIG=/usr/lib/postgresql/18/bin/pg_config
make
sudo make install
```

If `curl-config` is not in the path on you host, you can specify the path
explicitly:

```sh
export CURL_CONFIG=/opt/homebrew/opt/curl/bin/curl-config
make
sudo make install
```

If you encounter an error such as:

``` text
"Makefile", line 8: Need an operator
```

You need to use GNU make, which may well be installed on your system as
`gmake`:

``` sh
gmake
gmake install
gmake installcheck
```

If you encounter an error such as:

``` text
make: pg_config: Command not found
```

Be sure that you have `pg_config` installed and in your path. If you used a
package management system such as RPM to install PostgreSQL, be sure that the
`-devel` package is also installed. If necessary tell the build process where
to find it:

``` sh
export PG_CONFIG=/path/to/pg_config
make
sudo make install
```

To install the extension in a custom prefix on PostgreSQL 18 or later, pass
the `prefix` argument to `install` (but no other `make` targets):

```sh
sudo make install prefix=/usr/local/extras
```

Then ensure that the prefix is included in the following [`postgresql.conf`
parameters]:

```ini
extension_control_path = '/usr/local/extras/postgresql/share:$system'
dynamic_library_path   = '/usr/local/extras/postgresql/lib:$libdir'
```

#### Testing

To run the test suite, once the extension has been installed, run

```sh
make installcheck
```

If you encounter an error such as:

``` text
ERROR:  must be owner of database regression
```

You need to run the test suite using a super user, such as the default
"postgres" super user:

``` sh
make installcheck PGUSER=postgres
```

### Loading

Once `pg_clickhouse` is installed, you can add it to a database by connecting
as a super user and running:

``` sql
CREATE EXTENSION pg_clickhouse;
```

If you want to install `pg_clickhouse` and all of its supporting objects into
a specific schema, use the `SCHEMA` clause to specify the schema, like so:

``` sql
CREATE SCHEMA env;
CREATE EXTENSION pg_clickhouse SCHEMA env;
```

## Dependencies

The `pg_clickhouse` extension requires [PostgreSQL] 13 or higher, [libcurl],
[libuuid], [liblz4], and [libzstd]. Building the extension requires a C
compiler, [libSSL], and [GNU make].

## Road Map

Our top focus is finishing pushdown coverage for analytic workloads before
adding DML features. Our road map:

*   Get the remaining 10 un-pushed-down TPC-H queries optimally planned
*   Test and fix pushdown for the ClickBench queries
*   Support transparent pushdown of all PostgreSQL aggregate functions
*   Support transparent pushdown of all PostgreSQL functions
*   Allow server-level and session-level ClickHouse settings via CREATE SERVER
    and GUCs
*   Support all ClickHouse data types
*   Support lightweight DELETEs and UPDATEs
*   Support batch insertion via COPY
*   Add a function to execute an arbitrary ClickHouse query and return its
    results as a tables
*   Add support for pushdown of UNION queries when they all query the remote
    database

## Authors

*   [David E. Wheeler](https://justatheory.com/)
*   [serprex](https://github.com/serprex)
*   [Ildus Kurbangaliev](https://github.com/ildus)
*   [Ibrar Ahmed](https://github.com/ibrarahmad)

## Copyright

*   Copyright (c) 2025-2026, ClickHouse
*   Portions Copyright (c) 2023-2025, Ildus Kurbangaliev
*   Portions Copyright (c) 2019-2023, Adjust GmbH
*   Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group

  [PGXN]:       https://badge.fury.io/pg/pg_clickhouse.svg
  [⚙️]:         https://pgxn.org/dist/pg_clickhouse "Latest version on PGXN"
  [Postgres]:   https://github.com/clickhouse/pg_clickhouse/actions/workflows/postgres.yml/badge.svg
  [🐘]:         https://github.com/clickhouse/pg_clickhouse/actions/workflows/postgres.yml "Tested with PostgreSQL 13-18"
  [ClickHouse]: https://github.com/clickhouse/pg_clickhouse/actions/workflows/clickhouse.yml/badge.svg
  [🏠]:          https://github.com/clickhouse/pg_clickhouse/actions/workflows/clickhouse.yml "Tested with ClickHouse v23–26"
  [Docker]:     https://img.shields.io/github/v/release/ClickHouse/pg_clickhouse?label=%F0%9F%90%B3%20Docker&color=44cc11
  [🐳]:          https://github.com/ClickHouse/pg_clickhouse/pkgs/container/pg_clickhouse "Latest Docker release"
  [GitHub]:     https://img.shields.io/github/v/release/ClickHouse/pg_clickhouse?label=%F0%9F%90%99%20GitHub&color=44cc11
  [🐙]:          https://github.com/ClickHouse/pg_clickhouse/releases "Latest release on GitHub"

  [PostgreSQL Apt]: https://wiki.postgresql.org/wiki/Apt
  [PostgreSQL Yum]: https://yum.postgresql.org
  [PGXN client]: https://pgxn.github.io/pgxnclient/ "PGXN Client’s documentation"
  [Homebrew]: https://formulae.brew.sh/formula/pgxnclient#default "PGXN client on Homebrew"
  [Apt]: https://tracker.debian.org/pkg/pgxnclient "PGXN client on Debian Apt"
  [`postgresql.conf` parameters]: https://www.postgresql.org/docs/devel/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-OTHER
  [PostgreSQL]: https://www.postgresql.org "PostgreSQL: The World's Most Advanced Open Source Relational Database"
  [libcurl]: https://curl.se/libcurl/ "libcurl — your network transfer library"
  [libuuid]: https://linux.die.net/man/3/libuuid "libuuid - DCE compatible Universally Unique Identifier library"
  [liblz4]: https://lz4.org "LZ4 - Extremely fast compression"
  [libzstd]: https://facebook.github.io/zstd/ "Zstandard - Fast real-time compression algorithm"
  [GNU make]: https://www.gnu.org/software/make "GNU Make"
  [LibSSL]: https://openssl-library.org "OpenSSL Library"
  [TPC-H]: https://www.tpc.org/tpch/
  [re2]: https://github.com/ClickHouse/pg_re2 "pg_re2: ClickHouse-compatible regex functions using RE2"

  [Query 1]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/1.sql
  [Query 2]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/2.sql
  [Query 3]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/3.sql
  [Query 4]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/4.sql
  [Query 5]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/5.sql
  [Query 6]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/6.sql
  [Query 7]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/7.sql
  [Query 8]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/8.sql
  [Query 9]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/9.sql
  [Query 10]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/10.sql
  [Query 11]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/11.sql
  [Query 12]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/12.sql
  [Query 13]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/13.sql
  [Query 14]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/14.sql
  [Query 15]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/15.sql
  [Query 16]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/16.sql
  [Query 17]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/17.sql
  [Query 18]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/18.sql
  [Query 19]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/19.sql
  [Query 20]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/20.sql
  [Query 21]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/21.sql
  [Query 22]: https://github.com/ClickHouse/pg_clickhouse/blob/main/dev/tpch/queries/22.sql
