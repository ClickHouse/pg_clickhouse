SET datestyle = 'ISO';
CREATE SERVER stz_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'stz_test', driver 'binary');
CREATE SERVER stz_http_svr FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'stz_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER stz_bin_svr;
CREATE USER MAPPING FOR CURRENT_USER SERVER stz_http_svr;

CREATE SERVER stz_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER stz_admin;

\set ECHO errors
SELECT clickhouse_server_version('stz_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (23, 6) AS no_ch236 \gset
\if :no_ch236
\echo 'SKIP: session_timezone unsupported prior to ClickHouse 23.6'
\quit
\endif
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) >= (26, 3) AS ch263 \gset
\set ECHO all

CALL clickhouse_perform('stz_admin', 'DROP DATABASE IF EXISTS stz_test');
CALL clickhouse_perform('stz_admin', 'CREATE DATABASE stz_test');
CALL clickhouse_perform('stz_admin', $$
    CREATE TABLE stz_test.wall (id Int, ts DateTime64(6))
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('stz_admin', $$
    CREATE TABLE stz_test.utc (id Int, ts DateTime('UTC'))
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('stz_admin', $$
    CREATE TABLE stz_test.day (id Int, d Date, ts DateTime64(6, 'UTC'))
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('stz_admin', $$
    INSERT INTO stz_test.day VALUES
        (1, '2020-03-07', '2020-03-07 10:00:05.25'),
        (2, '2020-07-01', '1969-12-31 23:59:58.75')
$$);
CALL clickhouse_perform('stz_admin', $$
    INSERT INTO stz_test.utc
    SELECT number, addMonths(toDateTime('2020-01-01 10:00:00', 'UTC'), number * 3)
    FROM numbers(4)
$$);

CREATE FOREIGN TABLE stz_bin_wall (id int, ts timestamp)
    SERVER stz_bin_svr OPTIONS (table_name 'wall');
CREATE FOREIGN TABLE stz_http_wall (id int, ts timestamp)
    SERVER stz_http_svr OPTIONS (table_name 'wall');
CREATE FOREIGN TABLE stz_bin_utc (id int, ts timestamptz)
    SERVER stz_bin_svr OPTIONS (table_name 'utc');
CREATE FOREIGN TABLE stz_http_utc (id int, ts timestamptz)
    SERVER stz_http_svr OPTIONS (table_name 'utc');
CREATE FOREIGN TABLE stz_bin_day (id int, d timestamp, ts timestamptz)
    SERVER stz_bin_svr OPTIONS (table_name 'day');
CREATE FOREIGN TABLE stz_http_day (id int, d timestamp, ts timestamptz)
    SERVER stz_http_svr OPTIONS (table_name 'day');

-- Verify unzoned DateTime64 literals use PostgreSQL TimeZone.
SET timezone = 'America/Los_Angeles';
INSERT INTO stz_bin_wall VALUES (1, '2024-07-01 12:00');
INSERT INTO stz_http_wall VALUES (2, '2024-07-01 12:00');
CALL clickhouse_perform('stz_admin', $$
    INSERT INTO stz_test.wall VALUES (3, '2024-07-01 12:00:00')
$$);
SELECT * FROM clickhouse_query('stz_admin', $$
    SELECT id FROM stz_test.wall WHERE ts = '2024-07-01 12:00:00' ORDER BY id
$$) AS t(id int);
SELECT * FROM stz_bin_wall ORDER BY id;
SELECT * FROM stz_http_wall ORDER BY id;

-- Verify pushed down literals and functions match local evaluation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM stz_bin_wall
 WHERE ts = '2024-07-01 12:00' AND date_part('hour', ts) = 12;
SELECT id FROM stz_bin_wall
 WHERE ts = '2024-07-01 12:00' AND date_part('hour', ts) = 12 ORDER BY id;
SELECT id FROM stz_http_wall
 WHERE ts = '2024-07-01 12:00' AND date_part('hour', ts) = 12 ORDER BY id;

-- Execute six times to test parameters with generic plans.
PREPARE prep_bin(timestamp) AS SELECT id FROM stz_bin_wall WHERE ts = $1 ORDER BY id;
EXECUTE prep_bin('2024-07-01 12:00');
EXECUTE prep_bin('2024-07-01 12:00');
EXECUTE prep_bin('2024-07-01 12:00');
EXECUTE prep_bin('2024-07-01 12:00');
EXECUTE prep_bin('2024-07-01 12:00');
EXECUTE prep_bin('2024-07-01 12:00');
DEALLOCATE prep_bin;
PREPARE prep_http(timestamp) AS SELECT id FROM stz_http_wall WHERE ts = $1 ORDER BY id;
EXECUTE prep_http('2024-07-01 12:00');
EXECUTE prep_http('2024-07-01 12:00');
EXECUTE prep_http('2024-07-01 12:00');
EXECUTE prep_http('2024-07-01 12:00');
EXECUTE prep_http('2024-07-01 12:00');
EXECUTE prep_http('2024-07-01 12:00');
DEALLOCATE prep_http;

-- Read same instants as Tokyo wall clock times. Before ClickHouse 26.3,
-- literals use time zone unzoned columns had when table loaded
\if :ch263
SET timezone = 'Asia/Tokyo';
SELECT * FROM stz_bin_wall WHERE ts = '2024-07-02 04:00' ORDER BY id;
SELECT * FROM stz_http_wall WHERE ts = '2024-07-02 04:00' ORDER BY id;

-- Verify fixed offsets map to ClickHouse Fixed/UTC zones.
SET timezone = '+05:30';
SELECT * FROM stz_bin_wall WHERE ts = '2024-07-01 13:30' ORDER BY id;
SELECT * FROM stz_http_wall WHERE ts = '2024-07-01 13:30' ORDER BY id;
\endif

-- Verify explicit UTC columns use PostgreSQL TimeZone across DST.
SET timezone = 'America/Los_Angeles';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_trunc('day', ts) AS d, to_char(ts, 'HH24') AS h, count(*)
  FROM stz_bin_utc WHERE extract(hour FROM ts) = 3 GROUP BY d, h ORDER BY d;
SELECT date_trunc('day', ts) AS d, to_char(ts, 'HH24') AS h, count(*)
  FROM stz_bin_utc WHERE extract(hour FROM ts) = 3 GROUP BY d, h ORDER BY d;
SELECT date_trunc('day', ts) AS d, to_char(ts, 'HH24') AS h, count(*)
  FROM stz_http_utc WHERE extract(hour FROM ts) = 3 GROUP BY d, h ORDER BY d;
SELECT id, ts, date_part('hour', ts) FROM stz_bin_utc ORDER BY id;

-- Verify interval arithmetic preserves 02:00 across PST and PDT.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM stz_bin_utc WHERE ts + interval '3 months' = '2020-04-01 02:00';
SELECT id FROM stz_bin_utc WHERE ts + interval '3 months' = '2020-04-01 02:00';
SELECT id FROM stz_http_utc WHERE ts + interval '3 months' = '2020-04-01 02:00';
PREPARE prep_iv(interval) AS SELECT id FROM stz_bin_utc WHERE ts + $1 = '2020-04-01 02:00';
EXECUTE prep_iv('3 months');
EXECUTE prep_iv('3 months');
EXECUTE prep_iv('3 months');
EXECUTE prep_iv('3 months');
EXECUTE prep_iv('3 months');
EXECUTE prep_iv('3 months');
DEALLOCATE prep_iv;

-- Verify epoch extraction interprets timestamp wall clock times as UTC.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM stz_bin_wall WHERE date_part('epoch', ts) = 1719835200;
SELECT id FROM stz_bin_wall WHERE date_part('epoch', ts) = 1719835200 ORDER BY id;
SELECT id FROM stz_http_wall WHERE date_part('epoch', ts) = 1719835200 ORDER BY id;
SELECT id FROM stz_bin_utc WHERE extract(epoch FROM ts) = 1577872800;

-- Verify timezone() evaluates locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM stz_bin_utc WHERE date_part('hour', ts AT TIME ZONE 'UTC') = 10;
SELECT id FROM stz_bin_utc WHERE date_part('hour', ts AT TIME ZONE 'UTC') = 10 ORDER BY id;

-- Verify second and epoch extraction preserve microseconds before 1970.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, date_part('second', ts), extract(epoch FROM ts) FROM stz_bin_day
 WHERE date_part('second', ts) > 5 ORDER BY id;
SELECT id, date_part('second', ts), extract(epoch FROM ts) FROM stz_bin_day
 WHERE date_part('second', ts) > 5 ORDER BY id;
SELECT id, date_part('second', ts), extract(epoch FROM ts) FROM stz_http_day
 WHERE date_part('second', ts) > 5 ORDER BY id;

-- Verify date_trunc supports dates before 1970.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM stz_bin_day
 WHERE date_trunc('month', ts) = '1969-12-01' AND date_trunc('hour', ts) = '1969-12-31 15:00';
SELECT id FROM stz_bin_day
 WHERE date_trunc('month', ts) = '1969-12-01' AND date_trunc('hour', ts) = '1969-12-31 15:00';
SELECT id FROM stz_http_day
 WHERE date_trunc('month', ts) = '1969-12-01' AND date_trunc('hour', ts) = '1969-12-31 15:00';

-- Verify Date mapped to timestamp uses midnight and supports day arithmetic.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, d FROM stz_bin_day
 WHERE date_trunc('month', d) = '2020-03-01' AND d + interval '1 day' = '2020-03-08'
   AND date_part('epoch', d) = 1583539200;
SELECT id, d FROM stz_bin_day
 WHERE date_trunc('month', d) = '2020-03-01' AND d + interval '1 day' = '2020-03-08'
   AND date_part('epoch', d) = 1583539200;
SELECT id, d FROM stz_http_day
 WHERE date_trunc('month', d) = '2020-03-01' AND d + interval '1 day' = '2020-03-08'
   AND date_part('epoch', d) = 1583539200;

-- Verify date() uses TimeZone: 10:00 UTC falls on next day at UTC+14.
SET timezone = 'Pacific/Kiritimati';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, date(ts) FROM stz_bin_utc WHERE date(ts) = '2020-01-02';
SELECT id, date(ts) FROM stz_bin_utc WHERE date(ts) = '2020-01-02';
SELECT id, date(ts) FROM stz_http_utc WHERE date(ts) = '2020-01-02';
RESET timezone;

CALL clickhouse_perform('stz_admin', 'DROP DATABASE stz_test');
DROP USER MAPPING FOR CURRENT_USER SERVER stz_bin_svr;
DROP SERVER stz_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER stz_http_svr;
DROP SERVER stz_http_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER stz_admin;
DROP SERVER stz_admin;
