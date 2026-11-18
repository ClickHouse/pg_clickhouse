SET datestyle = 'ISO';
SET timezone = 'America/New_York';

CREATE SERVER date_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'date_bin_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER date_bin_svr;

CREATE SERVER date_bin_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER date_bin_admin;

CALL clickhouse_perform('date_bin_admin', 'DROP DATABASE IF EXISTS date_bin_test');
CALL clickhouse_perform('date_bin_admin', 'CREATE DATABASE date_bin_test');
CALL clickhouse_perform('date_bin_admin', $$
    CREATE TABLE date_bin_test.events (
        id   Int32,
        ts   DateTime('UTC'),
        ts64 DateTime64(6, 'Asia/Kolkata'),
        nts  Nullable(DateTime('UTC')),
        d    Date,
        val  Int32
    ) ENGINE = MergeTree ORDER BY id
$$);

CREATE FOREIGN TABLE date_bin_events (
    id   int,
    ts   timestamptz,
    ts64 timestamptz,
    nts  timestamptz,
    d    date,
    val  int
) SERVER date_bin_svr OPTIONS (table_name 'events');

-- Rows straddle a bucket boundary, both DST changes, and the Unix epoch.
INSERT INTO date_bin_events VALUES
    (1, '2024-03-10 06:59:59+00', '2024-03-10 06:59:59.999999+00', '2024-03-10 06:59:59+00', '2024-03-10', 1),
    (2, '2024-03-10 07:00:00+00', '2024-03-10 07:00:00+00',        NULL,                     '2024-03-10', 2),
    (3, '2024-03-10 07:17:42+00', '2024-03-10 07:17:42.5+00',      '2024-03-10 07:17:42+00', '2024-03-11', 3),
    (4, '2024-11-03 05:30:00+00', '2024-11-03 05:30:00.000001+00', '2024-11-03 05:30:00+00', '2024-11-03', 4),
    (5, '2024-11-03 06:30:00+00', '2024-11-03 06:30:00+00',        NULL,                     '2024-11-04', 5),
    (6, '1970-01-01 00:00:00+00', '1969-12-31 23:17:01.5+00',      '1970-01-01 00:00:00+00', '1970-01-01', 6),
    (7, '2000-01-01 00:00:00+00', '1900-01-01 00:00:00+00',        '2000-01-01 00:00:00+00', '2000-01-01', 7),
    (8, '2105-12-31 23:59:59+00', '2262-01-01 12:34:56.789012+00', '2105-12-31 23:59:59+00', '2100-06-15', 8);

-- Postgres evaluates date_bin() on a local copy of the rows for comparison.
CREATE TABLE date_bin_local AS SELECT * FROM date_bin_events;

-- A stride that divides the offset between origin and the Unix epoch needs
-- no shift.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('15 minutes', ts, '2001-01-01 00:00:00+00') AS b, count(*)
  FROM date_bin_events GROUP BY b ORDER BY b;
SELECT date_bin('15 minutes', ts, '2001-01-01 00:00:00+00') AS b, count(*)
  FROM date_bin_events GROUP BY b ORDER BY b;

-- Any other origin shifts the buckets.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('15 minutes', ts, '2001-01-01 00:02:30+00') AS b, sum(val)
  FROM date_bin_events GROUP BY b ORDER BY b;
SELECT date_bin('15 minutes', ts, '2001-01-01 00:02:30+00') AS b, sum(val)
  FROM date_bin_events GROUP BY b ORDER BY b;

-- Pushes down in WHERE and ORDER BY.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM date_bin_events
 WHERE date_bin('1 hour', ts, '2001-01-01') = '2024-03-10 02:00:00-05'
 ORDER BY date_bin('1 day', ts64, '2001-01-01'), id;
SELECT id FROM date_bin_events
 WHERE date_bin('1 hour', ts, '2001-01-01') = '2024-03-10 02:00:00-05'
 ORDER BY date_bin('1 day', ts64, '2001-01-01'), id;

-- Every stride and origin must match Postgres for every row and column:
-- strides that do not divide a day, day strides across DST changes, sub-second
-- strides, and origins after the data and before the Unix epoch. Grouping
-- pushes the remote date_bin() down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, date_bin('5 hours', ts64, '2300-05-05 05:05:05.5+00') AS b
  FROM date_bin_events GROUP BY id, b;

-- A date casts to midnight in TimeZone, not in the ClickHouse time zone.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('7 days', d, '2001-01-01') AS b FROM date_bin_events GROUP BY b;

CREATE FUNCTION date_bin_diff(stride interval, origin timestamptz)
RETURNS TABLE (id int, col text, remote timestamptz, local timestamptz)
LANGUAGE plpgsql AS $$
DECLARE
    c text;
BEGIN
    FOREACH c IN ARRAY ARRAY['ts', 'ts64', 'nts', 'd'] LOOP
        RETURN QUERY EXECUTE format($q$
            SELECT r.id, %L, r.b, l.b
              FROM (
                  SELECT id, date_bin(%L, %I, %L) AS b FROM date_bin_events GROUP BY id, b
              ) r
              JOIN (SELECT id, date_bin(%L, %I, %L) AS b FROM date_bin_local) l USING (id)
             WHERE r.b IS DISTINCT FROM l.b
        $q$, c, stride, c, origin, stride, c, origin);
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
        ('2001-01-01 00:00:00+00'::timestamptz), ('2001-01-01 00:02:30+00'),
        ('2000-01-03 00:00:00+00'), ('1970-01-01 00:00:00+00'),
        ('1969-07-20 20:17:40.123456+00'), ('2300-05-05 05:05:05.5+00'),
        ('2001-01-01 00:00:00')
       ) AS o(origin),
       date_bin_diff(s.stride, o.origin) AS d
 ORDER BY 1, 2, 3, 4;

-- The buckets themselves.
SELECT id,
       date_bin('5 hours', ts, '2001-01-01 00:00:00+00') AS ts_5h,
       date_bin('00:00:00.25', ts64, '2001-01-01 00:00:00+00') AS ts64_250ms,
       date_bin('1 day', nts, '2001-01-01') AS nts_day,
       date_bin('7 days', d, '2001-01-01') AS d_week
  FROM date_bin_events GROUP BY 1, 2, 3, 4, 5 ORDER BY id;

-- A stride with months, a stride that is not positive, a stride or origin
-- that is not constant, and an infinite origin all evaluate in Postgres,
-- which raises errors for the invalid ones.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('1 month', ts, '2001-01-01') AS b FROM date_bin_events GROUP BY b;
SELECT date_bin('1 month', ts, '2001-01-01') AS b FROM date_bin_events GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('-1 hour', ts, '2001-01-01') AS b FROM date_bin_events GROUP BY b;
SELECT date_bin('-1 hour', ts, '2001-01-01') AS b FROM date_bin_events GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('0 seconds', ts, '2001-01-01') AS b FROM date_bin_events GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin(make_interval(mins => val), ts, '2001-01-01') AS b FROM date_bin_events GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('1 hour', ts, ts64) AS b FROM date_bin_events GROUP BY b;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('1 hour', ts, 'infinity') AS b FROM date_bin_events GROUP BY b;

-- date_bin(interval, timestamp, timestamp) bins wall clock time and
-- evaluates in Postgres.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT date_bin('1 hour', ts::timestamp, '2001-01-01') AS b FROM date_bin_events GROUP BY b;

DROP FUNCTION date_bin_diff;
DROP TABLE date_bin_local;
DROP FOREIGN TABLE date_bin_events;
CALL clickhouse_perform('date_bin_admin', 'DROP DATABASE date_bin_test');
DROP USER MAPPING FOR CURRENT_USER SERVER date_bin_svr;
DROP SERVER date_bin_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER date_bin_admin;
DROP SERVER date_bin_admin;
