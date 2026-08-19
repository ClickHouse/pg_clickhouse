# pg_clickhouse Development Tools

This directory contains various scripts and configurations to assist with the
development of pg_clickhouse.

*   `bear.json`: [Bear Configuration](#bear-configuration)
*   `bear.yml`: [Bear Configuration](#bear-configuration)
*   `choci`: [Run ClickHouse Server](#run-local-clickhouse-server)
*   `configs`: Configuration files for `choci` and `runch`
*   `docker-compose.yml`: [ClickHouse Version Testing Containers](#clickhouse-version-testing-containers)
*   `Makefile`: Automates [ClickHouse Version Testing Containers](#clickhouse-version-testing-containers)
*   `README.md`: This file
*   `runch`: [Run ClickHouse Server](#run-clickhouse-server)
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
choci help
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

## ClickHouse Version Testing Containers

This directory's [docker-compose.yml](docker-compose.yml) starts images for
each supported major version of ClickHouse. Each container also runs
PostgreSQL 19. Both ClickHouse and PostgreSQL run on their default ports
(unexported) and trust all local connections. The containers are based on
[pgxn-tools].

Example usage:

```sh
make start
docker logs -f ch_26_3
# Wait a few minutes for the services to start (log output will stop)
docker exec -it ch_26_3 bash
pg-build-test # or make && make install && make installcheck
```

The containers should remain until you `make stop`; if you restart Docker (or
your system), they may need to be started again, e.g., `docker start ch_26_3`.

To run a single version, use:

```sh
(cd dev && docker compose -f docker-compose.yml up -d ch_24_3)
```

Run `(cd dev && docker compose config --services)` to get a list of service
versions.

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
  [pgxn-tools]: https://github.com/pgxn/docker-pgxn-tools/
  [Bear]: https://github.com/rizsotto/Bear "Bear generates a compilation database for Clang tooling"
