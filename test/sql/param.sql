SET datestyle = 'ISO';
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
EXPLAIN (VERBOSE, COSTS OFF) SELECT c1, c2 FROM bin_test.ft1 WHERE c1 = (SELECT 4);
SELECT c1, c2 FROM bin_test.ft1 WHERE c1 = (SELECT 4);

-- cleanup
DEALLOCATE st1;
DEALLOCATE st2;
DEALLOCATE st3;
DEALLOCATE st4;
DEALLOCATE st5;
DEALLOCATE st6;
DEALLOCATE st7;

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
EXPLAIN (VERBOSE, COSTS OFF) SELECT c1, c2 FROM http_test.ft1 WHERE c1 = (SELECT 4);
SELECT c1, c2 FROM http_test.ft1 WHERE c1 = (SELECT 4);

-- cleanup
DEALLOCATE st1;
DEALLOCATE st2;
DEALLOCATE st3;
DEALLOCATE st4;
DEALLOCATE st5;
DEALLOCATE st6;
DEALLOCATE st7;

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER param_bin_svr;
DROP SERVER param_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER param_http_svr;
DROP SERVER param_http_svr CASCADE;
