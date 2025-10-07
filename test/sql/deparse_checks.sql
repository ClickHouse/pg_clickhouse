CREATE SERVER deparse_lookback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'deparse_test');
CREATE USER MAPPING FOR CURRENT_USER SERVER deparse_lookback;

SELECT clickhouse_raw_query('drop database if exists deparse_test');
SELECT clickhouse_raw_query('create database deparse_test');
SELECT clickhouse_raw_query('
	create table deparse_test.t1 (a int, b Int8)
	engine = MergeTree()
	order by a');

SELECT clickhouse_raw_query('
	insert into deparse_test.t1 select number % 10, number % 10 > 5 from numbers(1, 100);');

IMPORT FOREIGN SCHEMA "deparse_test" FROM SERVER deparse_lookback INTO public;
\d+ t1
ALTER TABLE t1 ALTER COLUMN b SET DATA TYPE bool;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT (CASE WHEN b THEN 1 ELSE 2 END) as g1, MAX(a) FROM t1 GROUP BY g1;
SELECT (CASE WHEN b THEN 1 ELSE 2 END) as g1, MAX(a) FROM t1 GROUP BY g1;

DROP USER MAPPING FOR CURRENT_USER SERVER deparse_lookback;
SELECT clickhouse_raw_query('DROP DATABASE deparse_test');
DROP SERVER deparse_lookback CASCADE;
