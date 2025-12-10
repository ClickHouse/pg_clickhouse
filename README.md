pg_clickhouse Postgres Extension
================================

[![PGXN]][⚙️] [![Postgres]][🐘] [![ClickHouse]][🏠] [![Docker]][🐳]

This library contains the PostgreSQL extension `pg_clickhouse`, including a
foreign data wrapper for ClickHouse databases. It supports ClickHouse v23 and
later.

## Getting Started

The simplest way to try pg_clickhouse is the [Docker image][🐳], which
contains the standard PostgreSQL Docker image with the pg_clickhouse
extension:

```sh
docker run --name pg_clickhouse -e POSTGRES_PASSWORD=my_pass \
       -d ghcr.io/clickhouse/pg_clickhouse:18
docker exec -it pg_clickhouse psql -U postgres -c 'CREATE EXTENSION pg_clickhouse';
```

See the [tutorial](doc/tutorial.md) to get started importing ClickHouse tables
and pushing down queries.

## Documentation

*   [Reference](doc/pg_clickhouse.md)
*   [Tutorial](doc/tutorial.md)

## Test Case: TPC-H

This table compares [TPC-H] query performance between regular PostgreSQL
tables and pg_clickhouse connected to ClickHouse, both loaded at scaling
factor 1; ✅ indicates full pushdown, while a dash indicates a query
cancellation after 1m. All tests run on a MacBook Pro M4 Max with 36 GB of
memory.

| Query              | Pushdown | pg_clickhouse | PostgreSQL |
| -----------------: | :------: | ------------: | ---------: |
|  [Query 1](#r2q1)  |     ✅    |         73ms  |     4478ms |
|  [Query 2](#r2q2)  |          |             - |      560ms |
|  [Query 3](#r2q3)  |     ✅    |          74ms |     1454ms |
|  [Query 4](#r2q4)  |     ✅    |          67ms |      650ms |
|  [Query 5](#r2q5)  |     ✅    |         104ms |      452ms |
|  [Query 6](#r2q6)  |     ✅    |          42ms |      740ms |
|  [Query 7](#r2q7)  |     ✅    |          83ms |      633ms |
|  [Query 8](#r2q8)  |     ✅    |         114ms |      320ms |
|  [Query 9](#r2q9)  |     ✅    |         136ms |     3028ms |
| [Query 10](#r2q10) |     ✅    |          10ms |        6ms |
| [Query 11](#r2q11) |     ✅    |          78ms |      213ms |
| [Query 12](#r2q12) |     ✅    |          37ms |     1101ms |
| [Query 13](#r2q13) |          |        1242ms |      967ms |
| [Query 14](#r2q14) |     ✅    |          51ms |      193ms |
| [Query 15](#r2q15) |          |         522ms |     1095ms |
| [Query 16](#r2q16) |          |        1797ms |      492ms |
| [Query 17](#r2q17) |          |           9ms |     1802ms |
| [Query 18](#r2q18) |          |          10ms |     6185ms |
| [Query 19](#r2q19) |          |         532ms |       64ms |
| [Query 20](#r2q20) |          |        4595ms |      473ms |
| [Query 21](#r2q21) |          |        1702ms |     1334ms |
| [Query 22](#r2q22) |          |         268ms |      257ms |

### Compile From Source

#### General Unix

The PostgreSQL and curl development packages include `pg_config` and
`curl-config` in the path, so you should be able to just run `make` (or
`gmake`), then `make install`, then in your database `CREATE EXTENSION http`.

#### Debian / Ubuntu / APT

See [PostgreSQL Apt] for details on pulling from the PostgreSQL Apt repository.

```sh
sudo apt install \
  postgresql-server-18 \
  libcurl4-openssl-dev \
  uuid-dev \
  libssl-dev \
  make \
  cmake \
  g++
```

#### RedHat / CentOS / Yum

```sh
sudo yum install \
  postgresql-server \
  libcurl-devel \
  libuuid-devel \
  openssl-libs \
  automake \
  cmake \
  gcc
```

See [PostgreSQL Yum] for details on pulling from the PostgreSQL Yum repository.

#### Compile and Install

To build and install the ClickHouse library and `pg_clickhouse`, run:

```sh
make
sudo make install
```

<!-- XXX DSO currently disabled.
By default `make` dynamically links the `clickhouse-cpp` library (except on
macOS, where a dynamic `clickhouse-cpp` library is not yet supported). To
statically compile the ClickHouse library into `pg_clickhouse`, pass
`CH_BUILD=static`:

```sh
make CH_BUILD=static
sudo make install CH_BUILD=static
```
 -->

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

```
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

```
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

```
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
[libuuid]. Building the extension requires a C and C++ compiler, [libSSL], [GNU
make], and [CMake].

## Road Map

Our top focus is finishing pushdown coverage for analytic workloads before
adding DML features. Our road map:

*   Get the remaining 10 un-pushed-down TPC-H queries optimally planned
*   Test and fix pushdown for the ClickBench queries
*   Support transparent pushdown of all PostgreSQL aggregate functions
*   Support transparent pushdown of all PostgreSQL functions
*   Allow server-level and session-level ClickHouse settings via CREATE SERVER and GUCs
*   Support all ClickHouse data types
*   Support lightweight DELETEs and UPDATEs
*   Support batch insertion via COPY
*   Add a function to execute an arbitrary ClickHouse query and return its results as a tables
*   Add support for pushdown of UNION queries when they all query the remote
*   database


## Authors

*   [David E. Wheeler](https://justatheory.com/)
*   [Ildus Kurbangaliev](https://github.com/ildus)
*   [Ibrar Ahmed](https://github.com/ibrarahmad)

## Copyright

*   Copyright (c) 2025, ClickHouse
*   Portions Copyright (c) 2023-2025, Ildus Kurbangaliev
*   Portions Copyright (c) 2019-2023, Adjust GmbH
*   Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group

  [PGXN]: https://badge.fury.io/pg/pg_clickhouse.svg
  [⚙️]: https://pgxn.org/dist/pg_clickhouse "Latest version on PGXN"
  [Postgres]:  https://github.com/clickhouse/pg_clickhouse/actions/workflows/postgres.yml/badge.svg
  [🐘]:        https://github.com/clickhouse/pg_clickhouse/actions/workflows/postgres.yml "Tested with PostgreSQL 13-18"
  [ClickHouse]: https://github.com/clickhouse/pg_clickhouse/actions/workflows/clickhouse.yml/badge.svg
  [🏠]:          https://github.com/clickhouse/pg_clickhouse/actions/workflows/clickhouse.yml "Tested with ClickHouse v23–25"
  [Docker]:    https://ghcr-badge.egpl.dev/clickhouse/pg_clickhouse/latest_tag?color=%2344cc11&ignore=latest&label=Docker
  [🐳]:        https://github.com/ClickHouse/pg_clickhouse/pkgs/container/pg_clickhouse "Latest version on Docker Hub"

  [PostgreSQL Apt]: https://wiki.postgresql.org/wiki/Apt
  [PostgreSQL Yum]: https://yum.postgresql.org
  [`postgresql.conf` parameters]: https://www.postgresql.org/docs/devel/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-OTHER
  [PostgreSQL]: https://www.postgresql.org "PostgreSQL: The World's Most Advanced Open Source Relational Database"
  [libcurl]: https://curl.se/libcurl/ "libcurl — your network transfer library"
  [libuuid]: https://linux.die.net/man/3/libuuid "libuuid - DCE compatible Universally Unique Identifier library"
  [GNU make]: https://www.gnu.org/software/make "GNU Make"
  [CMake]: https://cmake.org/ "CMake: A Powerful Software Build System"
  [LibSSL]: https://openssl-library.org "OpenSSL Library"
  [TPC-H]: https://www.tpc.org/tpch/
