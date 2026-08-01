CREATE SERVER engines_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'engines_test');
CREATE USER MAPPING FOR CURRENT_USER SERVER engines_loopback;

CREATE SERVER engines_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER engines_admin;

CALL clickhouse_perform('engines_admin', 'drop database if exists engines_test');
CALL clickhouse_perform('engines_admin', 'create database engines_test');
CALL clickhouse_perform('engines_admin', '
	create table engines_test.t1 (a int, b int)
	engine = MergeTree()
	order by a');

CALL clickhouse_perform('engines_admin', '
	create table engines_test.t2 (a int, b AggregateFunction(sum, Int32))
	engine = AggregatingMergeTree()
	order by a');

CALL clickhouse_perform('engines_admin', '
	create table engines_test.t3 (a int, b Array(Int32), c Array(Int32))
	engine = MergeTree()
	order by a');

CALL clickhouse_perform('engines_admin', '
	insert into engines_test.t1 select number % 10, number from numbers(1, 100);');

CALL clickhouse_perform('engines_admin', '
	insert into engines_test.t2 select number % 10 as a, sumState(toInt32(number)) as b from numbers(1, 100) group by a;');

CALL clickhouse_perform('engines_admin', '
	insert into engines_test.t3 select number % 10,
		[1, number % 10 + 1], [1, 1] from numbers(1, 100);');

CALL clickhouse_perform('engines_admin', '
	create materialized view engines_test.t1_aggr
		engine=AggregatingMergeTree()
		order by a populate as select a, sumState(b) as b from engines_test.t1 group by a;');

CALL clickhouse_perform('engines_admin', '
	create materialized view engines_test.t3_aggr
		engine=AggregatingMergeTree()
		order by a populate as select a, sumMapState(b, c) as b from engines_test.t3 group by a;');

CALL clickhouse_perform('engines_admin', '
	create table engines_test.t4 (a int,
		b AggregateFunction(sum, Int32),
		c AggregateFunction(sumMap, Array(Int32), Array(Int32)),
		d SimpleAggregateFunction(sum, Int64),
		e AggregateFunction(count),
		f AggregateFunction(quantile, Int32))
	engine = AggregatingMergeTree()
	order by a');

CREATE SCHEMA engines_test;
IMPORT FOREIGN SCHEMA engines_test FROM SERVER engines_loopback INTO engines_test;
SET search_path = engines_test, public;

\d+ t1
\d+ t1_aggr
\d+ t2
\d+ t3
\d+ t3_aggr
\d+ t4

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, sum(b) FROM t1 GROUP BY a;
SELECT a, sum(b) FROM t1 GROUP BY a ORDER BY a;
EXPLAIN (VERBOSE, COSTS OFF) SELECT a, sum(b) FROM t1_aggr GROUP BY a;
SELECT a, sum(b) FROM t1_aggr GROUP BY a ORDER BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, sum(b) FROM t2 GROUP BY a;
SELECT a, sum(b) FROM t2 GROUP BY a ORDER BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, percentile_cont(0.75) WITHIN GROUP (ORDER BY f) FROM t4 GROUP BY a;
SELECT a, percentile_cont(0.75) WITHIN GROUP (ORDER BY f) FROM t4 GROUP BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, percentile_cont(0.75) WITHIN GROUP (ORDER BY f) / sum(d) FROM t4 GROUP BY a;
SELECT a, percentile_cont(0.75) WITHIN GROUP (ORDER BY f) / sum(d) FROM t4 GROUP BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, percentile_disc(0.75) WITHIN GROUP (ORDER BY f) FROM t4 GROUP BY a;
EXPLAIN (VERBOSE, COSTS OFF) SELECT a, percentile_disc(0.75) WITHIN GROUP (ORDER BY f) / sum(d) FROM t4 GROUP BY a;

DROP USER MAPPING FOR CURRENT_USER SERVER engines_loopback;
CALL clickhouse_perform('engines_admin', 'DROP DATABASE engines_test');
DROP SERVER engines_loopback CASCADE;
