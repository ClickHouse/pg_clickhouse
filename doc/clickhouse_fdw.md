clickhouse_fdw 2.0
==================

## Synopsis

```pgsql
CREATE EXTENSION clickhouse_fdw;
```

## Description

This library contains a single PostgreSQL extension, a [foreign data wrapper]
for [ClickHouse] databases. It supports PostgreSQL 13 and higher and
ClickHouse 22 and higher.



## Usage

### Functions

These functions provide the interface to query a ClickHouse database.

#### `clickhouse_raw_query`

```sql
SELECT clickhouse_raw_query(
    'CREATE TABLE t1 (x String) ENGINE = Memory',
    'host=localhost port=8123'
);
```
```pgsql
 clickhouse_raw_query 
----------------------
 
(1 row)
```

Connect to a ClickHouse service via its HTTP interface, execute a single
query, and disconnect. The optional second argument specifies a connection
string that defaults to `host=localhost port=8123`. The supported connection
parameters are:

*   `host`: The host to connect to; required.
*   `port`: The HTTP port to connect to; defaults to `8123` unless `host` is a
    ClickHouse Cloud host, in which case it defaults to `8443`
*   `username`: The username to connect as; defaults to `default`
*   `password`: The password to use to authenticate; defaults to no password

Useful for queries that return no records, but queries that do return values
will be returned as a single text value:

```sql
SELECT clickhouse_raw_query(
    'SELECT schema_name, schema_owner from information_schema.schemata',
    'host=localhost port=8123'
);
```
```pgsql
      clickhouse_raw_query       
---------------------------------
 INFORMATION_SCHEMA      default+
 default default                +
 git     default                +
 information_schema      default+
 system  default                +
 
(1 row)
```

### Pushdown Functions

These functions are designed to be pushed down to ClickHouse. If they're not,
they will raise an exception.

*   [dictGet](https://clickhouse.com/docs/sql-reference/functions/ext-dict-functions#dictget-dictgetordefault-dictgetornull)

### Pushdown Aggregates

These aggregate functions are designed to be pushed down to ClickHouse. If they're not,
they will raise an exception.

*   [argMax](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/argmax)
*   [argMin](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/argmin)
*   [uniq](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniq)
*   [uniqCombined](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqcombined)
*   [uniqCombined64](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqcombined64)
*   [uniqExact](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqexact)
*   [uniqHLL12](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqhll12)
*   [uniqTheta](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqthetasketch)

## Authors

*   [Ildus Kurbangaliev](https://github.com/ildus)
*   [Ibrar Ahmed](www.linkedin.com/in/ibrarahmed74)
*   [David E. Wheeler](https://justatheory.com/)

## Copyright

*   Portions Copyright (c) 2025 ClickHouse
*   Portions Copyright (c) 2019-2023, Adjust GmbH
*   Portions Copyright (c) 2019 Percona
*   Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group

  [foreign data wrapper]: https://www.postgresql.org/docs/current/fdwhandler.html
    "PostgreSQL Docs: Writing a Foreign Data Wrapper"
  [ClickHouse]: https://clickhouse.com/clickhouse
