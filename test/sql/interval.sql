SET intervalstyle = 'postgres';
CREATE SERVER binary_interval_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'interval_test', driver 'binary');
CREATE SERVER http_interval_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'interval_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_interval_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_interval_loopback;

CREATE SERVER interval_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER interval_admin;

\set ECHO errors
SELECT clickhouse_server_version('interval_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (23, 5) AS no_ch235 \gset
\if :no_ch235
\echo 'SKIP: Interval columns unsupported prior to ClickHouse 23.5'
\quit
\endif
\set ECHO all

CALL clickhouse_perform('interval_admin', 'DROP DATABASE IF EXISTS interval_test');
CALL clickhouse_perform('interval_admin', 'CREATE DATABASE interval_test');

CALL clickhouse_perform('interval_admin', format($$
    CREATE TABLE interval_test.intervals (
        id       Int32                NOT NULL,
        base     DateTime64(6, 'UTC') NOT NULL,
        nanos    IntervalNanosecond   NOT NULL,
        micros   IntervalMicrosecond  NOT NULL,
        millis   IntervalMillisecond  NOT NULL,
        seconds  IntervalSecond       NOT NULL,
        minutes  IntervalMinute       NOT NULL,
        hours    IntervalHour         NOT NULL,
        days     IntervalDay          NOT NULL,
        weeks    IntervalWeek         NOT NULL,
        months   IntervalMonth        NOT NULL,
        quarters IntervalQuarter      NOT NULL,
        years    IntervalYear         NOT NULL
    ) ENGINE = MergeTree ORDER BY (id);
$$));

-- Start with the imported format using the interval type.
CREATE SCHEMA ival_bin;
CREATE SCHEMA ival_http;
IMPORT FOREIGN SCHEMA "interval_test" FROM SERVER binary_interval_loopback INTO ival_bin;
IMPORT FOREIGN SCHEMA "interval_test" FROM SERVER http_interval_loopback INTO ival_http;

SELECT attname, format_type(atttypid, atttypmod) AS type
  FROM pg_attribute
 WHERE attrelid = 'ival_bin.intervals'::regclass AND attnum > 0
 ORDER BY attnum;

SELECT attname, format_type(atttypid, atttypmod) AS type
  FROM pg_attribute
 WHERE attrelid = 'ival_http.intervals'::regclass AND attnum > 0
 ORDER BY attnum;

-- Insert values.
INSERT INTO ival_bin.intervals
VALUES ( 1, '2026-09-01 00:00:00Z', '42 microsecond', '42 microsecond', '42 ms', '42 s', '42 m', '42 h', '42 d', '42 w', '42 mon', '168 mon', '42 y')
     , ( 2, '2026-09-02 00:00:00Z', '21 microsecond', '21 microsecond', '21 ms', '21 s', '21 m', '21 h', '21 d', '21 w', '21 mon', '84 mon',  '21 y')
;

INSERT INTO ival_http.intervals
VALUES ( 3, '2026-09-03 00:00:00Z', '21 microsecond', '21 microsecond', '21 ms', '21 s', '21 m', '21 h', '21 d', '21 w', '21 mon', '84 mon',  '21 y')
     , ( 4, '2026-09-04 00:00:00Z', '33 microsecond', '33 microsecond', '33 ms', '33 s', '33 m', '33 h', '33 d', '33 w', '33 mon', '132 mon', '33 y')
;

-- They should all be there.
SELECT * FROM ival_bin.intervals ORDER BY id;
SELECT * FROM ival_http.intervals ORDER BY id;

-- Test operator pushdown.
EXPLAIN (verbose, COSTS OFF)
SELECT id, base + nanos
  FROM ival_bin.intervals
 WHERE base + nanos > base
 ORDER BY id;

SELECT id, base + nanos
  FROM ival_bin.intervals
 WHERE base + nanos > base
 ORDER BY id;

EXPLAIN (verbose, COSTS OFF)
 SELECT id, base - nanos
   FROM ival_bin.intervals
  WHERE base - nanos < base
  ORDER BY id;

 SELECT id, base - nanos
   FROM ival_bin.intervals
  WHERE base - nanos < base
  ORDER BY id;

-- Now try BIGINTS.
CREATE FOREIGN TABLE ival_bin.int_intervals (
    id       INT         NOT NULL,
    base     TIMESTAMPTZ NOT NULL,
    nanos    BIGINT      NOT NULL,
    micros   BIGINT      NOT NULL,
    millis   BIGINT      NOT NULL,
    seconds  BIGINT      NOT NULL,
    minutes  BIGINT      NOT NULL,
    hours    BIGINT      NOT NULL,
    days     BIGINT      NOT NULL,
    weeks    BIGINT      NOT NULL,
    months   BIGINT      NOT NULL,
    quarters BIGINT      NOT NULL,
    years    BIGINT      NOT NULL
) SERVER binary_interval_loopback OPTIONS (table_name 'intervals');

CREATE FOREIGN TABLE ival_http.int_intervals (
    id       INT         NOT NULL,
    base     TIMESTAMPTZ NOT NULL,
    nanos    BIGINT      NOT NULL,
    micros   BIGINT      NOT NULL,
    millis   BIGINT      NOT NULL,
    seconds  BIGINT      NOT NULL,
    minutes  BIGINT      NOT NULL,
    hours    BIGINT      NOT NULL,
    days     BIGINT      NOT NULL,
    weeks    BIGINT      NOT NULL,
    months   BIGINT      NOT NULL,
    quarters BIGINT      NOT NULL,
    years    BIGINT      NOT NULL
) SERVER http_interval_loopback OPTIONS (table_name 'intervals');

-- Insert data.
CALL clickhouse_perform('interval_admin', 'TRUNCATE interval_test.intervals');

INSERT INTO ival_bin.int_intervals
VALUES ( 1, '2026-09-01 00:00:00Z', 42, 42, 42, 42, 42, 42, 42, 42, 42, 42, 42)
     , ( 2, '2026-09-02 00:00:00Z', 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21)
;

-- Fails because http driver doesn't know the remote interval subtype. Would
-- need to either `DESCRIBE` the table in advance, or perhaps store the types
-- in a column option. Probably not worth it given that the binary driver
-- works fine and is preferred.
INSERT INTO ival_http.int_intervals
VALUES ( 3, '2026-09-03 00:00:00Z', 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21)
     , ( 4, '2026-09-04 00:00:00Z', 33, 33, 33, 33, 33, 33, 33, 33, 33, 33, 33)
;

SELECT * FROM ival_bin.int_intervals ORDER BY id;
SELECT * FROM ival_http.int_intervals ORDER BY id;

CALL clickhouse_perform('interval_admin', 'DROP DATABASE interval_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_interval_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_interval_loopback;
DROP SERVER binary_interval_loopback CASCADE;
DROP SERVER http_interval_loopback CASCADE;
