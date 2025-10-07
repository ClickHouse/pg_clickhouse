clickhouse_fdw Postgres Extension
=================================

[![PGXN version](https://badge.fury.io/pg/clickhouse_fdw.svg)](https://badge.fury.io/pg/clickhouse_fdw)
[![Build Status](https://github.com/clickhouse/clickhouse_fdw/actions/workflows/ci.yml/badge.svg)](https://github.com/clickhouse/clickhouse_fdw/actions/workflows/ci.yml)

This library contains the PostgreSQL extension `clickhouse_fdw` a foreign data
wrapper for ClickHouse databases.

## Installation

### Compile From Source

#### General Unix

If you have PostgreSQL devel packages and curl devel packages installed, you
should have `pg_config` and `curl-config` on your path, so you should be able
to just run `make` (or `gmake`), then `make install`, then in your database
`CREATE EXTENSION http`.

#### Debian / Ubuntu / APT

See [PostgreSQL Apt] for details on pulling from the PostgreSQL Apt repository.

```sh
sudo apt install \
  postgresql-server-18 \
  libcurl4-openssl-dev \
  uuid-dev \
  make \
  cmake \
  g++
```

#### Compile and Install

To build and install the ClickHouse library and `clickhouse_fdw`, run:

```sh
make
sudo make install
```

By default `make` dynamically links the `clickhouse-cpp` library (except on
macOS, where a dynamic `clickhouse-cpp` library is not yet supported). To
statically compile the ClickHouse library into `clickhouse_fdw`, pass
`CH_BUILD=static`:

```sh
make CH_BUILD=static
sudo make install CH_BUILD=static
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

Once `clickhouse_fdw` is installed, you can add it to a database by connecting
to a database as a super user and running:

``` sql
CREATE EXTENSION clickhouse_fdw;
```

If you want to install `clickhouse_fdw` and all of its supporting objects into
a specific schema, use the `SCHEMA` clause to specify the schema, like so:

``` sql
CREATE SCHEMA env;
CREATE EXTENSION clickhouse_fdw SCHEMA env;
```

## Dependencies

The `clickhouse_fdw` extension requires PostgreSQL 11 or higher, [libcurl],
and [libuuid]. Building the extension requires a compiler, GNU `make`, and
[CMake].

## Copyright and License

Copyright (c) 2025 ClickHouse.

  [`postgresql.conf` parameters]: https://www.postgresql.org/docs/devel/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-OTHER
  [libcurl]: https://curl.se/libcurl/ "libcurl — your network transfer library"
  [libuuid]: https://linux.die.net/man/3/libuuid "libuuid - DCE compatible Universally Unique Identifier library"
  [CMake]: https://cmake.org/ "CMake: A Powerful Software Build System"
  [PostgreSQL Apt]: https://wiki.postgresql.org/wiki/Apt
