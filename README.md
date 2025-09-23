clickhouse_fdw Postgres Extension
=================================

[![PGXN version](https://badge.fury.io/pg/clickhouse_fdw.svg)](https://badge.fury.io/pg/clickhouse_fdw)
[![Build Status](https://github.com/clickhouse/clickhouse_fdw/actions/workflows/ci.yml/badge.svg)](https://github.com/clickhouse/clickhouse_fdw/actions/workflows/ci.yml)

This library contains the PostgreSQL extension `clickhouse_fdw` a foreign data
wrapper for ClickHouse databases.

To build `clickhouse_fdw`, just do this:

``` sh
make
make installcheck
make install
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
env PG_CONFIG=/path/to/pg_config make && make installcheck && make install
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

To install the extension in a custom prefix on PostgreSQL 18 or later, pass
the `prefix` argument to `install` (but no other `make` targets):

```sh
make install prefix=/usr/local/extras
```

Then ensure that the prefix is included in the following [`postgresql.conf`
parameters]:

```ini
extension_control_path = '/usr/local/extras/postgresql/share:$system'
dynamic_library_path   = '/usr/local/extras/postgresql/lib:$libdir'
```

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

Dependencies
-----------

The `clickhouse_fdw` extension requires PostgreSQL 11 or higher and [libcurl]. Building the
extension requires a compiler, GNU `make`, and [CMake].

Copyright and License
---------------------

Copyright (c) 2025 ClickHouse.

  [`postgresql.conf` parameters]: https://www.postgresql.org/docs/devel/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-OTHER
  [libcurl]: https://curl.se/libcurl/ "libcurl — your network transfer library"
  [CMake]: https://cmake.org/ "CMake: A Powerful Software Build System"
