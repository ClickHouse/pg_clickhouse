SET datestyle = 'ISO';
CREATE SERVER functions_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'functions_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER functions_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS functions_test');
SELECT clickhouse_raw_query('CREATE DATABASE functions_test');

-- argMax, argMin
SELECT clickhouse_raw_query($$
	CREATE TABLE functions_test.t1 (a int, b int, c DateTime) ENGINE = MergeTree ORDER BY (a);
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t1 VALUES
		(1, 1, '2019-01-01 10:00:00'),
		(2, 2, '2019-01-02 10:00:00'),
		(2, 2, '2019-01-02 11:00:00'),
		(2, 3, '2019-01-02 10:00:00')
$$);

SELECT clickhouse_raw_query($$
	drop dictionary if exists functions_test.t3_dict
$$);

SELECT clickhouse_raw_query('
	create table functions_test.t3 (a Int32, b Nullable(Int32))
	engine = MergeTree()
	order by a');
SELECT clickhouse_raw_query('CREATE TABLE functions_test.t3_map (key1 Int32, key2 String,
        val String) engine=TinyLog();');
SELECT clickhouse_raw_query('CREATE TABLE functions_test.t4 (val String) engine=TinyLog();');

CREATE FOREIGN TABLE t1 (a int, b int, c timestamp) SERVER functions_loopback;
CREATE FOREIGN TABLE t2 (a int, b int, c timestamp with time zone) SERVER functions_loopback OPTIONS (table_name 't1');
CREATE FOREIGN TABLE t3 (a int, b int) SERVER functions_loopback;
CREATE FOREIGN TABLE t3_map (key1 int, key2 text, val text) SERVER functions_loopback;
CREATE FOREIGN TABLE t4 (val text) SERVER functions_loopback;

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t3
	SELECT number+1, number+2
	  FROM numbers(10);
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t3_map
	SELECT number+1, 'key'|| number+1, 'val' || number+1
	  FROM numbers(10);
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t4
	SELECT 'val' || number+1
	  FROM numbers(2);
$$);

SELECT clickhouse_raw_query($$
	create dictionary functions_test.t3_dict
    (key1 Int32, key2 String, val String)
    primary key key1, key2
    source(clickhouse(host '127.0.0.1' port 9000 db 'functions_test' table 't3_map' user 'default' password ''))
    layout(complex_key_hashed())
    lifetime(10);
$$);

-- check coalesce((cast as Nullable...
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT coalesce(a::text, b::text, c::text) FROM t1 GROUP BY a, b, c;
SELECT coalesce(a::text, b::text, c::text) FROM t1 GROUP BY a, b, c;

-- check IN functions
EXPLAIN (VERBOSE, COSTS OFF)
	SELECT a, sum(b) FROM t1 WHERE a IN (1,2,3) GROUP BY a;
SELECT a, sum(b) FROM t1 WHERE a IN (1,2,3) GROUP BY a;

EXPLAIN (VERBOSE, COSTS OFF)
	SELECT a, sum(b) FROM t1 WHERE a NOT IN (1,2,3) GROUP BY a;
SELECT a, sum(b) FROM t1 WHERE a NOT IN (1,2,3) GROUP BY a;

-- check argMin, argMax, uniqExact
EXPLAIN (VERBOSE, COSTS OFF) SELECT argMin(a, b) FROM t1;
SELECT argMin(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT argMax(a, b) FROM t1;
SELECT argMax(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT argMin(a, c) FROM t1;
SELECT argMin(a, c) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT argMax(a, c) FROM t1;
SELECT argMax(a, c) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqExact(a) FROM t1;
SELECT uniqExact(a) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqExact(a) FILTER(WHERE b>1) FROM t1;
SELECT uniqExact(a) FILTER(WHERE b>1) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqExact(a, b) FROM t1;
SELECT uniqExact(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqExact(a, c) FROM t1;
SELECT uniqExact(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_trunc('dAy', c at time zone 'UTC') as d1 FROM t1 GROUP BY d1 ORDER BY d1;
SELECT date_trunc('day', c at time zone 'UTC') as d1 FROM t1 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_trunc('day', c at time zone 'UTC') as d1 FROM t2 GROUP BY d1 ORDER BY d1;
SELECT date_trunc('day', c at time zone 'UTC') as d1 FROM t2 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_part('day'::text, timezone('UTC'::text, c)) as d1 FROM t1 GROUP BY d1 ORDER BY d1;
SELECT date_part('day'::text, timezone('UTC'::text, c)) as d1 FROM t1 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_part('day'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;
SELECT date_part('day'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_part('doy'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;
SELECT date_part('doy'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_part('dow'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;
SELECT date_part('dow'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_part('minuTe'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;
SELECT date_part('minuTe'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_trunc('SeCond', c at time zone 'UTC') as d1 FROM t1 GROUP BY d1 ORDER BY d1;
SELECT date_trunc('SeCond', c at time zone 'UTC') as d1 FROM t1 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT date_part('ePoch'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;
SELECT date_part('ePoch'::text, timezone('UTC'::text, c)) as d1 FROM t2 GROUP BY d1 ORDER BY d1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT ltrim(val) AS a, btrim(val) AS b, rtrim(val) AS c FROM t4 GROUP BY a,b,c ORDER BY a;
SELECT ltrim(val) AS a, btrim(val) AS b, rtrim(val) AS c FROM t4 GROUP BY a,b,c ORDER BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT strpos(val, 'val') AS a FROM t4 GROUP BY a ORDER BY a;
SELECT strpos(val, 'val') AS a FROM t4 GROUP BY a ORDER BY a;

--- check dictGet
-- dictGet is broken for now
EXPLAIN (VERBOSE, COSTS OFF) SELECT a, dictGet('functions_test.t3_dict', 'val', (a, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a;
-- SELECT a, dictGet('functions_test.t3_dict', 'val', (a, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a;

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, dictGet('functions_test.t3_dict', 'val', (1, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a;
-- SELECT a, dictGet('functions_test.t3_dict', 'val', (1, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a;

DROP USER MAPPING FOR CURRENT_USER SERVER functions_loopback;
SELECT clickhouse_raw_query('DROP DATABASE functions_test');
DROP SERVER functions_loopback CASCADE;
