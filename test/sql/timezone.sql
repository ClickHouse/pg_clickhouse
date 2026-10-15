SET datestyle = 'ISO';
-- Create servers for each engine.
CREATE SERVER tz_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER tz_bin_svr;

CREATE SERVER tz_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER tz_http_svr;

CREATE SERVER tz_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER tz_admin;

CALL clickhouse_perform('tz_admin', 'DROP DATABASE IF EXISTS tz_test');
CALL clickhouse_perform('tz_admin', 'CREATE DATABASE tz_test');
CALL clickhouse_perform('tz_admin', $$
    CREATE TABLE tz_test.ts (
        id        Int,
        server_ts DateTime,
        utc_ts    DateTime('UTC'),
        nyc_ts    DateTime('America/New_York'),
        lax_ts    DateTime('America/Los_Angeles')
    ) ENGINE = MergeTree ORDER BY id
$$);

-- Insert some records, all times set to 10:00:00 UTC.
CALL clickhouse_perform('tz_admin', $$
    INSERT INTO tz_test.ts
    SELECT number,
           addMonths(toDateTime('2020-01-01 10:00:00', 'UTC'), number * 3),
           addMonths(toDateTime('2020-01-01 10:00:00', 'UTC'), number * 3),
           addMonths(toDateTime('2020-01-01 10:00:00', 'UTC'), number * 3),
           addMonths(toDateTime('2020-01-01 10:00:00', 'UTC'), number * 3)
    FROM numbers(0, 4)
$$);

-- Read as text under UTC, so every column shows the instant that was stored.
SET session timezone = 'UTC';
SELECT * FROM clickhouse_query('tz_admin', $$
    SELECT * FROM tz_test.ts ORDER BY id
$$) AS t(id text, server_ts text, utc_ts text, nyc_ts text, lax_ts text);

CREATE SCHEMA tz_bin;
IMPORT FOREIGN SCHEMA tz_test FROM SERVER tz_bin_svr INTO tz_bin;
\d tz_bin.ts
CREATE SCHEMA tz_http;
IMPORT FOREIGN SCHEMA tz_test FROM SERVER tz_http_svr INTO tz_http;
\d tz_http.ts

SET session timezone = 'UTC';
SELECT * FROM tz_bin.ts ORDER BY id;
SELECT * FROM tz_http.ts ORDER BY id;

SET session timezone = 'America/New_York';
SELECT * FROM tz_bin.ts ORDER BY id;
SELECT * FROM tz_http.ts ORDER BY id;

SET session timezone = 'America/Los_Angeles';
SELECT * FROM tz_bin.ts ORDER BY id;
SELECT * FROM tz_http.ts ORDER BY id;

-- With parameters. Execute 6 times to get parameter passing to kick in.
SET session timezone = 'UTC';
PREPARE prep_bin(int) AS SELECT * FROM tz_bin.ts WHERE id > $1;
EXECUTE prep_bin(1);
EXECUTE prep_bin(1);
EXECUTE prep_bin(1);
EXECUTE prep_bin(1);
EXECUTE prep_bin(1);
EXECUTE prep_bin(1);
DEALLOCATE prep_bin;

PREPARE prep_http(int) AS SELECT * FROM tz_http.ts WHERE id > $1;
EXECUTE prep_http(1);
EXECUTE prep_http(1);
EXECUTE prep_http(1);
EXECUTE prep_http(1);
EXECUTE prep_http(1);
-- Diff results < 28.8, due to https://github.com/ClickHouse/ClickHouse/issues/85847
EXECUTE prep_http(1);
DEALLOCATE prep_http;

-- Extract and truncate timestamptz in PostgreSQL TimeZone, not column zone.
SET session timezone = 'America/New_York';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_trunc('day', lax_ts) AS d, date_part('hour', utc_ts) AS h,
       to_char(nyc_ts, 'HH24') AS c, to_char(utc_ts::date, 'YYYY-MM') AS m
  FROM tz_bin.ts GROUP BY d, h, c, m ORDER BY d;
SELECT date_trunc('day', lax_ts) AS d, date_part('hour', utc_ts) AS h,
       to_char(nyc_ts, 'HH24') AS c, to_char(utc_ts::date, 'YYYY-MM') AS m
  FROM tz_bin.ts GROUP BY d, h, c, m ORDER BY d;
SELECT date_trunc('day', lax_ts) AS d, date_part('hour', utc_ts) AS h,
       to_char(nyc_ts, 'HH24') AS c, to_char(utc_ts::date, 'YYYY-MM') AS m
  FROM tz_http.ts GROUP BY d, h, c, m ORDER BY d;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tz_bin.ts WHERE extract(hour FROM lax_ts) = 5;
SELECT id FROM tz_bin.ts WHERE extract(hour FROM lax_ts) = 5;
SELECT id FROM tz_http.ts WHERE extract(hour FROM lax_ts) = 5;

-- Wall clock 02:30 falls in Los Angeles DST gap, so must not pass through it.
CALL clickhouse_perform('tz_admin', $$
    CREATE TABLE tz_test.gap (ts DateTime('UTC')) ENGINE = MergeTree ORDER BY ts
$$);
CALL clickhouse_perform('tz_admin', $$
    INSERT INTO tz_test.gap VALUES ('2020-03-08 02:30:00')
$$);
CREATE FOREIGN TABLE tz_bin.gap (ts timestamptz) SERVER tz_bin_svr
    OPTIONS (database 'tz_test');
SET session timezone = 'America/Los_Angeles';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_part('hour', ts AT TIME ZONE 'UTC') AS h,
       date_part('epoch', ts AT TIME ZONE 'UTC') AS e,
       to_char(ts AT TIME ZONE 'UTC', 'HH24:MI') AS c,
       (ts AT TIME ZONE 'UTC')::date AS d,
       (ts AT TIME ZONE 'UTC') AT TIME ZONE 'UTC' AS r
  FROM tz_bin.gap GROUP BY h, e, c, d, r;
SELECT date_part('hour', ts AT TIME ZONE 'UTC') AS h,
       date_part('epoch', ts AT TIME ZONE 'UTC') AS e,
       to_char(ts AT TIME ZONE 'UTC', 'HH24:MI') AS c,
       (ts AT TIME ZONE 'UTC')::date AS d,
       (ts AT TIME ZONE 'UTC') AT TIME ZONE 'UTC' AS r
  FROM tz_bin.gap GROUP BY h, e, c, d, r;

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER tz_bin_svr;
DROP SERVER tz_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER tz_http_svr;
DROP SERVER tz_http_svr CASCADE;
