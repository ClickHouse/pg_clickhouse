pg_clickhouse 0.1.0
===================

## Synopsis

```pgsql
CREATE EXTENSION pg_clickhouse;
```

## Description

This library contains a single PostgreSQL extension that enables remote query
execution on ClickHouse databases, including a [foreign data wrapper]. It
supports PostgreSQL 13 and higher and ClickHouse 22 and higher.

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
*   `array_position`: [toTimeZone](https://clickhouse.com/docs/sql-reference/functions/array-functions#indexOf)
*   `btrim`: [trimBoth](https://clickhouse.com/docs/sql-reference/functions/string-functions#trimboth)
*   `strpos`: [position](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#position)
*   `regexp_like` => [match](https://clickhouse.com/docs/sql-reference/functions/string-search-functions#match)

### Custom Functions

These custom functions created by `pg_clickhouse` provide foreign query
pushdown for select ClickHouse functions with no PostgreSQL equivalents. If
any of these functions cannot be pushed down they will raise an exception.

*   [dictGet](https://clickhouse.com/docs/sql-reference/functions/ext-dict-functions#dictget-dictgetordefault-dictgetornull)
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

*   `percentile_cont(double)` => [quantile](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantile)
*   `quantile(double)` => [quantile](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantile)
*   `quantileExact(double)` => [quantileExact](https://clickhouse.com/docs/sql-reference/aggregate-functions/reference/quantileexact)

### Session Settings

Set the `pg_clickhouse.session_settings` runtime parameter to configure
[ClickHouse settings] to be set on subsequent queries. Example:

```sql
SET pg_clickhouse.session_settings = 'join_use_nulls=1, final=1';
```

The default is `join_use_nulls=1`. Set it to an empty string to fall back on
the ClickHouse server's settings.

```sql
SET pg_clickhouse.session_settings = '';
```

The syntax is a comma-delimited list of key/value pairs separated by an equal
sign. Keys must correspond to [ClickHouse settings]. Escape spaces, commas,
and backslashes in values with a backslash:

```sql
SET pg_clickhouse.session_settings = 'join_algorithm = grace_hash\,hash';
```

Or use single quoted values to avoid escaping spaces and commas; consider
using [dollar quoting] to avoid the need to double-quote:

```sql
SET pg_clickhouse.session_settings = $$join_algorithm = 'grace_hash,hash'$$;
```

pg_clickhouse does not validate the settings, but passes them on to ClickHouse
for every query. It thus supports all settings for each ClickHouse version.

Note that pg_clickhouse must be loaded before setting
`pg_clickhouse.session_settings`; either use [library preloading] or simply
use one of the objects in the extension to ensure it loads.

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
  [ordered-set aggregate functions]: https://www.postgresql.org/docs/current/functions-aggregate.html#FUNCTIONS-ORDEREDSET-TABLE
  [Parametric aggregate functions]: https://clickhouse.com/docs/sql-reference/aggregate-functions/parametric-functions
  [ClickHouse settings]: https://clickhouse.com/docs/operations/settings/settings
    "ClickHouse Docs: Session Settings"
  [dollar quoting]: https://www.postgresql.org/docs/current/sql-syntax-lexical.html#SQL-SYNTAX-DOLLAR-QUOTING
    "PostgreSQL Docs: Dollar-Quoted String Constants"
  [library preloading]: https://www.postgresql.org/docs/18/runtime-config-client.html#RUNTIME-CONFIG-CLIENT-PRELOAD
    "PostgreSQL Docs: Shared Library Preloading
