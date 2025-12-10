# Changelog

All notable changes to this project will be documented in this file. It uses the
[Keep a Changelog] format, and this project adheres to [Semantic Versioning].

  [Keep a Changelog]: https://keepachangelog.com/en/1.1.0/
  [Semantic Versioning]: https://semver.org/spec/v2.0.0.html
    "Semantic Versioning 2.0.0"

## [v0.1.1] — Not Yet Released


  [v0.1.1]: https://github.com/clickhouse/pg_clickhouse/compare/v0.1.0...v0.1.1

## [v0.1.0] — 2025-12-09

### ⚡ Improvements

*   Improved function and aggregate pushdown
*   Added TLS support to both the http and binary engines
*   Added pushdown aggregate functions:
    *   `uniq()`
    *   `uniqExact()`
    *   `uniqCombined()`
    *   `uniqCombined64()`
    *   `uniqExact()`
    *   `uniqHLL12()`
    *   `uniqTheta()`
*   Added pushdown functions:
    *   `toUInt8()`
    *   `toUInt16()`
    *   `toUInt32()`
    *   `toUInt64()`
    *   `toUInt128()`
    *   `quantile()`
    *   `quantileExact()`
*   Mapped PostgreSQL `regexp_like()` to push down to ClickHouse `match()`
    function
*   Mapped PostgreSQL `extract()` to push down to equivalent ClickHouse
    DateTime extraction functions (already mapped to `date_part()`)
*   Mapped PostgreSQL `percentile_cont()` ordered set aggregate function to
    push down to ClickHouse `quantile()` parametrized 
*   Mapped the `COUNT()` return value to `bigint`
*   Added the query text and, for the http engine, the status code to error
    messages
*   Added `pg_clickhouse.session_settings` GUC, defaulting to
    `join_use_nulls 1, group_by_use_nulls 1, final 1`
*   Added mappings and support for additional data types:
    *   `Bool` => `boolean`
    *   `Decimal` => `numeric`
    *   `JSON` => `jsonb` (http engine only)
*   Added the `dbname` option to http engine connections, including
    `clickhouse_raw_query()`
*   Added LEFT SEMI JOIN pushdown for `EXISTS()` subqueries

### 🏗️ Build Setup

*   Ported from clickhouse_fdw
*   Made lots of general code cleanup
*   Added PGXS build pipeline
*   Added PGXN and GitHub release workflows
*   Added `pg_clickhouse` OCI image workflow that publishes to
    ghcr.io/clickhouse/pg_clickhouse, with tags for PostgreSQL versions 13-18

### 📚 Documentation

*   Added tutorial in [doc/tutorial.md](doc/tutorial.md)
*   Added reference documentation in [doc/pg_clickhouse.md](doc/pg_clickhouse.md)

### 🪲 Bug Fixes

*   Fixed `dictGet()`, `argMin()`, and `argMax()`
*   Fixed bug in filtered `COUNT()`
*   Fixed `AggregateFunction` option to propagate to a nested aggregate
    function call
*   Improved unsigned integer support

### ⬆️ Dependency Updates

*   Updated vendored clickhouse-cpp library
*   Reimplemented binary engine inserts with clickhouse-cpp improvements
*   Support and tested against PostgreSQL 13-18
*   Support and tested against ClickHouse 23-25

### 📔 Notes

*   Set full version in `PG_MODULE_MAGIC_EXT`
*   Set to default ports to TLS for ClickHouse Cloud host names

  [v0.1.0]: https://github.com/clickhouse/pg_clickhouse/compare/a1487bd...v0.1.0
  [RFC 9535]: https://www.rfc-editor.org/rfc/rfc9535.html
    "RFC 9535 JSONPath: Query Expressions for JSON"
