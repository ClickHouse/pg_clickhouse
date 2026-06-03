pg_clickhouse 0.3.1
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
the standard PostgreSQL Docker image with the pg_clickhouse and [re2][re2
extension] extensions:

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
    `pgch_version()` function or the Postgres [`pg_get_loaded_modules()`]
    function.
*   The extension version (defined in the control file) includes only the
    major and minor versions, visible in the `pg_catalog.pg_extension` table,
    the output of the `pg_available_extension_versions()` function, and `\dx
    pg_clickhouse`.

In practice this means that a release that increments the patch version, e.g.
from `v0.1.0` to `v0.1.1`, benefits all databases that have loaded `v0.1` and
do not need to run `ALTER EXTENSION` to benefit from the upgrade.

A release that increments the minor or major versions, on the other hand, will
be accompanied by SQL upgrade scripts, and all existing database that contain
the extension must run `ALTER EXTENSION pg_clickhouse UPDATE` to benefit from
the upgrade.

## DDL SQL Reference

The following SQL [DDL] expressions use pg_clickhouse.

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
*   `fetch_size`: Approximate batch size in bytes for HTTP streaming. Batches
    split on row boundaries. Defaults to `50000000` (50 MB). `0` disables
    streaming and buffers the full response. Foreign tables can override this
    value.
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

The supported options are:

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

> **⚠️ Imported Identifier Case Preservation**
>
> `IMPORT FOREIGN SCHEMA` runs `quote_identifier()` on the table and column
> names it imports, which double-quotes identifiers with uppercase characters
> or blank spaces. Such table and column names thus must be double-quoted in
> PostgreSQL queries. Names with all lowercase and no blank space characters
> do not need to be quoted.
>
> For example, given this ClickHouse table:
>
> ```sql
> CREATE OR REPLACE TABLE test
> (
>     id UInt64,
>     Name TEXT,
>     updatedAt DateTime DEFAULT now()
> )
> ENGINE = MergeTree
> ORDER BY id;
> ```
>
> `IMPORT FOREIGN SCHEMA` creates this foreign table:
>
> ```sql
> CREATE TABLE test
> (
>     id          BIGINT      NOT NULL,
>     "Name"      TEXT        NOT NULL,
>     "updatedAt" TIMESTAMPTZ NOT NULL
> );
> ```
>
> Queries therefore must quote appropriately, e.g.,
>
> ```sql
> SELECT id, "Name", "updatedAt" FROM test;
> ```
>
> To create objects with different names or all lowercase (and therefore
> case-insensitive) names, use [CREATE FOREIGN TABLE](#create-foreign-table).

### CREATE FOREIGN TABLE

Use [CREATE FOREIGN TABLE] to create a foreign table that can query data from
a ClickHouse database:

```sql
CREATE FOREIGN TABLE acts (
    user_id    bigint NOT NULL,
    page_views int,
    duration   smallint,
    sign       smallint
) SERVER taxi_srv OPTIONS(
    table_name 'acts',
    engine 'CollapsingMergeTree'
);
```

The supported table options are:

*   `database`: The name of the remote database. Defaults to the database
    defined for the foreign server.
*   `fetch_size`: Approximate batch size in bytes for HTTP streaming.
    Overrides server-level `fetch_size`. Defaults to `50000000` (50 MB). `0`
    disables streaming and buffers the full response.
*   `table_name`: The name of the remote table. Default to the name specified
    for the foreign table.
*   `engine`: The [table engine] used by the ClickHouse table. For
    `CollapsingMergeTree()` and `AggregatingMergeTree()`, pg_clickhouse
    automatically applies the parameters to function expressions executed on
    the table.

Use the [data type](#data-types) appropriate for the remote ClickHouse data
type of each column. The supported column options are:

*   `column_name`: The name of the column on the ClickHouse side, used in
    preference to the PostgreSQL attribute name when deparsing queries and
    inserts. Useful for mapping unquoted lowercase PostgreSQL column names to
    case-sensitive ClickHouse columns, e.g.,

    ```sql
    CREATE FOREIGN TABLE hits (
        watchid    bigint   OPTIONS(column_name 'WatchID'),
        javaenable smallint OPTIONS(column_name 'JavaEnable'),
        title      text     OPTIONS(column_name 'Title')
    ) SERVER taxi_srv OPTIONS(table_name 'hits');
    ```

*   `AggregateFunction`: The name of the aggregate function applied to an
    [AggregateFunction Type] column. Map the data type to the ClickHouse type
    passed to the function and specify the name of the aggregate function via
    the appropriate column option and pg_clickhouse will automatically append
    `Merge` to an aggregate function evaluating the column.

    ```sql
    CREATE FOREIGN TABLE test (
        column1 bigint  OPTIONS(AggregateFunction 'uniq'),
        column2 integer OPTIONS(AggregateFunction 'anyIf'),
        column3 bigint  OPTIONS(AggregateFunction 'quantiles(0.5, 0.9)')
    ) SERVER clickhouse_srv;
    ```

*   `SimpleAggregateFunction`: The name of the aggregate function applied to
    an [SimpleAggregateFunction Type] column. Map the data type to the
    ClickHouse type passed to the function and specify the name of the
    aggregate function via the appropriate column option.

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
DROP FOREIGN TABLE acts;
```

This command fails if there are any objects that depend on the foreign table.
Use the `CASCADE` clause to drop them, too:

```sql
DROP FOREIGN TABLE acts CASCADE;
```

## DML SQL Reference

The SQL [DML] expressions below may use pg_clickhouse. Examples depend on
these ClickHouse tables, created by [make-logs.sql](make-logs.sql):

```sql
CREATE TABLE logs (
    req_id    Int64 NOT NULL,
    start_at   DateTime64(6, 'UTC') NOT NULL,
    duration  Int32 NOT NULL,
    resource  Text  NOT NULL,
    method    Enum8('GET' = 1, 'HEAD', 'POST', 'PUT', 'DELETE', 'CONNECT', 'OPTIONS', 'TRACE', 'PATCH', 'QUERY') NOT NULL,
    node_id   Int64 NOT NULL,
    response  Int32 NOT NULL
) ENGINE = MergeTree
  ORDER BY start_at;

CREATE TABLE nodes (
    node_id Int64 NOT NULL,
    name    Text  NOT NULL,
    region  Text  NOT NULL,
    arch    Text  NOT NULL,
    os      Text  NOT NULL
) ENGINE = MergeTree
  PRIMARY KEY node_id;
```

### EXPLAIN

The [EXPLAIN] command works as expected, but the `VERBOSE` option triggers the
ClickHouse "Remote SQL" query to be emitted:

```pgsql
try=# EXPLAIN (VERBOSE)
       SELECT resource, avg(duration) AS average_duration
         FROM logs
        GROUP BY resource;
                                     QUERY PLAN
------------------------------------------------------------------------------------
 Foreign Scan  (cost=1.00..5.10 rows=1000 width=64)
   Output: resource, (avg(duration))
   Relations: Aggregate on (logs)
   Remote SQL: SELECT resource, avg(duration) FROM "default".logs GROUP BY resource
(4 rows)
```

This query pushes down to ClickHouse via a "Foreign Scan" plan node, the
remote SQL.

### SELECT

Use the [SELECT] statement to execute queries on pg_clickhouse tables just
like any other tables:

```pgsql
try=# SELECT start_at, duration, resource FROM logs WHERE req_id = 4117909262;
          start_at          | duration |    resource
----------------------------+----------+----------------
 2025-12-05 15:07:32.944188 |      175 | /widgets/totem
(1 row)
```

pg_clickhouse works to push query execution down to ClickHouse as much as
possible, including aggregate functions. Use [EXPLAIN](#explain) to determine
the pushdown extent. For the above query, for example, all execution is pushed
down to ClickHouse

```pgsql
try=# EXPLAIN (VERBOSE, COSTS OFF)
       SELECT start_at, duration, resource FROM logs WHERE req_id = 4117909262;
                                             QUERY PLAN
-----------------------------------------------------------------------------------------------------
 Foreign Scan on public.logs
   Output: start_at, duration, resource
   Remote SQL: SELECT start_at, duration, resource FROM "default".logs WHERE ((req_id = 4117909262))
(3 rows)
```

pg_clickhouse also pushes down JOINs to tables that are from the same remote
server:

```pgsql
try=# EXPLAIN (ANALYZE, VERBOSE)
       SELECT name, count(*), round(avg(duration))
         FROM logs
         LEFT JOIN nodes on logs.node_id = nodes.node_id
        GROUP BY name;
                                                                                  QUERY PLAN
-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 Foreign Scan  (cost=1.00..5.10 rows=1000 width=72) (actual time=3.201..3.221 rows=8.00 loops=1)
   Output: nodes.name, (count(*)), (round(avg(logs.duration), 0))
   Relations: Aggregate on ((logs) LEFT JOIN (nodes))
   Remote SQL: SELECT r2.name, count(*), round(avg(r1.duration), 0) FROM  "default".logs r1 ALL LEFT JOIN "default".nodes r2 ON (((r1.node_id = r2.node_id))) GROUP BY r2.name
   FDW Time: 0.086 ms
 Planning Time: 0.335 ms
 Execution Time: 3.261 ms
(7 rows)
```

Joining with a local table will generate less efficient queries without
careful tuning. In this example, we make a local copy of the
`nodes` table and join to it instead of the remote table:

```pgsql
try=# CREATE TABLE local_nodes AS SELECT * FROM nodes;
SELECT 8

try=# EXPLAIN (ANALYZE, VERBOSE)
       SELECT name, count(*), round(avg(duration))
         FROM logs
         LEFT JOIN local_nodes on logs.node_id = local_nodes.node_id
        GROUP BY name;
                                                             QUERY PLAN
-------------------------------------------------------------------------------------------------------------------------------------
 HashAggregate  (cost=147.65..150.65 rows=200 width=72) (actual time=6.215..6.235 rows=8.00 loops=1)
   Output: local_nodes.name, count(*), round(avg(logs.duration), 0)
   Group Key: local_nodes.name
   Batches: 1  Memory Usage: 32kB
   Buffers: shared hit=1
   ->  Hash Left Join  (cost=31.02..129.28 rows=2450 width=36) (actual time=2.202..5.125 rows=1000.00 loops=1)
         Output: local_nodes.name, logs.duration
         Hash Cond: (logs.node_id = local_nodes.node_id)
         Buffers: shared hit=1
         ->  Foreign Scan on public.logs  (cost=10.00..20.00 rows=1000 width=12) (actual time=2.089..3.779 rows=1000.00 loops=1)
               Output: logs.req_id, logs.start_at, logs.duration, logs.resource, logs.method, logs.node_id, logs.response
               Remote SQL: SELECT duration, node_id FROM "default".logs
               FDW Time: 1.447 ms
         ->  Hash  (cost=14.90..14.90 rows=490 width=40) (actual time=0.090..0.091 rows=8.00 loops=1)
               Output: local_nodes.name, local_nodes.node_id
               Buckets: 1024  Batches: 1  Memory Usage: 9kB
               Buffers: shared hit=1
               ->  Seq Scan on public.local_nodes  (cost=0.00..14.90 rows=490 width=40) (actual time=0.069..0.073 rows=8.00 loops=1)
                     Output: local_nodes.name, local_nodes.node_id
                     Buffers: shared hit=1
 Planning:
   Buffers: shared hit=14
 Planning Time: 0.551 ms
 Execution Time: 6.589 ms
```

In this case, we can push more of the aggregation down to ClickHouse by
grouping on `node_id` instead of the local column, and then join
to the lookup table later:

```sql
try=# EXPLAIN (ANALYZE, VERBOSE)
       WITH remote AS (
           SELECT node_id, count(*), round(avg(duration))
             FROM logs
            GROUP BY node_id
       )
       SELECT name, remote.count, remote.round
         FROM remote
         JOIN local_nodes
           ON remote.node_id = local_nodes.node_id
        ORDER BY name;
                                                          QUERY PLAN
-------------------------------------------------------------------------------------------------------------------------------
 Sort  (cost=65.68..66.91 rows=490 width=72) (actual time=4.480..4.484 rows=8.00 loops=1)
   Output: local_nodes.name, remote.count, remote.round
   Sort Key: local_nodes.name
   Sort Method: quicksort  Memory: 25kB
   Buffers: shared hit=4
   ->  Hash Join  (cost=27.60..43.79 rows=490 width=72) (actual time=4.406..4.422 rows=8.00 loops=1)
         Output: local_nodes.name, remote.count, remote.round
         Inner Unique: true
         Hash Cond: (local_nodes.node_id = remote.node_id)
         Buffers: shared hit=1
         ->  Seq Scan on public.local_nodes  (cost=0.00..14.90 rows=490 width=40) (actual time=0.010..0.016 rows=8.00 loops=1)
               Output: local_nodes.node_id, local_nodes.name, local_nodes.region, local_nodes.arch, local_nodes.os
               Buffers: shared hit=1
         ->  Hash  (cost=15.10..15.10 rows=1000 width=48) (actual time=4.379..4.381 rows=8.00 loops=1)
               Output: remote.count, remote.round, remote.node_id
               Buckets: 1024  Batches: 1  Memory Usage: 9kB
               ->  Subquery Scan on remote  (cost=1.00..15.10 rows=1000 width=48) (actual time=4.337..4.360 rows=8.00 loops=1)
                     Output: remote.count, remote.round, remote.node_id
                     ->  Foreign Scan  (cost=1.00..5.10 rows=1000 width=48) (actual time=4.330..4.349 rows=8.00 loops=1)
                           Output: logs.node_id, (count(*)), (round(avg(logs.duration), 0))
                           Relations: Aggregate on (logs)
                           Remote SQL: SELECT node_id, count(*), round(avg(duration), 0) FROM "default".logs GROUP BY node_id
                           FDW Time: 0.055 ms
 Planning:
   Buffers: shared hit=5
 Planning Time: 0.319 ms
 Execution Time: 4.562 ms
 ```

 The "Foreign Scan" node now pushes down aggregation by `node_id`, reducing
 the number of rows that must be pulled back into Postgres from 1000 (all of
 them) to just 8, one for each node.

### PREPARE, EXECUTE, DEALLOCATE

 As of v0.1.2, pg_clickhouse supports parameterized queries, mainly created
 by the [PREPARE] command:

```pgsql
try=# PREPARE avg_durations_between_dates(date, date) AS
       SELECT date(start_at), round(avg(duration)) AS average_duration
         FROM logs
        WHERE date(start_at) BETWEEN $1 AND $2
        GROUP BY date(start_at)
        ORDER BY date(start_at);
PREPARE
```

Use [EXECUTE] as usual to execute a prepared statement:

```pgsql
try=# EXECUTE avg_durations_between_dates('2025-12-09', '2025-12-13');
    date    | average_duration
------------+------------------
 2025-12-09 |              190
 2025-12-10 |              194
 2025-12-11 |              197
 2025-12-12 |              190
 2025-12-13 |              195
(5 rows)
```

> [!WARNING]
> Parameterized execution prevents the [http driver](#create-server) from
> properly converting DateTime time zones on ClickHouse versions prior to
> 25.8, when the [underlying bug] was [fixed]. Note that sometimes PostgreSQL
> will use a parameterized query plan even without using `PREPARE`. For any
> queries on that require accurate time zone conversion, and where upgrading
> to 25.8 or later is not an option, use the [binary driver](#create-server),
> instead.

pg_clickhouse pushes down the aggregations, as usual, as seen in the
[EXPLAIN](#explain) verbose output:

```pgsql
try=# EXPLAIN (VERBOSE) EXECUTE avg_durations_between_dates('2025-12-09', '2025-12-13');
                                                                                                            QUERY PLAN
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 Foreign Scan  (cost=1.00..5.10 rows=1000 width=36)
   Output: (date(start_at)), (round(avg(duration), 0))
   Relations: Aggregate on (logs)
   Remote SQL: SELECT date(start_at), round(avg(duration), 0) FROM "default".logs WHERE ((date(start_at) >= '2025-12-09')) AND ((date(start_at) <= '2025-12-13')) GROUP BY (date(start_at)) ORDER BY date(start_at) ASC NULLS LAST
(4 rows)
```

Note that it has sent the full date values, not the parameter placeholders.
This holds for the first five requests, as described in the PostgreSQL
[PREPARE notes]. On the sixth execution, it sends ClickHouse
`{param:type}`-style [query parameters]:
parameters:

```pgsql
                                                                                                         QUERY PLAN
-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 Foreign Scan  (cost=1.00..5.10 rows=1000 width=36)
   Output: (date(start_at)), (round(avg(duration), 0))
   Relations: Aggregate on (logs)
   Remote SQL: SELECT date(start_at), round(avg(duration), 0) FROM "default".logs WHERE ((date(start_at) >= {p1:Date})) AND ((date(start_at) <= {p2:Date})) GROUP BY (date(start_at)) ORDER BY date(start_at) ASC NULLS LAST
(4 rows)
```

Use [DEALLOCATE] to deallocate a prepared statement:

```pgsql
try=# DEALLOCATE avg_durations_between_dates;
DEALLOCATE
```

### INSERT

Use the [INSERT] command to insert values into a remote ClickHouse table:

```pgsql
try=# INSERT INTO nodes(node_id, name, region, arch, os)
VALUES (9,  'Augustin Gamarra', 'us-west-2', 'amd64', 'Linux')
     , (10, 'Cerisier', 'us-east-2', 'amd64', 'Linux')
     , (11, 'Dewalt', 'use-central-1', 'arm64', 'macOS')
;
INSERT 0 3
```

### COPY

Use the [COPY] command to insert a batch of rows into a remote ClickHouse
table:

```pgsql
try=# COPY logs FROM stdin CSV;
4285871863,2025-12-05 11:13:58.360760,206,/widgets,POST,8,401
4020882978,2025-12-05 11:33:48.248450,199,/users/1321945,HEAD,3,200
3231273177,2025-12-05 12:20:42.158575,220,/search,GET,2,201
\.
>> COPY 3
```

> **⚠️ Batch API Limitations**
>
> pg_clickhouse has not yet implemented support for the PostgreSQL FDW batch
> insert API. Thus [COPY] currently uses [INSERT](#insert) statements to
> insert records. This will be improved in a future release.

### LOAD

Use [LOAD] to load the pg_clickhouse shared library:

```pgsql
try=# LOAD 'pg_clickhouse';
LOAD
```

It's not normally necessary to use [LOAD], as Postgres will automatically load
pg_clickhouse the first time any of its features (functions, foreign tables,
etc.) are used.

The one time it may be useful to [LOAD] pg_clickhouse is to [SET](#set)
pg_clickhouse parameters before executing queries that depend on them.

### SET

Use [SET] to set the pg_clickhouse custom configuration parameters.

#### `pg_clickhouse.session_settings`

The `pg_clickhouse.session_settings` parameter configures [ClickHouse
settings] to be set on subsequent queries. Example:

```sql
SET pg_clickhouse.session_settings = 'join_use_nulls 1, final 1';
```

The default is `join_use_nulls 1, group_by_use_nulls 1, final 1`. Set it to an
empty string to fall back on the ClickHouse server's settings.

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

Some settings will be ignored in cases where they would interfere with the
operation of pg_clickhouse itself. These include:

*   `date_time_output_format`: the http driver requires it to be "iso"
*   `format_tsv_null_representation`: the http driver requires the default
*   `output_format_tsv_crlf_end_of_line` the http driver requires the default

Otherwise, pg_clickhouse does not validate the settings, but passes them on to
ClickHouse for every query. It thus supports all settings for each ClickHouse
version.

Note that pg_clickhouse must be loaded before setting
`pg_clickhouse.session_settings`; either use [shared library preloading] or
simply use one of the objects in the extension to ensure it loads.

#### `pg_clickhouse.pushdown_regex`

The `pg_clickhouse.pushdown_regex` parameter controls whether pg_clickhouse
pushes down regular expression functions and operators. It does so by default;
set this parameter to false to prevent them from being pushed down:

```sql
SET pg_clickhouse.pushdown_regex = 'false';
```

See [Regular Expressions](#regular-expressions) for details.

### ALTER ROLE

Use [ALTER ROLE]'s `SET` command to [preload](#preloading) pg_clickhouse
and/or [SET](#set) its parameters for specific roles:

```pgsql
try=# ALTER ROLE CURRENT_USER SET session_preload_libraries = pg_clickhouse;
ALTER ROLE

try=# ALTER ROLE CURRENT_USER SET pg_clickhouse.session_settings = 'final 1';
ALTER ROLE
```

Use the [ALTER ROLE]'s `RESET` command to reset pg_clickhouse preloading
and/or parameters:

```pgsql
try=# ALTER ROLE CURRENT_USER RESET session_preload_libraries;
ALTER ROLE

try=# ALTER ROLE CURRENT_USER RESET pg_clickhouse.session_settings;
ALTER ROLE
```

## Preloading

If every or nearly every Postgres connection needs to use pg_clickhouse,
consider using [shared library preloading] to automatically load it:

### `session_preload_libraries`

Loads the shared library for every new connection to PostgreSQL:

```ini
session_preload_libraries = pg_clickhouse
```

Useful to take advantage of updates without restarting the server: just
reconnect. May also be set for specific users or roles via [ALTER
ROLE](#alter-role).

### `shared_preload_libraries`

Loads the shared library into the PostgreSQL parent process at startup time:

```ini
shared_preload_libraries = pg_clickhouse
```

Useful to save memory and load overhead for every session, but requires the
cluster to be restart when the library is updated.

## Data Types

pg_clickhouse maps the following ClickHouse data types to PostgreSQL data
types. [IMPORT FOREIGN SCHEMA](#import-foreign-schema) uses the first type in
the PostgreSQL column when importing columns; additional types may be used in
[CREATE FOREIGN TABLE](#create-foreign-table) statements:

| ClickHouse |    PostgreSQL    |             Notes             |
|------------|------------------|-------------------------------|
| Bool       | boolean          |                               |
| Date       | date             |                               |
| Date32     | date             |                               |
| DateTime   | timestamptz      |                               |
| Decimal    | numeric          |                               |
| Float32    | real             |                               |
| Float64    | double precision |                               |
| IPv4       | inet             |                               |
| IPv6       | inet             |                               |
| Int16      | smallint         |                               |
| Int32      | integer          |                               |
| Int64      | bigint           |                               |
| Int8       | smallint         |                               |
| JSON       | jsonb, json      |                               |
| String     | text, bytea      |                               |
| UInt16     | integer          |                               |
| UInt32     | bigint           |                               |
| UInt64     | bigint           | Errors on values > BIGINT max |
| UInt8      | smallint         |                               |
| UUID       | uuid             |                               |

Additional notes and details follow.

### BYTEA

ClickHouse does not provide the equivalent of the PostgreSQL [BYTEA] type, but
allows any bytes to be stored in [String] type. In general ClickHouse strings
should be mapped to the PostgreSQL [TEXT], but when using binary data, map it
to [BYTEA]. Example:

```sql
-- Create clickHouse table with String columns.
SELECT clickhouse_raw_query($$
    CREATE TABLE bytes (
	      c1 Int8, c2 String, c3 String
    ) ENGINE = MergeTree ORDER BY (c1);
$$);

-- Create foreign table with BYTEA columns.
CREATE FOREIGN TABLE bytes (
    c1 int,
    c2 BYTEA,
    c3 BYTEA
) SERVER ch_srv OPTIONS( table_name 'bytes' );

-- Insert binary data into the foreign table.
INSERT INTO bytes
SELECT n, sha224(bytea('val'||n)), decode(md5('int'||n), 'hex')
  FROM generate_series(1, 4) n;

-- View the results.
SELECT * FROM bytes;
```

That final `SELECT` query will output:

```pgsql
 c1 |                             c2                             |                 c3
----+------------------------------------------------------------+------------------------------------
  1 | \x1bf7f0cc821d31178616a55a8e0c52677735397cdde6f4153a9fd3d7 | \xae3b28cde02542f81acce8783245430d
  2 | \x5f6e9e12cd8592712e638016f4b1a2e73230ee40db498c0f0b1dc841 | \x23e7c6cacb8383f878ad093b0027d72b
  3 | \x53ac2c1fa83c8f64603fe9568d883331007d6281de330a4b5e728f9e | \x7e969132fc656148b97b6a2ee8bc83c1
  4 | \x4e3c2e4cb7542a45173a8dac939ddc4bc75202e342ebc769b0f5da2f | \x8ef30f44c65480d12b650ab6b2b04245
(4 rows)
```

Note that if there are any nul bytes in the ClickHouse columns, a foreign
table using [TEXT] columns will not output the proper values:

```sql
-- Create foreign table with TEXT columns.
CREATE FOREIGN TABLE texts (
    c1 int,
    c2 TEXT,
    c3 TEXT
) SERVER ch_srv OPTIONS( table_name 'bytes' );

-- Encode binary data as hex.
SELECT c1, encode(c2::bytea, 'hex'), encode(c3::bytea, 'hex') FROM texts ORDER BY c1;
```

Will output:

```pgsql
 c1 |                          encode                          |              encode
----+----------------------------------------------------------+----------------------------------
  1 | 1bf7f0cc821d31178616a55a8e0c52677735397cdde6f4153a9fd3d7 | ae3b28cde02542f81acce8783245430d
  2 | 5f6e9e12cd8592712e638016f4b1a2e73230ee40db498c0f0b1dc841 | 23e7c6cacb8383f878ad093b
  3 | 53ac2c1fa83c8f64603fe9568d883331                         | 7e969132fc656148b97b6a2ee8bc83c1
  4 | 4e3c2e4cb7542a45173a8dac939ddc4bc75202e342ebc769b0f5da2f | 8ef30f44c65480d12b650ab6b2b04245
(4 rows)
```

Note that rows two and three contain truncated values. This is because
PostgreSQL relies on nul-terminated strings and does not support nuls in its
strings.

Attempting to insert binary values into [TEXT] columns will succeed and work
as expected:

```sql
-- Insert via text columns:
TRUNCATE texts;
INSERT INTO texts
SELECT n, sha224(bytea('val'||n)), decode(md5('int'||n), 'hex')
  FROM generate_series(1, 4) n;

-- View the data.
SELECT c1, encode(c2::bytea, 'hex'), encode(c3::bytea, 'hex') FROM texts ORDER BY c1;
```

The text columns will be correct:

```pgsql

 c1 |                          encode                          |              encode
----+----------------------------------------------------------+----------------------------------
  1 | 1bf7f0cc821d31178616a55a8e0c52677735397cdde6f4153a9fd3d7 | ae3b28cde02542f81acce8783245430d
  2 | 5f6e9e12cd8592712e638016f4b1a2e73230ee40db498c0f0b1dc841 | 23e7c6cacb8383f878ad093b0027d72b
  3 | 53ac2c1fa83c8f64603fe9568d883331007d6281de330a4b5e728f9e | 7e969132fc656148b97b6a2ee8bc83c1
  4 | 4e3c2e4cb7542a45173a8dac939ddc4bc75202e342ebc769b0f5da2f | 8ef30f44c65480d12b650ab6b2b04245
(4 rows)
```

But reading them as [BYTEA] will not:

```pgsql
# SELECT * FROM bytes;
 c1 |                                                           c2                                                           |                                   c3
----+------------------------------------------------------------------------------------------------------------------------+------------------------------------------------------------------------
  1 | \x5c783162663766306363383231643331313738363136613535613865306335323637373733353339376364646536663431353361396664336437 | \x5c786165336232386364653032353432663831616363653837383332343534333064
  2 | \x5c783566366539653132636438353932373132653633383031366634623161326537333233306565343064623439386330663062316463383431 | \x5c783233653763366361636238333833663837386164303933623030323764373262
  3 | \x5c783533616332633166613833633866363436303366653935363864383833333331303037643632383164653333306134623565373238663965 | \x5c783765393639313332666336353631343862393762366132656538626338336331
  4 | \x5c783465336332653463623735343261343531373361386461633933396464633462633735323032653334326562633736396230663564613266 | \x5c783865663330663434633635343830643132623635306162366232623034323435
(4 rows)
```

> [!TIP]
> As a rule, only use [TEXT] columns for encoded strings and use [BYTEA]
> columns only for binary data, and never switch between them.

## Function and Operator Reference

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

By default, no role has `EXECUTE` access to this function; consider [GRANT]ing
access only to roles that legitimately need to execute ad-hoc ClickHouse
queries, e.g., a dedicated ClickHouse admin role:

```sql
GRANT EXECUTE ON FUNCTION clickhouse_raw_query(text, text) TO ch_admin;
```

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

`pg_clickhouse` pushes down a subset of the PostgreSQL builtin functions used
in conditionals (`HAVING` and `WHERE` clauses). That subset maps to ClickHouse
equivalents as follows:

*   `abs`: [abs](https://clickhouse.com/docs/sql-reference/functions/arithmetic-functions#abs)
*   `factorial`: [factorial](https://clickhouse.com/docs/sql-reference/functions/math-functions#factorial)
*   `mod` (int2/int4/int8/numeric): [modulo](https://clickhouse.com/docs/sql-reference/functions/arithmetic-functions#modulo)
*   `pow` & `power` (float8/numeric): [pow](https://clickhouse.com/docs/sql-reference/functions/math-functions#pow)
*   `round`: [round](https://clickhouse.com/docs/sql-reference/functions/rounding-functions#round)
*   `sin`, `cos`, `tan`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `asinh`,
    `degrees`, `radians`, `pi`: [ClickHouse math functions](https://clickhouse.com/docs/sql-reference/functions/math-functions)
    of the same name. `asin`, `acos`, `atanh`, `acosh` are not pushed
    down: PG raises on out-of-range input where CH returns `NaN`.
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
*   `extract(field FROM source)`: same mappings as `date_part`
*   `date(timestamp)` & `date(timestamptz)`: [toDate](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#toDate)
    (deparsed as CH alias `date`)
*   `array_position`: [indexOf](https://clickhouse.com/docs/sql-reference/functions/array-functions#indexOf)
*   `array_cat`: [arrayConcat](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayConcat)
*   `array_append`: [arrayPushBack](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayPushBack)
*   `array_prepend`: [arrayPushFront](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayPushFront)
*   `array_remove`: [arrayRemove](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayRemove)
*   `array_length` & `cardinality`: [length](https://clickhouse.com/docs/sql-reference/functions/array-functions#length)
*   `array_to_string`: [arrayStringConcat](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayStringConcat)
*   `string_to_array`: [splitByString](https://clickhouse.com/docs/sql-reference/functions/splitting-merging-functions#splitByString)
*   `split_part`: [splitByString](https://clickhouse.com/docs/sql-reference/functions/splitting-merging-functions#splitByString) + array subscript
*   `trim_array`: [arrayResize](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayResize)
*   `array_fill`: [arrayWithConstant](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayWithConstant)
*   `array_reverse`: [arrayReverse](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayReverse)
*   `array_shuffle`: [arrayShuffle](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayShuffle)
*   `array_sample`: [arrayRandomSample](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayRandomSample)
*   `array_sort`: [arraySort](https://clickhouse.com/docs/sql-reference/functions/array-functions#arraySort) / [arrayReverseSort](https://clickhouse.com/docs/sql-reference/functions/array-functions#arrayReverseSort)
*   `btrim`: [trimBoth](https://clickhouse.com/docs/sql-reference/functions/string-functions#trimboth)
*   `ltrim`: [ltrim](https://clickhouse.com/docs/sql-reference/functions/string-functions#ltrim)
*   `rtrim`: [rtrim](https://clickhouse.com/docs/sql-reference/functions/string-functions#rtrim)
*   `concat_ws`: [concatWithSeparator](https://clickhouse.com/docs/sql-reference/functions/string-functions#concatwithseparator)
*   `lower(text)`: [lowerUTF8](https://clickhouse.com/docs/sql-reference/functions/string-functions#lowerutf8)
*   `upper(text)`: [upperUTF8](https://clickhouse.com/docs/sql-reference/functions/string-functions#upperutf8)
*   `substring(text, ...)` & `substr(text, ...)`: [substringUTF8](https://clickhouse.com/docs/sql-reference/functions/string-functions#substringutf8)
*   `substring(bytea, ...)` & `substr(bytea, ...)`: [substring](https://clickhouse.com/docs/sql-reference/functions/string-functions#substring)
*   `length(text)`: [lengthUTF8](https://clickhouse.com/docs/sql-reference/functions/string-functions#lengthutf8)
*   `length(bytea)` & `octet_length`: [length](https://clickhouse.com/docs/sql-reference/functions/array-functions#length)
*   `reverse(text)`: [reverseUTF8](https://clickhouse.com/docs/sql-reference/functions/string-functions#reverseutf8)
*   `reverse(bytea)`: [reverse](https://clickhouse.com/docs/sql-reference/functions/string-functions#reverse)
*   `strpos`: [positionUTF8](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#positionutf8)
*   `regexp_like`: [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)
*   `regexp_replace`: [replaceRegexpOne](https://clickhouse.com/docs/sql-reference/functions/string-replace-functions#replaceRegexpOne) or [replaceRegexpOne](https://clickhouse.com/docs/sql-reference/functions/string-replace-functions#replaceRegexpAll) when the `g` flag is present
*   `regexp_split_to_array`: [splitByRegexp](https://clickhouse.com/docs/sql-reference/functions/splitting-merging-functions#splitByRegexp)
*   `md5`: [MD5](https://clickhouse.com/docs/sql-reference/functions/hash-functions#MD5)
*   `json_extract_path_text`: [sub-column syntax](https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns)
*   `json_extract_path`: [toJSONString](https://clickhouse.com/docs/sql-reference/functions/json-functions#toJSONString) + [sub-column syntax](https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns)
*   `jsonb_extract_path_text`: [sub-column syntax](https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns)
*   `jsonb_extract_path`: [toJSONString](https://clickhouse.com/docs/sql-reference/functions/json-functions#toJSONString) + [sub-column syntax](https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns)
*   `bit_count(bytea)`: [bitCount](https://clickhouse.com/docs/sql-reference/functions/bit-functions#bitcount)
*   `to_timestamp(float8)`: [fromUnixTimestamp](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#fromUnixTimestamp)
*   `to_char(timestamp[tz], fmt)`: [formatDateTime](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#formatDateTime)
    when `fmt` is a string constant whose every keyword has a faithful
    ClickHouse equivalent. See [to_char()](#to_char) under Compatibility
    Notes for the supported keywords. Otherwise the function evaluates
    locally in PostgreSQL.
*   `statement_timestamp`, `transaction_timestamp`, & `clock_timestamp`:
    [nowInBlock64](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#nowInBlock64)
    (`nowInBlock64(9, $session_timezone)`)
*   `CURRENT_DATE`:
    [now](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#now) and
    [toDate](https://clickhouse.com/docs/sql-reference/functions/type-conversion-functions#toDate)
    (`toDate(now($session_timezone))`)
*   `now`, `CURRENT_TIMESTAMP`, & `LOCALTIMESTAMP`:
    [now64](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#now64)
    (`now64(9, $session_timezone)`)
*   `CURRENT_TIMESTAMP(n)` & `LOCALTIMESTAMP(n)`:
    [now64](https://clickhouse.com/docs/sql-reference/functions/date-time-functions#now64)
    (`now64(n, $session_timezone)`)
*   `CURRENT_DATABASE`: Passed as value from PostgreSQL function.
*   `CURRENT_SCHEMA`: Passed as value from PostgreSQL function.
*   `CURRENT_CATALOG`: Passed as value from PostgreSQL function.
*   `CURRENT_USER`: Passed as value from PostgreSQL function.
*   `USER`: Passed as value from PostgreSQL function.
*   `CURRENT_ROLE`: Passed as value from PostgreSQL function.
*   `SESSION_USER`: Passed as value from PostgreSQL function.

### Pushdown Operators

*   Array slice (`arr[L:U]`): [arraySlice](https://clickhouse.com/docs/sql-reference/functions/array-functions#arraySlice)
*   `@>` (array contains): [hasAll](https://clickhouse.com/docs/sql-reference/functions/array-functions#hasAll)
*   `<@` (array contained by): [hasAll](https://clickhouse.com/docs/sql-reference/functions/array-functions#hasAll)
*   `&&` (array overlap): [hasAny](https://clickhouse.com/docs/sql-reference/functions/array-functions#hasAny)
*   `~` (regexp match): [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)
*   `!~` (regexp not match): [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)
*   `~*` (case insensitive regexp no match): [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)
*   `!~*` (case insensitive regexp not match): [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)
*   `->>` (JSON/JSONB extract element as text): [sub-column syntax](https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns)
*   `->` (JSON/JSONB extract): [toJSONString](https://clickhouse.com/docs/sql-reference/functions/json-functions#toJSONString) + [sub-column syntax](https://clickhouse.com/docs/sql-reference/data-types/newjson#reading-json-paths-as-sub-columns)

### Custom Functions

These custom functions created by `pg_clickhouse` provide foreign query
pushdown for select ClickHouse functions with no PostgreSQL equivalents. If
any of these functions cannot be pushed down they will raise an exception.

*   [dictGet](https://clickhouse.com/docs/sql-reference/functions/ext-dict-functions#dictget-dictgetordefault-dictgetornull)

### Extension Pushdown

pg_clickhouse recognizes functions from select core and third-party extensions,
pushing them down to their ClickHouse equivalents.

#### re2

All [re2 extension] functions push down 1:1 to ClickHouse:

*   `re2match` → [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)
*   `re2extract` → [extract](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#extract)
*   `re2extractall` → [extractAll](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#extractAll)
*   `re2regexpextract` → [regexpExtract](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#regexpExtract)
*   `re2extractgroups` → [extractGroups](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#extractGroups)
*   `re2replaceregexpone` → [replaceRegexpOne](https://clickhouse.com/docs/sql-reference/functions/string-replace-functions#replaceRegexpOne)
*   `re2replaceregexpall` → [replaceRegexpAll](https://clickhouse.com/docs/sql-reference/functions/string-replace-functions#replaceRegexpAll)
*   `re2countmatches` → [countMatches](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#countMatches)
*   `re2countmatchescaseinsensitive` → [countMatchesCaseInsensitive](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#countMatchesCaseInsensitive)
*   `re2multimatchany` → [multiMatchAny](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#multiMatchAny)
*   `re2multimatchanyindex` → [multiMatchAnyIndex](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#multiMatchAnyIndex)
*   `re2multimatchallindices` → [multiMatchAllIndices](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#multiMatchAllIndices)

#### intarray

One [intarray] function pushes down to ClickHouse:

*   `idx` → [indexOf](https://clickhouse.com/docs/sql-reference/functions/array-functions#indexOf)

#### fuzzystrmatch

Two [fuzzystrmatch] functions push down to ClickHouse:

*   `soundex`: [soundex](https://clickhouse.com/docs/sql-reference/functions/string-functions#soundex)
*   `levenshtein` (2-arg): [editDistanceUTF8](https://clickhouse.com/docs/sql-reference/functions/string-functions#editDistanceUTF8)

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

*   [array_agg](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/grouparray)
*   [avg](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/avg)
*   [bit_and](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/groupbitand)
*   [bit_or](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/groupbitor)
*   [bit_xor](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/groupbitxor)
*   [bool_and / every](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/groupbitand)
*   [bool_or](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/groupbitor)
*   [count](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/count)
*   [min](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/min)
*   [max](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/max)
*   [string_agg](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/groupconcat)
*   [sum](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/sum)

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

### Pushdown Window Functions

These PostgreSQL [window functions] push down to ClickHouse with `OVER
(PARTITION BY ... ORDER BY ...)` clauses, including frame specifications where
applicable.

*   [row_number](https://clickhouse.com/docs/sql-reference/window-functions#row_number)
*   [rank](https://clickhouse.com/docs/sql-reference/window-functions#rank)
*   [dense_rank](https://clickhouse.com/docs/sql-reference/window-functions#dense_rank)
*   [ntile](https://clickhouse.com/docs/sql-reference/window-functions#ntile)
*   [cume_dist](https://clickhouse.com/docs/sql-reference/window-functions#cume_dist)
*   [percent_rank](https://clickhouse.com/docs/sql-reference/window-functions#percent_rank)
*   [lead](https://clickhouse.com/docs/sql-reference/window-functions#lead)
*   [lag](https://clickhouse.com/docs/sql-reference/window-functions#lag)
*   [first_value](https://clickhouse.com/docs/sql-reference/window-functions#first_value)
*   [last_value](https://clickhouse.com/docs/sql-reference/window-functions#last_value)
*   [nth_value](https://clickhouse.com/docs/sql-reference/window-functions#nth_value)
*   `min` / `max` (with `OVER` clause)

Ranking functions (`row_number`, `rank`, `dense_rank`, `ntile`, `cume_dist`,
`percent_rank`) omit their frame clause during pushdown because ClickHouse
rejects frame specifications on these functions.

## Compatibility Notes

### Regular Expressions

While pg_clickhouse pushes down regular expressions to ClickHouse equivalents
when [pg_clickhouse.pushdown_regex](#pg_clickhousepushdown_regex) is true (the
default), and makes an effort to ensure a basic level of compatibility, be
aware of the differences between the two and how pg_clickhouse handles them.

*   PostgreSQL supports [POSIX Regular Expressions] while ClickHouse supports
    [RE2 Regular Expressions][RE2]. Beware of differences in behavior: write
    RE2 when the regular expression will be evaluated by ClickHouse (e.g., in
    a `WHERE` clause) and POSIX when it will be evaluated by Postgres (e.g.,
    in a `SELECT` clause).

*   pg_clickhouse pushes down the Postgres [Regex flags] by prepending them
    to ClickHouse regular expression inside `(?)`. For example:

    ``` sql
    regexp_like(val, '^VAL\d', 'i')
    ```

    Becomes

    ```sql
    match(val, concat('(?i-s)', '^VAL\\d'))
    ```

    Note the inclusion of `-s`; this aligns the behavior with Postgres regular
    expressions by disabling `s`, which ClickHouse enables by default.
    pg_clickhouse will not include `-s` if the flags in the Postgres function
    call include `s`. Unfortunately, this behavior breaks the compatibility of
    some regular expression in Postgres 24 and earlier.

*   The only flags both support, and therefore can be used when evaluated by
    ClickHouse, are:

    *   `i`: case-insensitive
    *   `m`: multi-line mode:
    *   `s`: let `.` match `\n`
    *   `p`: partial newline-sensitive matching (treated the same as `s`)
    *   `t`: tight syntax (the default, removed by pg_clickhouse)

    RE2 supports only these flags; don't use any other [Postgres flags]

*   Any other flags passed to regular expression functions will cause the
    function not to be pushed down.

*   The exception is `regexp_replace()`, which also supports the `g` flag.
    When `g` is set, pg_clickhouse uses `replaceRegexpAll()` instead of
    `replaceRegexpOne()` and removes the flag before prepending other flags.

*   The replacement argument to Postgres `regexp_replace()` supports `\&` to
    refer to the entire match, while in ClickHouse supports `\0` for the
    entire match. Be sure to use `\0` when the function pushes down to
    ClickHouse.

To avoid all ambiguity, consider setting
[pg_clickhouse.pushdown_regex](#pg_clickhousepushdown_regex) to prevent
Postgres regular expression from pushing down to ClickHouse, and using the
[re2 extension], for which pg_clickhouse supports [direct pushdown](#re2) of
ClickHouse-compatible [RE2] regular expressions.

### `to_char()`

PostgreSQL [`to_char()`] for `timestamp` and `timestamp with time zone`
pushes down to ClickHouse [formatDateTime] only when the format argument
is a non-NULL string constant whose every PostgreSQL keyword has a
byte-for-byte identical ClickHouse equivalent. If the format is dynamic
(not a `Const`), or contains any unsupported keyword or modifier, the
call falls back to local evaluation in PostgreSQL — pushdown is never
attempted with a partial translation, so output stays PG-compatible.

Two-argument `to_char()` forms over `numeric`, `interval`, and other
non-timestamp types never push down; ClickHouse [formatDateTime] only
formats date-time values.

#### Translated keywords

| PostgreSQL                 | ClickHouse | Meaning                               |
| -------------------------- | ---------- | ------------------------------------- |
| `YYYY`, `yyyy`             | `%Y`       | 4-digit year                          |
| `YY`, `yy`                 | `%y`       | 2-digit year                          |
| `MM`, `mm`                 | `%m`       | zero-padded month (01–12)             |
| `DD`, `dd`                 | `%d`       | zero-padded day of month (01–31)      |
| `DDD`, `ddd`               | `%j`       | zero-padded day of year (001–366)     |
| `HH24`, `hh24`             | `%H`       | zero-padded 24-hour (00–23)           |
| `HH`, `hh`, `HH12`, `hh12` | `%I`       | zero-padded 12-hour (01–12)           |
| `MI`, `mi`                 | `%i`       | zero-padded minute (00–59)            |
| `SS`, `ss`                 | `%S`       | zero-padded second (00–59)            |
| `Q`, `q`                   | `%Q`       | quarter (1–4)                         |
| `Mon`                      | `%b`       | abbreviated month name, e.g., `Oct`   |
| `Dy`                       | `%a`       | abbreviated weekday name, e.g., `Mon` |
| `AM`, `PM`                 | `%p`       | meridiem indicator, always uppercase  |

#### Quoted text and literals

Text wrapped in `"..."` passes through verbatim, with any literal `%`
doubled to `%%` to escape ClickHouse's specifier prefix. A `\"` outside
quotes also passes through as a literal `"`. Inside `"..."`, backslash
only escapes `"`; other backslash sequences are treated as literal text.

## Authors

*   [David E. Wheeler](https://justatheory.com/)

## Copyright

Copyright (c) 2025-2026, ClickHouse.

  [foreign data wrapper]: https://www.postgresql.org/docs/current/fdwhandler.html
    "PostgreSQL Docs: Writing a Foreign Data Wrapper"
  [Docker image]: https://github.com/ClickHouse/pg_clickhouse/pkgs/container/pg_clickhouse
    "Latest version on Docker Hub"
  [ClickHouse]: https://clickhouse.com/clickhouse
  [Semantic Versioning]: https://semver.org/spec/v2.0.0.html
    "Semantic Versioning 2.0.0"
  [`pg_get_loaded_modules()`]: https://pgpedia.info/g/pg_get_loaded_modules.html
    "pgPedia: pg_get_loaded_modules()"
  [DDL]: https://en.wikipedia.org/wiki/Data_definition_language
    "Wikipedia: Data definition language"
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
  [CREATE FOREIGN TABLE]: https://www.postgresql.org/docs/current/sql-createforeigntable.html
    "PostgreSQL Docs: CREATE FOREIGN TABLE"
  [table engine]: https://clickhouse.com/docs/engines/table-engines
    "ClickHouse Docs: Table engines"
  [AggregateFunction Type]: https://clickhouse.com/docs/sql-reference/data-types/aggregatefunction
    "ClickHouse Docs: AggregateFunction Type"
  [SimpleAggregateFunction Type]: https://clickhouse.com/docs/sql-reference/data-types/simpleaggregatefunction
    "ClickHouse Docs: SimpleAggregateFunction Type"
  [ALTER FOREIGN TABLE]: https://www.postgresql.org/docs/current/sql-alterforeigntable.html
    "PostgreSQL Docs: ALTER FOREIGN TABLE"
  [DROP FOREIGN TABLE]: https://www.postgresql.org/docs/current/sql-dropforeigntable.html
    "PostgreSQL Docs: DROP FOREIGN TABLE"
  [DML]: https://en.wikipedia.org/wiki/Data_manipulation_language
    "Wikipedia: Data manipulation language"
  [EXPLAIN]: https://www.postgresql.org/docs/current/sql-explain.html
    "PostgreSQL Docs: EXPLAIN"
  [SELECT]: https://www.postgresql.org/docs/current/sql-select.html
    "PostgreSQL Docs: SELECT"
  [PREPARE]: https://www.postgresql.org/docs/current/sql-prepare.html
    "PostgreSQL Docs: PREPARE"
  [EXECUTE]: https://www.postgresql.org/docs/current/sql-execute.html
    "PostgreSQL Docs: EXECUTE"
  [DEALLOCATE]: https://www.postgresql.org/docs/current/sql-deallocate.html
    "PostgreSQL Docs: DEALLOCATE"
  [PREPARE]: https://www.postgresql.org/docs/current/sql-prepare.html
    "PostgreSQL Docs: PREPARE"
  [INSERT]: https://www.postgresql.org/docs/current/sql-insert.html
    "PostgreSQL Docs: INSERT"
  [COPY]: https://www.postgresql.org/docs/current/sql-copy.html
    "PostgreSQL Docs: COPY"
  [LOAD]: https://www.postgresql.org/docs/current/sql-load.html
    "PostgreSQL Docs: LOAD"
  [SET]: https://www.postgresql.org/docs/current/sql-set.html
    "PostgreSQL Docs: SET"
  [ALTER ROLE]: https://www.postgresql.org/docs/current/sql-alterrole.html
    "PostgreSQL Docs: ALTER ROLE"
  [shared library preloading]: https://www.postgresql.org/docs/current/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-PRELOAD
    "PostgreSQL Docs: Shared Library Preloading"
  [ordered-set aggregate functions]: https://www.postgresql.org/docs/current/functions-aggregate.html#FUNCTIONS-ORDEREDSET-TABLE
  [Parametric aggregate functions]: https://clickhouse.com/docs/sql-reference/aggregate-functions/parametric-functions
  [ClickHouse settings]: https://clickhouse.com/docs/operations/settings/settings
    "ClickHouse Docs: Session Settings"
  [dollar quoting]: https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-SYNTAX-DOLLAR-QUOTING
    "PostgreSQL Docs: Dollar-Quoted String Constants"
  [PREPARE notes]: https://www.postgresql.org/docs/current/sql-prepare.html#SQL-PREPARE-NOTES
    "PostgreSQL Docs: PREPARE notes"
  [query parameters]: https://clickhouse.com/docs/guides/developer/stored-procedures-and-prepared-statements#alternatives-to-prepared-statements-in-clickhouse
    "ClickHouse Docs: Alternatives to prepared statements in ClickHouse"
  [underlying bug]: https://github.com/ClickHouse/ClickHouse/issues/85847
    "ClickHouse/ClickHouse#85847 Some queries in a multipart forms don't read settings"
  [fixed]: https://github.com/ClickHouse/ClickHouse/pull/85570
    "ClickHouse/ClickHouse#85570 fix HTTP with multipart"
  [BYTEA]: https://www.postgresql.org/docs/current/datatype-binary.html
    "PostgreSQL Docs: Binary Data Types"
  [GRANT]: https://www.postgresql.org/docs/current/sql-grant.html
    "PostgreSQL Docs: GRANT"
  [String]: https://clickhouse.com/docs/sql-reference/data-types/string
    "ClickHouse Docs: String"
  [TEXT]: https://www.postgresql.org/docs/current/datatype-character.html
    "PostgreSQL Docs: Character Types"
  [window functions]: https://www.postgresql.org/docs/current/functions-window.html
    "PostgreSQL Docs: Window Functions"
  [POSIX Regular Expressions]: https://www.postgresql.org/docs/18/functions-matching.html#FUNCTIONS-POSIX-REGEXP
    "PostgreSQL Docs: POSIX Regular Expressions"
  [Postgres flags]: https://www.postgresql.org/docs/18/functions-matching.html#POSIX-EMBEDDED-OPTIONS-TABLE
    "PostgreSQL Docs: ARE Embedded-Option Letters"
  [RE2]: https://github.com/google/re2/wiki/Syntax "RE2 Syntax"
  [re2 extension]: https://github.com/ClickHouse/pg_re2
    "pg_re2: ClickHouse-compatible regex functions using RE2"
  [intarray]: https://www.postgresql.org/docs/current/intarray.html
    "PostgreSQL Docs: intarray"
  [fuzzystrmatch]: https://www.postgresql.org/docs/current/fuzzystrmatch.html
    "PostgreSQL Docs: fuzzystrmatch"
  [`to_char()`]: https://www.postgresql.org/docs/current/functions-formatting.html
    "PostgreSQL Docs: Data Type Formatting Functions"
  [formatDateTime]: https://clickhouse.com/docs/sql-reference/functions/date-time-functions#formatDateTime
    "ClickHouse Docs: formatDateTime"
