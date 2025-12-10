pg_clickhouse 0.1.0
===================

## Synopsis

```sql
CREATE EXTENSION pg_clickhouse;
```

## Description

This library contains PostgreSQL extension that enables remote query execution
on ClickHouse databases, including a [foreign data wrapper]. It supports
PostgreSQL 13 and higher and ClickHouse 23 and higher.

## Getting Started

The simplest way to try pg_clickhouse is the [Docker image], which contains
the standard PostgreSQL Docker image with the pg_clickhouse extension:

```sh
docker run --name pg_clickhouse -e POSTGRES_PASSWORD=my_pass \
       -d ghcr.io/clickhouse/pg_clickhouse:18
docker exec -it pg_clickhouse psql -U postgres
```

See the [tutorial](tutorial.md) to get started importing ClickHouse tables and
pushing down queries.

## Usage

```sql
CREATE EXTENSION pg_clickhouse;
CREATE SERVER taxi_srv FOREIGN DATA WRAPPER clickhouse_fdw
       OPTIONS(driver 'binary', host 'localhost', dbname 'taxi');
CREATE USER MAPPING FOR CURRENT_USER SERVER taxi_srv
       OPTIONS (user 'default');
CREATE SCHEMA taxi;
IMPORT FOREIGN SCHEMA taxi FROM SERVER taxi_srv INTO taxi;
```

## Documentation

*   [Installation](../README.md)
*   [Tutorial](doc/tutorial.md)

## Versioning Policy

pg_clickhouse adheres to [Semantic Versioning] for its public releases.

*   The major version increments for API changes
*   The minor version increments for backward compatible SQL changes
*   The patch version increments for binary-only changes

Once installed, PostgreSQL tracks two variations of the version:

*   The library version (defined by `PG_MODULE_MAGIC` on PostgreSQL 18 and
    higher) includes the full semantic version, visible in the output of the
    `pg_get_loaded_modules()` function.
*   The extension version (defined in the control file) includes only the
    major and minor versions, visible in the `pg_catalog.pg_extension` table,
    the output of the `pg_available_extension_versions()` function, and `\dx
    pg_clickhouse`.

In practice this means that a release that increments the patch version, e.g.
from `v0.1.0` to `v0.1.1`, benefits all databases that have loaded `v0.1` and
do not need to run `ALTER EXTENSION` to benefit from the upgrade.

A release that increments the minor or major versions, on the other hand, will
be accompanied by SQL upgrade scrips, and all existing database that contain
the extension must run `ALTER EXTENSION pg_clickhouse UPDATE` to benefit from
the upgrade.

## SQL Reference

The following SQL expressions use pg_clickhouse.

### CREATE EXTENSION

Use [CREATE EXTENSION] to add pg_clickhouse to a database:

```sql
CREATE EXTENSION pg_clickhouse;
```

Use `WITH SCHEMA` to install it into a specific schema (recommended):

```sql
CREATE SCHEMA ch;
CREATE EXTENSION pg_clickhouse WITH SCHEMA ch;
```

### ALTER EXTENSION

Use [ALTER EXTENSION] to change pg_clickhouse. Examples:

*   After installing a new release of pg_clickhouse, use the `UPDATE` clause:

    ```sql
    ALTER EXTENSION pg_clickhouse UPDATE;
    ```

*   Use `SET SCHEMA` to move the extension to a new schema:

    ```sql
    CREATE SCHEMA ch;
    ALTER EXTENSION pg_clickhouse SET SCHEMA ch;
    ```

### DROP EXTENSION

Use [DROP EXTENSION] to remove pg_clickhouse from a database:

```sql
DROP EXTENSION pg_clickhouse;
```

This command fails if there are any objects that depend on pg_clickhouse. Use
the `CASCADE` clause to drop them, too:

```sql
DROP EXTENSION pg_clickhouse CASCADE;
```

### CREATE SERVER

Use [CREATE SERVER] to create a foreign server that connects to a ClickHouse
server. Example:

```sql
CREATE SERVER taxi_srv FOREIGN DATA WRAPPER clickhouse_fdw
       OPTIONS(driver 'binary', host 'localhost', dbname 'taxi');
```

The supported options are:

*   `driver`: The ClickHouse connection driver to use, either "binary" or
    "http". **Required.**
*   `dbname`: The ClickHouse database to use upon connecting. Defaults to
    "default".
*   `host`: The host name of the ClickHouse server. Defaults to "localhost";
*   `port`: The port to connect to on the ClickHouse server. Defaults as
    follows:
    *   9440 if `driver` is "binary" and `host` is a ClickHouse Cloud host
    *   9004 if `driver` is "binary" and `host` is not a ClickHouse Cloud host
    *   8443 if `driver` is "http" and `host` is a ClickHouse Cloud host
    *   8123 if `driver` is "http" and `host` is not a ClickHouse Cloud host

### ALTER SERVER

Use [ALTER SERVER] to change a foreign server. Example:

```sql
ALTER SERVER taxi_srv OPTIONS (SET driver 'http');
```

The options are the same as for [CREATE SERVER](#create-server).

### DROP SERVER

Use [DROP SERVER] to remove a foreign server:

```sql
DROP SERVER taxi_srv;
```

This command fails if any other objects depend on the server. Use `CASCADE` to
also drop those dependencies:

```sql
DROP SERVER taxi_srv CASCADE;
```

### CREATE USER MAPPING

Use [CREATE USER MAPPING] to map a PostgreSQL user to a ClickHouse user. For
example, to map the current PostgreSQL user to the remote ClickHouse user when
connecting with the `taxi_srv` foreign server:

```sql
CREATE USER MAPPING FOR CURRENT_USER SERVER taxi_srv
       OPTIONS (user 'demo');
```

The The supported options are:

*   `user`: The name of the ClickHouse user. Defaults to "default".
*   `password`: The password of the ClickHouse user.

### ALTER USER MAPPING

Use [ALTER USER MAPPING] to change the definition of a user mapping:

```sql
ALTER USER MAPPING FOR CURRENT_USER SERVER taxi_srv
       OPTIONS (SET user 'default');
```

The options are the same as for [CREATE USER MAPPING](#create-user-mapping).

### DROP USER MAPPING

Use [DROP USER MAPPING] to remove a user mapping:

```sql
DROP USER MAPPING FOR CURRENT_USER SERVER taxi_srv;
```

### IMPORT FOREIGN SCHEMA

Use [IMPORT FOREIGN SCHEMA] to import all the tables defines in a ClickHouse
database as foreign tables into a PostgreSQL schema:

```sql
CREATE SCHEMA taxi;
IMPORT FOREIGN SCHEMA demo FROM SERVER taxi_srv INTO taxi;
```

Use `LIMIT TO` to limit the import to specific tables:

```sql
IMPORT FOREIGN SCHEMA demo LIMIT TO (trips) FROM SERVER taxi_srv INTO taxi;
```

Use `EXCEPT` to exclude tables:

```sql
IMPORT FOREIGN SCHEMA demo EXCEPT (users) FROM SERVER taxi_srv INTO taxi;
```

pg_clickhouse will fetch a list of all the tables in the specified ClickHouse
database ("demo" in the above examples), fetch column definitions for each,
and execute [CREATE FOREIGN TABLE](#create-foreign-table) commands to create
the foreign tables. Columns will be defined using the [supported data
types](#data-types) and, were detectible, the options supported by [CREATE
FOREIGN TABLE](#create-foreign-table).

### CREATE FOREIGN TABLE

Use [IMPORT FOREIGN SCHEMA] to create a foreign table that can query data from
a ClickHouse database:

```sql
CREATE FOREIGN TABLE uact (
    user_id    bigint NOT NULL,
    page_views int,
    duration   smallint,
    sign       smallint
) SERVER taxi_srv OPTIONS(
    table_name 'uact'
    engine 'CollapsingMergeTree'
);
```

The supported table options are:

*   `database`: The name of the remote database. Defaults to the database
    defined for the foreign server.
*   `table_name`: The name of the remote table. Default to the name specified
    for the foreign table.
*   `engine`: The [table engine] used by the ClickHouse table. For
    `CollapsingMergeTree()` and `AggregatingMergeTree()`, pg_clickhouse
    automatically applies the parameters to function expressions executed on
    the table.

Use the [data type](#data-types) appropriate for the remote ClickHouse data
type of each column. For [AggregateFunction Type] and [SimpleAggregateFunction
Type] columns, map the data type to the ClickHouse type passed to the function
and specify the name of the aggregate function via the appropriate column
option:

*   `AggregateFunction`: The name of the aggregate function applied to an
    [AggregateFunction Type] column
*   `SimpleAggregateFunction`: The name of the aggregate function applied to
    an [SimpleAggregateFunction Type] column

Example:

(aggregatefunction 'sum')

```sql
CREATE FOREIGN TABLE test (
    column1 bigint  OPTIONS(AggregateFunction 'uniq'),
    column2 integer OPTIONS(AggregateFunction 'anyIf'),
    column3 bigint  OPTIONS(AggregateFunction 'quantiles(0.5, 0.9)')
) SERVER clickhouse_srv;
```

For columns with the `AggregateFunction` function, pg_clickhouse will
automatically append `Merge` to an aggregate function evaluating the column.

### ALTER FOREIGN TABLE

Use [ALTER FOREIGN TABLE] to change the definition of a foreign table:

```sql
ALTER TABLE table ALTER COLUMN b OPTIONS (SET AggregateFunction 'count');
```

The supported table and column options are the same as for [CREATE FOREIGN
TABLE].

### DROP FOREIGN TABLE

Use [DROP FOREIGN TABLE] to remove a foreign table:

```sql
DROP FOREIGN TABLE uact;
```

This command fails if there are any objects that depend on the foreign table.
Use the `CASCADE` clause to drop them, too:

```sql
DROP FOREIGN TABLE uact CASCADE;
```

## Function and Operator Reference

### Data Types

pg_clickhouse maps the following ClickHouse data types to PostgreSQL data
types:

```
 ClickHouse |    PostgreSQL    |             Notes             
------------+------------------+-------------------------------
 Bool       | boolean          | 
 Date       | date             | 
 DateTime   | timestamp        | 
 Decimal    | numeric          | 
 Float32    | real             | 
 Float64    | double precision | 
 IPv4       | inet             | 
 IPv6       | inet             | 
 Int16      | smallint         | 
 Int32      | integer          | 
 Int64      | bigint           | 
 Int8       | smallint         | 
 JSON       | jsonb            | HTTP engine only
 String     | text             | 
 UInt16     | integer          | 
 UInt32     | bigint           | 
 UInt64     | bigint           | Errors on values > BIGINT max
 UInt8      | smallint         | 
 UUID       | uuid             | 
```

### Functions

These functions provide the interface to query a ClickHouse database.

#### `clickhouse_raw_query`

```sql
SELECT clickhouse_raw_query(
    'CREATE TABLE t1 (x String) ENGINE = Memory',
    'host=localhost port=8123'
);
```

Connect to a ClickHouse service via its HTTP interface, execute a single
query, and disconnect. The optional second argument specifies a connection
string that defaults to `host=localhost port=8123`. The supported connection
parameters are:

*   `host`: The host to connect to; required.
*   `port`: The HTTP port to connect to; defaults to `8123` unless `host` is a
    ClickHouse Cloud host, in which case it defaults to `8443`
*   `dbname`: The name of the database to connect to.
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
```sql
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

All PostgreSQL builtin functions used in conditionals (`HAVING` and `WHERE`
clauses) to query ClickHouse foreign tables automatically push down to
ClickHouse with the same names and signatures. However, some have different
names or signatures and must be mapped to their equivalents. `pg_clickhouse`
maps the following functions:

*   `date_part`:
    *   `date_part('day')`: [toDayOfMonth](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toDayOfMonth)
    *   `date_part('doy')`: [toDayOfYear](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toDayOfYear)
    *   `date_part('dow')`: [toDayOfWeek](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toDayOfWeek)
    *   `date_part('year')`: [toYear](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toYear)
    *   `date_part('month')`: [toMonth](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toMonth)
    *   `date_part('hour')`: [toHour](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toHour)
    *   `date_part('minute')`: [toMinute](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toMinute)
    *   `date_part('second')`: [toSecond](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toSecond)
    *   `date_part('quarter')`: [toQuarter](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toQuarter)
    *   `date_part('isoyear')`: [toISOYear](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toISOYear)
    *   `date_part('week')`: [toISOYear](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toISOWeek)
    *   `date_part('epoch')`: [toISOYear](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toUnixTimestamp)
*   `date_trunc`:
    *   `date_trunc('week')`: [toMonday](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toMonday)
    *   `date_trunc('second')`: [toStartOfSecond](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfSecond)
    *   `date_trunc('minute')`: [toStartOfMinute](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfMinute)
    *   `date_trunc('hour')`: [toStartOfHour](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfHour)
    *   `date_trunc('day')`: [toStartOfDay](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfDay)
    *   `date_trunc('month')`: [toStartOfMonth](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfMonth)
    *   `date_trunc('quarter')`: [toStartOfQuarter](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfQuarter)
    *   `date_trunc('year')`: [toStartOfYear](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#toStartOfYear)
*   `array_position`: [indexOf](https://clickhouse.com/docs/sql-reference/functions/array-functions#indexOf)
*   `btrim`: [trimBoth](https://clickhouse.com/docs/sql-reference/functions/string-functions#trimboth)
*   `strpos`: [position](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#position)
*   `regexp_like`: [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)

### Custom Functions

These custom functions created by `pg_clickhouse` provide foreign query
pushdown for select ClickHouse functions with no PostgreSQL equivalents. If
any of these functions cannot be pushed down they will raise an exception.

*   [dictGet](https://clickhouse.com/docs/sql-reference/functions/ext-dict-functions#dictget-dictgetordefault-dictgetornull)

### Pushdown Casts

pg_clickhouse pushes down casts such as `CAST(x AS bigint)` for compatible
data types. For incompatible types the pushdown will fail; if `x` in this
example is a ClickHouse `UInt64`, ClickHouse will refuse to cast the value.

In order to push down casts to incompatible data types, pg_clickhouse provides
the following functions. They raise an exception in PostgreSQL if they are not
pushed down.

*   [toUInt8](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#touint8)
*   [toUInt16](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#touint16)
*   [toUInt32](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#touint32)
*   [toUInt64](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#touint64)
*   [toUInt128](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#touint128)

### Pushdown Aggregates

These PostgreSQL aggregate functions pushdown to ClickHouse.

*   [count](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/count)

### Custom Aggregates

These custom aggregate functions created by `pg_clickhouse` provide foreign
query pushdown for select ClickHouse aggregate functions with no PostgreSQL
equivalents. If any of these functions cannot be pushed down they will raise
an exception.

*   [argMax](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/argmax)
*   [argMin](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/argmin)
*   [uniq](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniq)
*   [uniqCombined](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqcombined)
*   [uniqCombined64](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqcombined64)
*   [uniqExact](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqexact)
*   [uniqHLL12](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqhll12)
*   [uniqTheta](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/uniqthetasketch)
*   [quantile](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantile)
*   [quantileExact](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantileexact)

### Pushdown Ordered Set Aggregates

These [ordered-set aggregate functions] map to ClickHouse [Parametric
aggregate functions] by passing their *direct argument* as a parameter and
their `ORDER BY` expressions as arguments. For example, this PostgreSQL query:

```sql
SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a) FROM t1;
```

Maps to this ClickHouse query:

```sql
SELECT quantile(0.25)(a) FROM t1;
```

Note that the non-default `ORDER BY` suffixes `DESC` and `NULLS FIRST`
are not supported and will raise an error.

*   `percentile_cont(double)`: [quantile](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantile)
*   `quantile(double)`: [quantile](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantile)
*   `quantileExact(double)`: [quantileExact](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantileexact)

### Session Settings

Set the `pg_clickhouse.session_settings` runtime parameter to configure
[ClickHouse settings] to be set on subsequent queries. Example:

```sql
SET pg_clickhouse.session_settings = 'join_use_nulls 1, final 1';
```

The default is `join_use_nulls 1`. Set it to an empty string to fall back on
the ClickHouse server's settings.

```sql
SET pg_clickhouse.session_settings = '';
```

The syntax is a comma-delimited list of key/value pairs separated by one or
more spaces. Keys must correspond to [ClickHouse settings]. Escape spaces,
commas, and backslashes in values with a backslash:

```sql
SET pg_clickhouse.session_settings = 'join_algorithm grace_hash\,hash';
```

Or use single quoted values to avoid escaping spaces and commas; consider
using [dollar quoting] to avoid the need to double-quote:

```sql
SET pg_clickhouse.session_settings = $$join_algorithm 'grace_hash,hash'$$;
```

If you care about legibility and need to set many settings, use multiple
lines, for example:

```sql
SET pg_clickhouse.session_settings TO $$
    connect_timeout 2,
    count_distinct_implementation uniq,
    final 1,
    group_by_use_nulls 1,
    join_algorithm 'prefer_partial_merge',
    join_use_nulls 1,
    log_queries_min_type QUERY_FINISH,
    max_block_size 32768,
    max_execution_time 45,
    max_result_rows 1024,
    metrics_perf_events_list 'this,that',
    network_compression_method ZSTD,
    poll_interval 5,
    totals_mode after_having_auto
$$;
```

pg_clickhouse does not validate the settings, but passes them on to ClickHouse
for every query. It thus supports all settings for each ClickHouse version.

Note that pg_clickhouse must be loaded before setting
`pg_clickhouse.session_settings`; either use [library preloading] or simply
use one of the objects in the extension to ensure it loads.

## Authors

*   [David E. Wheeler](https://justatheory.com/)
*   [Ildus Kurbangaliev](https://github.com/ildus)
*   [Ibrar Ahmed](https://github.com/ibrarahmad)

## Copyright

*   Copyright (c) 2025, ClickHouse
*   Portions Copyright (c) 2023-2025, Ildus Kurbangaliev
*   Portions Copyright (c) 2019-2023, Adjust GmbH
*   Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group

  [foreign data wrapper]: https://www.postgresql.org/docs/current/fdwhandler.html
    "PostgreSQL Docs: Writing a Foreign Data Wrapper"
  [Docker image]: https://github.com/ClickHouse/pg_clickhouse/pkgs/container/pg_clickhouse
    "Latest version on Docker Hub"
  [ClickHouse]: https://clickhouse.com/clickhouse
  [Semantic Versioning]: https://semver.org/spec/v2.0.0.html
    "Semantic Versioning 2.0.0"
  [CREATE EXTENSION]: https://www.postgresql.org/docs/current/sql-createextension.html
    "PostgreSQL Docs: CREATE EXTENSION"
  [ALTER EXTENSION]: https://www.postgresql.org/docs/current/sql-alterextension.html
    "PostgreSQL Docs: ALTER EXTENSION"
  [DROP EXTENSION]: https://www.postgresql.org/docs/current/sql-dropextension.html
    "PostgreSQL Docs: DROP EXTENSION"
  [CREATE SERVER]: https://www.postgresql.org/docs/current/sql-createserver.html
    "PostgreSQL Docs: CREATE SERVER"
  [ALTER SERVER]: https://www.postgresql.org/docs/current/sql-alterserver.html
    "PostgreSQL Docs: ALTER SERVER"
  [DROP SERVER]: https://www.postgresql.org/docs/current/sql-dropserver.html
    "PostgreSQL Docs: DROP SERVER"
  [CREATE USER MAPPING]: https://www.postgresql.org/docs/current/sql-createusermapping.html
    "PostgreSQL Docs: CREATE USER MAPPING"
  [ALTER USER MAPPING]: https://www.postgresql.org/docs/current/sql-alterusermapping.html
    "PostgreSQL Docs: ALTER USER MAPPING"
  [DROP USER MAPPING]: https://www.postgresql.org/docs/current/sql-dropusermapping.html
    "PostgreSQL Docs: DROP USER MAPPING"
  [IMPORT FOREIGN SCHEMA]: https://www.postgresql.org/docs/current/sql-importforeignschema.html
    "PostgreSQL Docs: IMPORT FOREIGN SCHEMA"
  [table engine]: https://clickhouse.com/docs/engines/table-engines
    "ClickHouse Docs: Table engines"
  [AggregateFunction Type]: https://clickhouse.com/docs/sql-reference/data-types/aggregatefunction
    "ClickHouse Docs: AggregateFunction Type"
  [SimpleAggregateFunction Type]: https://clickhouse.com/docs/sql-reference/data-types/simpleaggregatefunction
    "ClickHouse Docs: SimpleAggregateFunction Type"
  [ordered-set aggregate functions]: https://www.postgresql.org/docs/current/functions-aggregate.html#FUNCTIONS-ORDEREDSET-TABLE
  [Parametric aggregate functions]: https://clickhouse.com/docs/sql-reference/aggregate-functions/parametric-functions
  [ClickHouse settings]: https://clickhouse.com/docs/operations/settings/settings
    "ClickHouse Docs: Session Settings"
  [dollar quoting]: https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-SYNTAX-DOLLAR-QUOTING
    "PostgreSQL Docs: Dollar-Quoted String Constants"
  [library preloading]: https://www.postgresql.org/docs/18/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-PRELOAD
    "PostgreSQL Docs: Shared Library Preloading
