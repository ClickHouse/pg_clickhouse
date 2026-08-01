CREATE SERVER deparse_lookback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'deparse_test');
CREATE USER MAPPING FOR CURRENT_USER SERVER deparse_lookback;

CREATE SERVER deparse_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER deparse_admin;

CALL clickhouse_perform('deparse_admin', 'drop database if exists deparse_test');
CALL clickhouse_perform('deparse_admin', 'create database deparse_test');
CALL clickhouse_perform('deparse_admin', '
	create table deparse_test.t1 (a int, b Int8)
	engine = MergeTree()
	order by a');

CALL clickhouse_perform('deparse_admin', '
	insert into deparse_test.t1 select number % 10, number % 10 > 5 from numbers(1, 100);');

CREATE SCHEMA deparse_test;
IMPORT FOREIGN SCHEMA deparse_test FROM SERVER deparse_lookback INTO deparse_test;
SET search_path = deparse_test, public;
\d+ t1
ALTER TABLE t1 ALTER COLUMN b SET DATA TYPE bool;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT (CASE WHEN b THEN 1 ELSE 2 END) as g1, MAX(a) FROM t1 GROUP BY g1;
SELECT (CASE WHEN b THEN 1 ELSE 2 END) as g1, MAX(a) FROM t1 GROUP BY g1;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 ORDER BY a NULLS FIRST, b LIMIT 3;
SELECT * FROM t1 ORDER BY a NULLS FIRST, b LIMIT 3;

DROP USER MAPPING FOR CURRENT_USER SERVER deparse_lookback;
CALL clickhouse_perform('deparse_admin', 'DROP DATABASE deparse_test');
DROP SERVER deparse_lookback CASCADE;
