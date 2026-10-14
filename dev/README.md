# pg_clickhouse Development Tools

This directory contains various scripts and configurations to assist with the
development of pg_clickhouse.

*   `bear.json`: [Bear Configuration](#bear-configuration)
*   `bear.yml`: [Bear Configuration](#bear-configuration)
*   `choci`: [Run Docker ClickHouse Server](#run-docker-clickhouse-server)
*   `configs`: Configuration files for `choci` and `runch`
*   `README.md`: This file
*   `runch`: [Run Local ClickHouse Server](#run-local-clickhouse-server)
*   [`tpch`]: [TPC-H Benchmark Scripts](./tpch/README.md)
*   `type_table.awk`: [Data Type Table](#data-type-table)

## Run Local ClickHouse Server

Use `runch` to locally run a specific version of the ClickHouse server. For a
complete list of commands, run:

```sh
runch help
```

`runch` relies on [clickhousectl] to start or stop a local ClickHouse server
on the default ports (assuming [clickhousectl] is not currently running any
other servers). Useful for testing against a specific version of ClickHouse.
For example, to test pg_clickhouse against ClickHouse 23.8:

```sh
./dev/runch start 23.8
make installcheck
./dev/runch stop
```

Specify any version supported by [clickhousectl]. Currently defaults to
`26.6`.

## Run Docker ClickHouse Server

Use `choci` to run a specific version of ClickHouse in an OCI (Docker)
container. For a complete list of commands, run:

```sh
./dev/choci help
```

`choci` relies on [docker] to start or stop a ClickHouse server with the
default ports mapped to localhost. Useful for testing against a specific
version of ClickHouse. For example, to test pg_clickhouse against ClickHouse
23.8:

```sh
./dev/choci start 23.8
make installcheck
./dev/choci stop
```

Specify any tag supported by the [Docker image]. Defaults to `latest`.

For ClickHouse 26.3+, `choci start` also seeds an Iceberg warehouse and starts
a Python container serving its REST catalog on port 8181 inside ClickHouse's
network namespace. It waits for both services to accept requests. `choci stop`
removes both containers and their warehouse.

Build and [install](../README.md#compile-from-source) this checkout, then use a
running PostgreSQL instance you can connect to as a superuser. From repo root,
run catalog import tests with:

```sh
./dev/choci start 26.3
make installcheck TESTS=test/sql/import_lake.sql
./dev/choci stop
```

`runch start` does the same for a native server, seeding the warehouse under
its `user_files` directory and serving the catalog with host Python 3.10+.
`runch stop` stops the catalog:

```sh
./dev/runch start 26.3
make installcheck TESTS=test/sql/import_lake.sql
./dev/runch stop
```

## Data Type Table

Run `make type-table` to regenerate the data type table in
[doc/pg_clickhouse.md](../doc/pg_clickhouse.md):

```sh
make type-table
```

## Bear Configuration

Configuration files for [Bear], used to generate the `compile_commands.json`
file:

```sh
make compile_commands.json
```

This file is used by the `clang-tidy` and `lint` targets to analyze the
pg_clickhouse source code to report issues. The pg_clickhouse project converts
enables all warnings and converts them to errors, but this configuration gives
`clang-tidy` indigestion. We use the Bear configuration to disable all errors
to avoid this issue.

There are currently two files:

*   `bear.yml` works with Bear 4 and later, the current release
*   `bear.json` works with Bear 3, which ships with Debian

The `compile_commands.json` target determines which to use.

  [clickhousectl]: https://clickhouse.com/docs/interfaces/cli "ClickHouse Docs: clickhousectl"
  [Docker image]: https://hub.docker.com/r/clickhouse/clickhouse-server
    "clickhouse/clickhouse-server on DockerHub"
  [Bear]: https://github.com/rizsotto/Bear "Bear generates a compilation database for Clang tooling"
