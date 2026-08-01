-- clickhouse_raw_query() is deprecated and drops out next release.
-- Prefer clickhouse_query() / clickhouse_perform().
SELECT clickhouse_raw_query('CREATE DATABASE IF NOT EXISTS raw_query_test');
SELECT clickhouse_raw_query('DROP DATABASE raw_query_test', 'host=localhost port=8123');
