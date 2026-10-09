\i test/include/normalize.sql
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

CALL clickhouse_perform('deparse_admin', '
	create table deparse_test.t2 (id Int32, grp Int32, s String, ts DateTime64(6), n Nullable(Int32), arr Array(Int32))
	engine = MergeTree()
	order by id');
CALL clickhouse_perform('deparse_admin', '
	create table deparse_test.t3 (id Int32, v Nullable(Int32), sign Int8)
	engine = CollapsingMergeTree(sign)
	order by id');

CREATE SCHEMA deparse_test;
IMPORT FOREIGN SCHEMA deparse_test FROM SERVER deparse_lookback INTO deparse_test;
SET search_path = deparse_test, public;
\d t1
ALTER TABLE t1 ALTER COLUMN b SET DATA TYPE bool;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT (CASE WHEN b THEN 1 ELSE 2 END) as g1, MAX(a) FROM t1 GROUP BY g1;
SELECT (CASE WHEN b THEN 1 ELSE 2 END) as g1, MAX(a) FROM t1 GROUP BY g1;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 ORDER BY a NULLS FIRST, b LIMIT 3;
SELECT * FROM t1 ORDER BY a NULLS FIRST, b LIMIT 3;

-- Expression forms
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE greatest(id, grp) > 1 AND least(id, grp, 3) < 5;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE nullif(grp, 1) = 2;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT CASE grp WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE s END, count(*) FROM t2 GROUP BY 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE id / 2 = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE concat(s, NULL::text) = 'x' AND coalesce(n > 1, true);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE array_fill(grp, ARRAY[2]) = ARRAY[1, 1];
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE to_char(ts, '"a\"b" \"YYYY %') = 'x';

-- Array literals
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE arr = ARRAY[[1, 2], [3, 4]] OR arr = ARRAY[1, NULL] OR arr = '{}';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE s = ANY(ARRAY['a''b', E'c\\d', NULL]);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE CASE grp WHEN 1 THEN true WHEN 2 THEN false END;

-- hstore fetch with constant hstore ships as array lookup
CREATE EXTENSION IF NOT EXISTS hstore;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE ('a=>x, b=>y'::hstore -> s) = 'x';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2 WHERE (hstore(s, s) -> s) = 'x';

-- Aggregates: CollapsingMergeTree sign weighting, ordered-set
EXPLAIN (VERBOSE, COSTS OFF)
SELECT sum(v), avg(v) FILTER (WHERE v > 0), avg(v), count(v) FROM t3;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT percentile_cont(0.5) WITHIN GROUP (ORDER BY grp),
       percentile_disc(ARRAY[0.25, 0.75]) WITHIN GROUP (ORDER BY grp)
FROM t2;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT rank(1, 'x') WITHIN GROUP (ORDER BY grp, s) FROM t2;

-- Casts
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t2
WHERE id::int2 = 1 AND id::float4 = 1 AND id::numeric = 1 AND id::numeric(10, 2) = 1
  AND s::varchar = 'a' AND s::varchar(5) = 'a' AND s::char(3) = 'a' AND (id > 1)::bool;

-- Window frames, one window per query to push down
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS BETWEEN 1 FOLLOWING AND 3 FOLLOWING) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS BETWEEN 3 PRECEDING AND 1 PRECEDING) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS UNBOUNDED PRECEDING) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS CURRENT ROW) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT sum(grp) OVER (ORDER BY id ROWS 2 PRECEDING) FROM t2;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT lag(grp, 2, 0) OVER (PARTITION BY grp, s ORDER BY id, ts) FROM t2;
SELECT * FROM pg_temp.remote_sql($$
SELECT id, sum(grp) FILTER (WHERE grp > 1) OVER (ORDER BY id) FROM t2$$);

-- Join and grouping shapes
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, t2.id FROM t1 RIGHT JOIN t2 ON t1.a = t2.id WHERE t1.a > 1 OR t1.a IS NULL;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, t2.id FROM t1 FULL JOIN t2 ON t1.a = t2.id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, t2.id FROM t1 JOIN t2 ON t1.a < t2.id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a FROM t1 JOIN t2 ON t1.a = t2.id WHERE t2.s = random()::text;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 LEFT JOIN t2 ON t1.a = t2.id AND t2.s = random()::text;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 LEFT JOIN (SELECT id, coalesce(n, 0) AS x FROM t2) ss ON t1.a = ss.id
JOIN t2 t3 ON t3.id = t1.a;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a FROM t1 WHERE NOT EXISTS (SELECT 1 FROM t2 JOIN t2 t3 ON t2.id = t3.id WHERE t2.id = t1.a);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT grp, count(*) FROM t2 GROUP BY ROLLUP(grp);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT grp, count(*) FROM t2 WHERE s = random()::text GROUP BY grp;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT grp FROM t2 GROUP BY grp HAVING max(s) = random()::text;

DROP USER MAPPING FOR CURRENT_USER SERVER deparse_lookback;
CALL clickhouse_perform('deparse_admin', 'DROP DATABASE deparse_test');
DROP SERVER deparse_lookback CASCADE;
