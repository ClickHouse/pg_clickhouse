SET datestyle = 'ISO';
CREATE SERVER binary_queries_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'binary_queries_test', driver 'binary');
CREATE SERVER binary_queries_loopback2 FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'binary_queries_test', driver 'binary');

CREATE USER MAPPING FOR CURRENT_USER SERVER binary_queries_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_queries_loopback2;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS binary_queries_test');
SELECT clickhouse_raw_query('CREATE DATABASE binary_queries_test');
SELECT clickhouse_raw_query('CREATE TABLE binary_queries_test.t1
	(c1 Int, c2 Int, c3 String, c4 Date, c5 Date, c6 String, c7 String, c8 String)
	ENGINE = MergeTree PARTITION BY c4 ORDER BY (c1);
');
SELECT clickhouse_raw_query('CREATE TABLE binary_queries_test.t2 (c1 Int, c2 String)
	ENGINE = MergeTree PARTITION BY c1 % 10000 ORDER BY (c1);');
SELECT clickhouse_raw_query('CREATE TABLE binary_queries_test.t3 (c1 Int, c3 String)
	ENGINE = MergeTree PARTITION BY c1 % 10000 ORDER BY (c1);');
SELECT clickhouse_raw_query('CREATE TABLE binary_queries_test.t4 (c1 Int, c2 Int, c3 String)
	ENGINE = MergeTree PARTITION BY c1 % 10000 ORDER BY (c1);');

CREATE FOREIGN TABLE ft1 (
	c0 int,
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 date,
	c5 date,
	c6 varchar(10),
	c7 char(10) default 'ft1',
	c8 text
) SERVER binary_queries_loopback OPTIONS (table_name 't1');

ALTER FOREIGN TABLE ft1 DROP COLUMN c0;

CREATE FOREIGN TABLE ft2 (
	c1 int NOT NULL,
	c2 text NOT NULL
) SERVER binary_queries_loopback OPTIONS (table_name 't2');

CREATE FOREIGN TABLE ft4 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER binary_queries_loopback OPTIONS (table_name 't4');

CREATE FOREIGN TABLE ft5 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER binary_queries_loopback OPTIONS (table_name 't4');

CREATE FOREIGN TABLE ft6 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text
) SERVER binary_queries_loopback2 OPTIONS (table_name 't4');

select clickhouse_raw_query($$
    INSERT INTO binary_queries_test.t1
        SELECT number,
               number % 10,
               toString(number),
               toDate('1990-01-01'),
               toDate('1990-01-01'),
               number % 10,
               number % 10,
               'foo'
        FROM numbers(1, 110);$$);

select clickhouse_raw_query($$
    INSERT INTO binary_queries_test.t2
        SELECT number,
               concat('AAA', toString(number))
        FROM numbers(1, 100);$$);

select clickhouse_raw_query($$
    INSERT INTO binary_queries_test.t4
        SELECT number,
               number + 1,
               concat('AAA', toString(number))
        FROM numbers(1, 100);$$);

\set VERBOSITY terse
SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work

ALTER SERVER binary_queries_loopback OPTIONS (SET dbname 'no such database');

SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should fail

ALTER USER MAPPING FOR CURRENT_USER SERVER binary_queries_loopback OPTIONS (ADD user 'no such user');

SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should fail

ALTER SERVER binary_queries_loopback OPTIONS (SET dbname 'binary_queries_test');
ALTER USER MAPPING FOR CURRENT_USER SERVER binary_queries_loopback OPTIONS (DROP user);

SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work again

\set VERBOSITY default
ANALYZE ft1;

EXPLAIN (COSTS OFF) SELECT * FROM ft1 ORDER BY c1 OFFSET 100 LIMIT 10;
SELECT * FROM ft1 ORDER BY c1 OFFSET 100 LIMIT 10;

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 ORDER BY t1.c1, t1.tableoid OFFSET 100 LIMIT 10;

SELECT * FROM ft1 t1 ORDER BY t1.c1, t1.tableoid OFFSET 100 LIMIT 10;

EXPLAIN (VERBOSE, COSTS OFF) SELECT t1 FROM ft1 t1 ORDER BY t1.c1 OFFSET 100 LIMIT 10;

SELECT t1 FROM ft1 t1 ORDER BY t1.c1 OFFSET 100 LIMIT 10;

SELECT * FROM ft1 WHERE false;

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';

SELECT COUNT(*) FROM ft1 t1;

SELECT * FROM ft1 t1 WHERE t1.c3 IN (SELECT c2 FROM ft2 t2 WHERE c1 <= 10) ORDER BY c1;

SELECT * FROM ft1 t1 WHERE t1.c3 = (SELECT MAX(c2) FROM ft2 t2) ORDER BY c1;

WITH t1 AS (SELECT * FROM ft1 WHERE c1 <= 10) SELECT t2.c1, t2.c2, t2.c2 FROM t1, ft2 t2 WHERE t1.c1 = t2.c1 ORDER BY t1.c1;

SELECT 'fixed', NULL FROM ft1 t1 WHERE c1 = 1;

SET enable_hashjoin TO false;

SET enable_nestloop TO false;

EXPLAIN (VERBOSE, COSTS OFF) SELECT t1.c1, t2.c1 FROM ft2 t1 JOIN ft1 t2 ON (t1.c1 = t2.c1) OFFSET 100 LIMIT 10;

SELECT DISTINCT t1.c1, t2.c1 FROM ft2 t1 JOIN ft1 t2 ON (t1.c1 = t2.c1) order by t1.c1 LIMIT 10;

EXPLAIN (VERBOSE, COSTS OFF) SELECT t1.c1, t2.c1 FROM ft2 t1 LEFT JOIN ft1 t2 ON (t1.c1 = t2.c1) OFFSET 100 LIMIT 10;

EXPLAIN SELECT DISTINCT t1.c1, t2.c1 FROM ft2 t1 LEFT JOIN ft1 t2 ON (t1.c1 = t2.c1) order by t1.c1 LIMIT 10;
SELECT DISTINCT t1.c1, t2.c1 FROM ft2 t1 LEFT JOIN ft1 t2 ON (t1.c1 = t2.c1) order by t1.c1 LIMIT 10;

RESET enable_hashjoin;
RESET enable_nestloop;

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 1;         -- Var, OpExpr(b), Const

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 100 AND t1.c2 = 0; -- BoolExpr

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 IS NULL;        -- NullTest

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 IS NOT NULL;    -- NullTest

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE round(abs(c1), 0) = 1; -- FuncExpr

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = -c1;          -- OpExpr(l)

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE 1 = factorial(c1);           -- OpExpr(r)

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE (c1 IS NOT NULL) IS DISTINCT FROM (c1 IS NOT NULL); -- DistinctExpr

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = ANY(ARRAY[c2, 1, c1 + 0]); -- ScalarArrayOpExpr

SELECT * FROM ft1 t1 WHERE c1 = ANY(ARRAY[c2, 1, c1 + 0]) ORDER BY c1; -- ScalarArrayOpExpr

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c1 = (ARRAY[c1,c2,3])[1]; -- ArrayRef

SELECT * FROM ft1 t1 WHERE c1 = (ARRAY[c1,c2,3])[1] ORDER BY c1; -- ArrayRef

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c6 = E'foo''s\\bar';  -- check special chars

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE c8 = 'foo';  -- can't be sent to remote

EXPLAIN (VERBOSE, COSTS OFF) SELECT (CASE WHEN c1 < 10 THEN 1 WHEN c1 < 50 THEN 2 ELSE 3 END) a,
	sum(length(c2)) FROM ft2 GROUP BY a ORDER BY a;
SELECT (CASE WHEN c1 < 10 THEN 1 WHEN c1 < 50 THEN 2 ELSE 3 END) a,
	sum(length(c2)) FROM ft2 GROUP BY a ORDER BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT SUM(c1) FILTER (WHERE c1 < 20) FROM ft2;
SELECT SUM(c1) FILTER (WHERE c1 < 20) FROM ft2;

EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(DISTINCT c1) FROM ft2;
SELECT COUNT(DISTINCT c1) FROM ft2;

/* DISTINCT with IF */
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(DISTINCT c1) FILTER (WHERE c1 < 20) FROM ft2;
SELECT COUNT(DISTINCT c1) FILTER (WHERE c1 < 20) FROM ft2;

/* https://github.com/ClickHouse/clickhouse_fdw/issues/25 */
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(*) FILTER (WHERE c1 < 20) FROM ft2;
SELECT COUNT(*) FILTER (WHERE c1 < 10) FROM ft2;

SELECT clickhouse_raw_query('DROP DATABASE binary_queries_test');

DROP USER MAPPING FOR CURRENT_USER SERVER binary_queries_loopback2;
DROP USER MAPPING FOR CURRENT_USER SERVER binary_queries_loopback;
DROP SERVER binary_queries_loopback2 CASCADE;
DROP SERVER binary_queries_loopback CASCADE;
