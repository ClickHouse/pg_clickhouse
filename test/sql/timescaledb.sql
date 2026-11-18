SELECT NOT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 'timescaledb')
    OR current_setting('shared_preload_libraries') !~ 'timescaledb' AS no_timescaledb \gset
\if :no_timescaledb
\echo 'SKIP: timescaledb extension not available'
\quit
\endif

-- Push down TimescaleDB time_bucket(), first(), and last(), comparing results
-- with TimescaleDB on a local copy of the same rows.
-- Concurrent tests may create timescaledb; serialize and silence its banner
SET client_min_messages = error;
SELECT pg_advisory_lock(hashtext('timescaledb'));
CREATE EXTENSION IF NOT EXISTS timescaledb;
SELECT pg_advisory_unlock(hashtext('timescaledb'));
RESET client_min_messages;

SET datestyle = 'ISO';
SET timezone = 'America/New_York';

CREATE SERVER timescaledb_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'timescaledb_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER timescaledb_svr;

CREATE SERVER timescaledb_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER timescaledb_admin;

CALL clickhouse_perform('timescaledb_admin', 'DROP DATABASE IF EXISTS timescaledb_test');
CALL clickhouse_perform('timescaledb_admin', 'CREATE DATABASE timescaledb_test');
CALL clickhouse_perform('timescaledb_admin', $$
    CREATE TABLE timescaledb_test.metrics (
        id     Int32,
        device Int32,
        ts     DateTime('UTC'),
        ts64   DateTime64(6, 'Asia/Kolkata'),
        nts    Nullable(DateTime('UTC')),
        val    Nullable(Float64),
        n      Int32
    ) ENGINE = MergeTree ORDER BY id
$$);

CREATE FOREIGN TABLE ts_metrics (
    id     int,
    device int,
    ts     timestamptz,
    ts64   timestamptz,
    nts    timestamptz,
    val    float8,
    n      int
) SERVER timescaledb_svr OPTIONS (table_name 'metrics');

-- Rows straddle a bucket boundary, both DST changes, and the Unix epoch. NULL
-- values fall on the first and last rows of some groups.
INSERT INTO ts_metrics VALUES
    (1, 1, '2024-03-10 06:59:59+00', '2024-03-10 06:59:59.999999+00', '2024-03-10 06:59:59+00', NULL, 1),
    (2, 1, '2024-03-10 07:00:00+00', '2024-03-10 07:00:00+00',        NULL,                     2.5,  2),
    (3, 1, '2024-03-10 07:17:42+00', '2024-03-10 07:17:42.5+00',      '2024-03-10 07:17:42+00', NULL, 3),
    (4, 2, '2024-11-03 05:30:00+00', '2024-11-03 05:30:00.000001+00', '2024-11-03 05:30:00+00', 4.5,  4),
    (5, 2, '2024-11-03 06:30:00+00', '2024-11-03 06:30:00+00',        NULL,                     5.5,  5),
    (6, 3, '1970-01-01 00:00:00+00', '1969-12-31 23:17:01.5+00',      '1970-01-01 00:00:00+00', 6.5,  6),
    (7, 3, '2000-01-01 00:00:00+00', '1900-01-01 00:00:00+00',        '2000-01-01 00:00:00+00', 7.5,  7),
    (8, 4, '2105-12-31 23:59:59+00', '2262-01-01 12:34:56.789012+00', NULL,                     8.5,  8);

-- TimescaleDB evaluates its functions on a local copy of the rows for
-- comparison.
CREATE TABLE ts_local AS SELECT * FROM ts_metrics;

-- A stride that divides the offset between the default origin, Monday,
-- 2000-01-03 UTC, and the Unix epoch needs no shift.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('15 minutes', ts) AS b, count(*)
  FROM ts_metrics GROUP BY b ORDER BY b;
SELECT time_bucket('15 minutes', ts) AS b, count(*)
  FROM ts_metrics GROUP BY b ORDER BY b;

-- Weeks start on Monday, not on the Thursday of the Unix epoch.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('7 days', ts) AS b, sum(n)
  FROM ts_metrics GROUP BY b ORDER BY b;
SELECT time_bucket('7 days', ts) AS b, sum(n)
  FROM ts_metrics GROUP BY b ORDER BY b;

-- An explicit origin.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 hour', ts, '2001-01-01 00:02:30+00'::timestamptz) AS b, sum(n)
  FROM ts_metrics GROUP BY b ORDER BY b;
SELECT time_bucket('1 hour', ts, '2001-01-01 00:02:30+00'::timestamptz) AS b, sum(n)
  FROM ts_metrics GROUP BY b ORDER BY b;

-- Pushes down in WHERE.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM ts_metrics
 WHERE time_bucket('1 hour', ts) = '2024-03-10 02:00:00-05' ORDER BY id;
SELECT id FROM ts_metrics
 WHERE time_bucket('1 hour', ts) = '2024-03-10 02:00:00-05' ORDER BY id;

-- Every stride and origin must match TimescaleDB for every row and column.
-- Grouping pushes the remote time_bucket() down.
CREATE FUNCTION ts_bucket_diff(stride interval, origin timestamptz)
RETURNS TABLE (id int, col text, remote timestamptz, local timestamptz)
LANGUAGE plpgsql AS $$
DECLARE
    c    text;
    call text;
BEGIN
    FOREACH c IN ARRAY ARRAY['ts', 'ts64', 'nts'] LOOP
        call := CASE WHEN origin IS NULL
            THEN format('time_bucket(%L, %I)', stride, c)
            ELSE format('time_bucket(%L, %I, %L::timestamptz)', stride, c, origin)
        END;
        RETURN QUERY EXECUTE format($q$
            SELECT r.id, %L, r.b, l.b
              FROM (SELECT id, %s AS b FROM ts_metrics GROUP BY id, b) r
              JOIN (SELECT id, %s AS b FROM ts_local) l USING (id)
             WHERE r.b IS DISTINCT FROM l.b
        $q$, c, call, call);
    END LOOP;
END;
$$;

SELECT s.stride, o.origin, d.*
  FROM (VALUES
        ('1 second'::interval), ('7 seconds'), ('5 minutes'), ('15 minutes'),
        ('1 hour'), ('5 hours'), ('1 day'), ('7 days'), ('1 day 02:30:00'),
        ('00:00:00.25'), ('00:00:00.000007'), ('30 days'), ('10000 days')
       ) AS s(stride),
       (VALUES
        (NULL::timestamptz), ('2001-01-01 00:02:30+00'),
        ('1970-01-01 00:00:00+00'), ('1969-07-20 20:17:40.123456+00'),
        ('2300-05-05 05:05:05.5+00')
       ) AS o(origin),
       ts_bucket_diff(s.stride, o.origin) AS d
 ORDER BY 1, 2, 3, 4;

-- first() and last() keep a NULL value on the first or last row, and ignore
-- rows with a NULL time.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT device, first(val, ts), last(val, ts), first(n, nts), last(n, nts)
  FROM ts_metrics GROUP BY device ORDER BY device;
SELECT device, first(val, ts), last(val, ts), first(n, nts), last(n, nts)
  FROM ts_metrics GROUP BY device ORDER BY device;
SELECT device, first(val, ts), last(val, ts), first(n, nts), last(n, nts)
  FROM ts_local GROUP BY device ORDER BY device;

-- With time_bucket() and FILTER.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 day', ts) AS b, first(n, ts), last(val, ts) FILTER (WHERE n > 1)
  FROM ts_metrics GROUP BY b ORDER BY b;
SELECT time_bucket('1 day', ts) AS b, first(n, ts), last(val, ts) FILTER (WHERE n > 1)
  FROM ts_metrics GROUP BY b ORDER BY b;
SELECT time_bucket('1 day', ts) AS b, first(n, ts), last(val, ts) FILTER (WHERE n > 1)
  FROM ts_local GROUP BY b ORDER BY b;

-- Without rows or groups they return NULL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT first(n, ts), last(n, ts) FROM ts_metrics WHERE id > 100;
SELECT first(n, ts), last(n, ts) FROM ts_metrics WHERE id > 100;

-- A value orders the rows as well as a time.
SELECT first(id, val), last(id, val) FROM ts_metrics;
SELECT first(id, val), last(id, val) FROM ts_local;

-- A stride with months, a stride that is not positive, and a stride or
-- origin that is not constant all evaluate in Postgres.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 month', ts) AS b FROM ts_metrics GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('-1 hour', ts) AS b FROM ts_metrics GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket(make_interval(mins => n), ts) AS b FROM ts_metrics GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 hour', ts, ts64) AS b FROM ts_metrics GROUP BY b;

-- So do the variants that bin wall clock time and integers, or take an
-- offset or a time zone.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 hour', ts::timestamp) AS b FROM ts_metrics GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket(5, n) AS b FROM ts_metrics GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 hour', ts, '10 minutes'::interval) AS b FROM ts_metrics GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT time_bucket('1 day', ts, 'Europe/Berlin') AS b FROM ts_metrics GROUP BY b;

DROP FUNCTION ts_bucket_diff;
DROP TABLE ts_local;
DROP FOREIGN TABLE ts_metrics;
CALL clickhouse_perform('timescaledb_admin', 'DROP DATABASE timescaledb_test');
DROP USER MAPPING FOR CURRENT_USER SERVER timescaledb_svr;
DROP SERVER timescaledb_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER timescaledb_admin;
DROP SERVER timescaledb_admin;
