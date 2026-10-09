SET datestyle = 'ISO';
\i test/include/normalize.sql
-- Create servers for each engine.
CREATE SERVER param_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER param_bin_svr;

CREATE SERVER param_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER param_http_svr;

-- Create the schema in ClickHouse.
CREATE SERVER param_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER param_admin;

CALL clickhouse_perform('param_admin', 'DROP DATABASE IF EXISTS param_test');
CALL clickhouse_perform('param_admin', 'CREATE DATABASE param_test');
CALL clickhouse_perform('param_admin', $$
    CREATE TABLE param_test.ft1 (
        c1 Int,
        c2 Int,
        c3 String,
        c4 Date,
        c5 Date,
        c6 String,
        c7 String,
        c8 Array(String)
    ) ENGINE = MergeTree PARTITION BY c4 ORDER BY (c1)
$$);

-- Insert some data.
CALL clickhouse_perform('param_admin', $$
    INSERT INTO param_test.ft1
    SELECT number,
           number % 10,
           toString(number),
           addDays(toDate('1970-01-01'), number % 100),
           addDays(toDate('1970-01-01'), number % 100),
           number % 10,
           number % 10,
           ['foo']
    FROM numbers(1, 110)
$$);

CALL clickhouse_perform('param_admin', $$
    CREATE TABLE param_test.mixed (
        id  Int,
        u   UUID,
        ts  DateTime64(3, 'UTC'),
        lts DateTime64(3),
        d   Date32
    ) ENGINE = MergeTree ORDER BY id
$$);

CALL clickhouse_perform('param_admin', $$
    INSERT INTO param_test.mixed VALUES
        (1, '188dff2f-0205-4912-8210-2214e84482cd', '2026-09-28 23:59:59.500', '2026-09-28 23:59:59.500', '1960-01-01'),
        (2, '00000000-0000-0000-0000-000000000002', '2026-09-28 23:59:59.300', '2026-09-28 23:59:59.300', '2026-09-29')
$$);

CALL clickhouse_perform('param_admin', $$
    CREATE TABLE param_test.typed (id Int, f Float64)
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('param_admin', $$
    INSERT INTO param_test.typed VALUES (1, 1e-10), (2, 0.5), (3, inf), (4, -inf), (5, -0.)
$$);

-- ===================================================================
-- binary foreign tables
-- ===================================================================
CREATE SCHEMA bin_test;

CREATE FOREIGN TABLE bin_test.ft1 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'ft1',
	c8 text[]
) SERVER param_bin_svr OPTIONS(
    database 'param_test',
    table_name 'ft1'
);

CREATE FOREIGN TABLE bin_test.ft2 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'f21',
	c8 text[]
) SERVER param_bin_svr OPTIONS(
    database 'param_test',
    table_name 'ft1'
);

-- ===================================================================
-- binary parameterized queries
-- ===================================================================
-- simple join
PREPARE st1(int, int) AS SELECT t1.c3, t2.c3 FROM bin_test.ft1 t1, bin_test.ft2 t2 WHERE t1.c1 = $1 AND t2.c1 = $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st1(1, 2);

EXECUTE st1(1, 1);
EXECUTE st1(101, 101);
SET enable_hashjoin TO off;
SET enable_sort TO off;
-- subquery using stable function (can't be sent to remote, but is?)
PREPARE st2(int) AS SELECT * FROM bin_test.ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM bin_test.ft2 t2 WHERE c1 > $1 AND date(c4) = date('1970-01-17')) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st2(10, 20);
EXECUTE st2(10, 20);
EXECUTE st2(101, 121);
RESET enable_hashjoin;
RESET enable_sort;

-- subquery using immutable function (can be sent to remote)
PREPARE st3(int) AS SELECT * FROM bin_test.ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM bin_test.ft2 t2 WHERE c1 > $1 AND date(c5) = date('1970-01-17'::timestamptz)) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st3(10, 20);
EXECUTE st3(10, 20);
EXECUTE st3(20, 30);

-- custom plan should be chosen initially
PREPARE st4(int) AS SELECT * FROM bin_test.ft1 t1 WHERE t1.c1 = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
-- once we try it enough times, should switch to generic plan
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
-- value of $1 should not be sent to remote
PREPARE st5(text[], int) AS SELECT * FROM bin_test.ft1 t1 WHERE c8 = $1 and c1 = $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXECUTE st5('{foo}', 1);

-- altering FDW options requires replanning
PREPARE st6 AS SELECT * FROM bin_test.ft1 t1 WHERE t1.c1 = t1.c2 ORDER BY t1.c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st6;
PREPARE st7 AS INSERT INTO bin_test.ft1 (c1,c2,c3) VALUES (1001,101,'foo');
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st7;
CALL clickhouse_perform('param_admin', 'RENAME TABLE param_test.ft1 TO param_test.t1');
ALTER FOREIGN TABLE bin_test.ft1 OPTIONS (SET table_name 't1');
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st6;
EXECUTE st6;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st7;
CALL clickhouse_perform('param_admin', 'RENAME TABLE param_test.t1 TO param_test.ft1');
ALTER FOREIGN TABLE bin_test.ft1 OPTIONS (SET table_name 'ft1');

-- implicit parameter
SELECT * FROM pg_temp.remote_sql($$SELECT c1, c2 FROM bin_test.ft1 WHERE c1 = (SELECT 4)$$);
SELECT c1, c2 FROM bin_test.ft1 WHERE c1 = (SELECT 4);

-- UUID, fractional timestamp, and pre-1970 date parameters
CREATE FOREIGN TABLE bin_test.mixed (
	id int NOT NULL,
	u uuid,
	ts timestamptz,
	lts timestamp,
	d date
) SERVER param_bin_svr OPTIONS(
    database 'param_test',
    table_name 'mixed'
);
SET plan_cache_mode = force_generic_plan;
PREPARE st8(uuid) AS SELECT id FROM bin_test.mixed WHERE u = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st8('188dff2f-0205-4912-8210-2214e84482cd');
EXECUTE st8('188dff2f-0205-4912-8210-2214e84482cd');
PREPARE st9(timestamptz) AS SELECT id FROM bin_test.mixed WHERE ts <= $1 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st9('2026-09-28 23:59:59.999999+00');
EXECUTE st9('2026-09-28 23:59:59.999999+00');
EXECUTE st9('2026-09-28 23:59:59.4+00');
PREPARE st10(timestamp) AS SELECT id FROM bin_test.mixed WHERE lts <= $1 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st10('2026-09-28 23:59:59.999999');
EXECUTE st10('2026-09-28 23:59:59.999999');
EXECUTE st10('2026-09-28 23:59:59.4');
PREPARE st11(date) AS SELECT id FROM bin_test.mixed WHERE d = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st11('1960-01-01');
EXECUTE st11('1960-01-01');
PREPARE st12(interval) AS SELECT id FROM bin_test.mixed WHERE ts + $1 >= '2026-10-29 00:00:00+00' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st12('1 month 0.5 seconds');
EXECUTE st12('1 month 0.5 seconds');
EXECUTE st12('1 month 1 day');
PREPARE st13(interval) AS SELECT id FROM bin_test.mixed WHERE d - $1 >= '1959-12-31' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st13('1 day 1 microsecond');
EXECUTE st13('1 day 1 microsecond');
EXECUTE st13('1 day');
EXECUTE st13('-1 year');

CREATE FOREIGN TABLE bin_test.typed (
	id int NOT NULL,
	f float8
) SERVER param_bin_svr OPTIONS(
    database 'param_test',
    table_name 'typed'
);
PREPARE st14(float8) AS SELECT id FROM bin_test.typed WHERE f = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st14(1e-10);
EXECUTE st14(1e-10);
EXECUTE st14('Infinity');
EXECUTE st14('-Infinity');
EXECUTE st14('-0');
PREPARE st15(float8) AS SELECT id FROM bin_test.typed WHERE f > $1 ORDER BY id;
EXECUTE st15('-Infinity');
EXECUTE st15('-0');
EXECUTE st15(0.25);
PREPARE st16(float8) AS SELECT id FROM bin_test.typed WHERE f <> $1 ORDER BY id;
EXECUTE st16('NaN');
RESET plan_cache_mode;

-- cleanup
DEALLOCATE st1;
DEALLOCATE st2;
DEALLOCATE st3;
DEALLOCATE st4;
DEALLOCATE st5;
DEALLOCATE st6;
DEALLOCATE st7;
DEALLOCATE st8;
DEALLOCATE st9;
DEALLOCATE st10;
DEALLOCATE st11;
DEALLOCATE st12;
DEALLOCATE st13;
DEALLOCATE st14;
DEALLOCATE st15;
DEALLOCATE st16;

-- ===================================================================
-- http foreign tables
-- ===================================================================
CREATE SCHEMA http_test;

CREATE FOREIGN TABLE http_test.ft1 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'ft1',
	c8 text[]
) SERVER param_http_svr OPTIONS(
    database 'param_test',
    table_name 'ft1'
);

CREATE FOREIGN TABLE http_test.ft2 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 timestamptz,
	c5 timestamp,
	c6 varchar(10),
	c7 char(10) default 'f21',
	c8 text[]
) SERVER param_http_svr OPTIONS(
    database 'param_test',
    table_name 'ft1'
);

-- ===================================================================
-- http parameterized queries
-- ===================================================================
-- simple join
PREPARE st1(int, int) AS SELECT t1.c3, t2.c3 FROM http_test.ft1 t1, http_test.ft2 t2 WHERE t1.c1 = $1 AND t2.c1 = $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st1(1, 2);

EXECUTE st1(1, 1);
EXECUTE st1(101, 101);
SET enable_hashjoin TO off;
SET enable_sort TO off;
-- subquery using stable function (can't be sent to remote, but is?)
PREPARE st2(int) AS SELECT * FROM http_test.ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM http_test.ft2 t2 WHERE c1 > $1 AND date(c4) = date('1970-01-17')) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st2(10, 20);
EXECUTE st2(10, 20);
EXECUTE st2(101, 121);
RESET enable_hashjoin;
RESET enable_sort;

-- subquery using immutable function (can be sent to remote)
PREPARE st3(int) AS SELECT * FROM http_test.ft1 t1 WHERE t1.c1 < $2 AND t1.c3 IN (SELECT c3 FROM http_test.ft2 t2 WHERE c1 > $1 AND date(c5) = date('1970-01-17'::timestamptz)) ORDER BY c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st3(10, 20);
EXECUTE st3(10, 20);
EXECUTE st3(20, 30);

-- custom plan should be chosen initially
PREPARE st4(int) AS SELECT * FROM http_test.ft1 t1 WHERE t1.c1 = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
-- once we try it enough times, should switch to generic plan
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st4(1);
-- value of $1 should not be sent to remote
PREPARE st5(text[], int) AS SELECT * FROM http_test.ft1 t1 WHERE c8 = $1 and c1 = $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st5('{foo}', 1);
EXECUTE st5('{foo}', 1);

-- altering FDW options requires replanning
PREPARE st6 AS SELECT * FROM http_test.ft1 t1 WHERE t1.c1 = t1.c2 ORDER BY t1.c1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st6;
PREPARE st7 AS INSERT INTO http_test.ft1 (c1,c2,c3) VALUES (1001,101,'foo');
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st7;
CALL clickhouse_perform('param_admin', 'RENAME TABLE param_test.ft1 TO param_test.t1');
ALTER FOREIGN TABLE http_test.ft1 OPTIONS (SET table_name 't1');
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st6;
EXECUTE st6;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st7;
CALL clickhouse_perform('param_admin', 'RENAME TABLE param_test.t1 TO param_test.ft1');
ALTER FOREIGN TABLE http_test.ft1 OPTIONS (SET table_name 'ft1');

-- implicit parameter
SELECT * FROM pg_temp.remote_sql($$SELECT c1, c2 FROM http_test.ft1 WHERE c1 = (SELECT 4)$$);
SELECT c1, c2 FROM http_test.ft1 WHERE c1 = (SELECT 4);

-- UUID, fractional timestamp, and pre-1970 date parameters
CREATE FOREIGN TABLE http_test.mixed (
	id int NOT NULL,
	u uuid,
	ts timestamptz,
	lts timestamp,
	d date
) SERVER param_http_svr OPTIONS(
    database 'param_test',
    table_name 'mixed'
);
SET plan_cache_mode = force_generic_plan;
PREPARE st8(uuid) AS SELECT id FROM http_test.mixed WHERE u = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st8('188dff2f-0205-4912-8210-2214e84482cd');
EXECUTE st8('188dff2f-0205-4912-8210-2214e84482cd');
PREPARE st9(timestamptz) AS SELECT id FROM http_test.mixed WHERE ts <= $1 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st9('2026-09-28 23:59:59.999999+00');
EXECUTE st9('2026-09-28 23:59:59.999999+00');
EXECUTE st9('2026-09-28 23:59:59.4+00');
PREPARE st10(timestamp) AS SELECT id FROM http_test.mixed WHERE lts <= $1 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st10('2026-09-28 23:59:59.999999');
EXECUTE st10('2026-09-28 23:59:59.999999');
EXECUTE st10('2026-09-28 23:59:59.4');
PREPARE st11(date) AS SELECT id FROM http_test.mixed WHERE d = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st11('1960-01-01');
EXECUTE st11('1960-01-01');
PREPARE st12(interval) AS SELECT id FROM http_test.mixed WHERE ts + $1 >= '2026-10-29 00:00:00+00' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st12('1 month 0.5 seconds');
EXECUTE st12('1 month 0.5 seconds');
EXECUTE st12('1 month 1 day');
PREPARE st13(interval) AS SELECT id FROM http_test.mixed WHERE d - $1 >= '1959-12-31' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st13('1 day 1 microsecond');
EXECUTE st13('1 day 1 microsecond');
EXECUTE st13('1 day');
EXECUTE st13('-1 year');

CREATE FOREIGN TABLE http_test.typed (
	id int NOT NULL,
	f float8
) SERVER param_http_svr OPTIONS(
    database 'param_test',
    table_name 'typed'
);
PREPARE st14(float8) AS SELECT id FROM http_test.typed WHERE f = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE st14(1e-10);
EXECUTE st14(1e-10);
EXECUTE st14('Infinity');
EXECUTE st14('-Infinity');
EXECUTE st14('-0');
PREPARE st15(float8) AS SELECT id FROM http_test.typed WHERE f > $1 ORDER BY id;
EXECUTE st15('-Infinity');
EXECUTE st15('-0');
EXECUTE st15(0.25);
PREPARE st16(float8) AS SELECT id FROM http_test.typed WHERE f <> $1 ORDER BY id;
EXECUTE st16('NaN');

-- ===================================================================
-- parameter literals per type, generic plans keep them as Params
-- ===================================================================
CALL clickhouse_perform('param_admin', $$
    CREATE TABLE param_test.lit (
        id Int32, i8 Int64, f4 Float32, f8 Float64, s String, by String,
        d Date, ts DateTime64(6), arr Array(String)
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('param_admin', $$
    INSERT INTO param_test.lit VALUES
    (1, 9000000000, 1.5, 2.25, 'q''uote\\back\b\f\r\n\t\a\v', 'by\0te',
     '2024-02-29', '2024-02-29 12:34:56', ['t\tb', 'n\nl', 'q''x', 'b\\s', 'bs\b'])
$$);
CREATE FOREIGN TABLE bin_test.lit (
    id int, i8 bigint, f4 real, f8 float8, s text, by bytea,
    d date, ts timestamp, arr text[]
) SERVER param_bin_svr OPTIONS (database 'param_test', table_name 'lit');
CREATE FOREIGN TABLE http_test.lit (
    id int, i8 bigint, f4 real, f8 float8, s text, by bytea,
    d date, ts timestamp, arr text[]
) SERVER param_http_svr OPTIONS (database 'param_test', table_name 'lit');
SET plan_cache_mode = force_generic_plan;
PREPARE lit_bin(bigint, real, float8, text, bytea, date, timestamp, text[]) AS
SELECT id FROM bin_test.lit
 WHERE i8 = $1 AND f4 = $2 AND f8 = $3 AND s = $4 AND by = $5
   AND d = $6 AND ts = $7 AND arr = $8;
PREPARE lit_http(bigint, real, float8, text, bytea, date, timestamp, text[]) AS
SELECT id FROM http_test.lit
 WHERE i8 = $1 AND f4 = $2 AND f8 = $3 AND s = $4 AND by = $5
   AND d = $6 AND ts = $7 AND arr = $8;
EXPLAIN (VERBOSE, COSTS OFF)
EXECUTE lit_bin(9000000000, 1.5, 2.25, E'q''uote\\back\b\f\r\n\t\x07\x0b', '\x6279007465',
                '2024-02-29', '2024-02-29 12:34:56',
                ARRAY[E't\tb', E'n\nl', 'q''x', E'b\\s', E'bs\b']);
EXECUTE lit_bin(9000000000, 1.5, 2.25, E'q''uote\\back\b\f\r\n\t\x07\x0b', '\x6279007465',
                '2024-02-29', '2024-02-29 12:34:56',
                ARRAY[E't\tb', E'n\nl', 'q''x', E'b\\s', E'bs\b']);
EXPLAIN (VERBOSE, COSTS OFF)
EXECUTE lit_http(9000000000, 1.5, 2.25, E'q''uote\\back\b\f\r\n\t\x07\x0b', '\x6279007465',
                 '2024-02-29', '2024-02-29 12:34:56',
                 ARRAY[E't\tb', E'n\nl', 'q''x', E'b\\s', E'bs\b']);
EXECUTE lit_http(9000000000, 1.5, 2.25, E'q''uote\\back\b\f\r\n\t\x07\x0b', '\x6279007465',
                 '2024-02-29', '2024-02-29 12:34:56',
                 ARRAY[E't\tb', E'n\nl', 'q''x', E'b\\s', E'bs\b']);
EXECUTE lit_bin(9000000000, 1.5, 2.25, NULL, '\x6279007465',
                '2024-02-29', '2024-02-29 12:34:56',
                ARRAY[E't\tb', E'n\nl', 'q''x', E'b\\s', E'bs\b']);
DEALLOCATE lit_bin;
DEALLOCATE lit_http;
RESET plan_cache_mode;

-- cleanup
DEALLOCATE st1;
DEALLOCATE st2;
DEALLOCATE st3;
DEALLOCATE st4;
DEALLOCATE st5;
DEALLOCATE st6;
DEALLOCATE st7;
DEALLOCATE st8;
DEALLOCATE st9;
DEALLOCATE st10;
DEALLOCATE st11;
DEALLOCATE st12;
DEALLOCATE st13;
DEALLOCATE st14;
DEALLOCATE st15;
DEALLOCATE st16;

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER param_bin_svr;
DROP SERVER param_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER param_http_svr;
DROP SERVER param_http_svr CASCADE;
