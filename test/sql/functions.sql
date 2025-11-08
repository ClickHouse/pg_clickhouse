SET datestyle = 'ISO';
CREATE SERVER functions_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'functions_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER functions_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS functions_test');
SELECT clickhouse_raw_query('CREATE DATABASE functions_test');

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
SELECT clickhouse_raw_query('CREATE TABLE functions_test.t5 (ts DateTime) engine=TinyLog();');

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t5 VALUES
		('2025-10-15T20:12:25'),
		('2026-11-16T32:13:26'),
		('2027-12-17T22:14:27'),
		('2028-01-18T23:15:28'),
		('2029-02-19T01:16:29'),
		('2030-03-20T02:16:30')
$$);

CREATE FOREIGN TABLE t1 (a int, b int, c timestamp) SERVER functions_loopback;
CREATE FOREIGN TABLE t2 (a int, b int, c timestamp with time zone) SERVER functions_loopback OPTIONS (table_name 't1');
CREATE FOREIGN TABLE t3 (a int, b int) SERVER functions_loopback;
CREATE FOREIGN TABLE t3_map (key1 int, key2 text, val text) SERVER functions_loopback;
CREATE FOREIGN TABLE t4 (val text) SERVER functions_loopback;
CREATE FOREIGN TABLE t5 (ts timestamp) SERVER functions_loopback;

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t3
	SELECT number+1, number+2
	  FROM numbers(10);
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t3_map
	SELECT toString(number+1), 'key' || toString(number+1), 'val' || toString(number+1)
	  FROM numbers(10);
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t4
	SELECT 'val' || toString(number+1)
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

-- check aggregates.
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
SELECT uniq(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniq(a, c) FROM t1;
SELECT uniq(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqExact(a, b) FROM t1;
SELECT uniqExact(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqExact(a, c) FROM t1;
SELECT uniqExact(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqCombined(a, b) FROM t1;
SELECT uniqCombined(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqCombined(a, c) FROM t1;
SELECT uniqCombined(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqCombined64(a, b) FROM t1;
SELECT uniqCombined64(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqCombined64(a, c) FROM t1;
SELECT uniqCombined64(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqHLL12(a, b) FROM t1;
SELECT uniqHLL12(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqHLL12(a, c) FROM t1;
SELECT uniqHLL12(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqTheta(a, b) FROM t1;
SELECT uniqTheta(a, b) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT uniqTheta(a, c) FROM t1;
SELECT uniqTheta(a, c) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT quantile(params(0.25), a) FROM t1;
SELECT quantile(params(0.25), a) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT quantile(a) FROM t1;
SELECT quantile(a) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT quantileExact(params(0.75), a) FROM t1;
SELECT quantileExact(params(0.75), a) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT quantileExact(a) FROM t1;
SELECT quantileExact(a) FROM t1;

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
EXPLAIN (VERBOSE, COSTS OFF) SELECT a, dictGet('functions_test.t3_dict', 'val', (a, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a;
SELECT a, dictGet('functions_test.t3_dict', 'val', (a, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a LIMIT 3;

EXPLAIN (VERBOSE, COSTS OFF) SELECT a, dictGet('functions_test.t3_dict', 'val', (1, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a;
SELECT a, dictGet('functions_test.t3_dict', 'val', (1, 'key' || a::text)) as val, sum(b) FROM t3 GROUP BY a, val ORDER BY a LIMIT 3;

-- Check date_part mappings.
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('year', ts) = '2023';
SELECT ts FROM t5 WHERE date_part('year', ts) = '2027';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('month', ts) = '11';
SELECT ts FROM t5 WHERE date_part('month', ts) = '11';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('day', ts) = '18';
SELECT ts FROM t5 WHERE date_part('day', ts) = '18';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('hour', ts) = '20';
SELECT ts FROM t5 WHERE date_part('hour', ts) = '20';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('minute', ts) = '16';
SELECT ts FROM t5 WHERE date_part('minute', ts) = '16';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('second', ts) = '26';
SELECT ts FROM t5 WHERE date_part('second', ts) = '26';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('doy', ts) = '351';
SELECT ts FROM t5 WHERE date_part('doy', ts) = '351';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('dow', ts) = '2';
SELECT ts FROM t5 WHERE date_part('dow', ts) = '2';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('quarter', ts) = '1';
SELECT ts FROM t5 WHERE date_part('quarter', ts) = '1';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('isoyear', ts) = '2025';
SELECT ts FROM t5 WHERE date_part('isoyear', ts) = '2025';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('week', ts) = '47';
SELECT ts FROM t5 WHERE date_part('week', ts) = '47';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('epoch', ts) > '1866158180';
SELECT ts FROM t5 WHERE date_part('epoch', ts) > '1866158180';

-- Check date_part from date.
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('year', ts::date) = 2027;
SELECT ts FROM t5 WHERE date_part('year', ts::date) = 2027;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('month', ts::date) = 11;
SELECT ts FROM t5 WHERE date_part('month', ts::date) = 11;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('day', ts::date) = 18;
SELECT ts FROM t5 WHERE date_part('day', ts::date) = 18;

-- Check EXTRACT mappings.
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(year FROM ts) = 2027;
SELECT ts FROM t5 WHERE EXTRACT(year FROM ts) = 2027;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(month FROM ts) = 11;
SELECT ts FROM t5 WHERE EXTRACT(month FROM ts) = 11;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(day FROM ts) = 18;
SELECT ts FROM t5 WHERE EXTRACT(day FROM ts) = 18;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(hour FROM ts) = 20;
SELECT ts FROM t5 WHERE EXTRACT(hour FROM ts) = 20;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(minute FROM ts) = 16;
SELECT ts FROM t5 WHERE EXTRACT(minute FROM ts) = 16;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(second FROM ts) = 26;
SELECT ts FROM t5 WHERE EXTRACT(second FROM ts) = 26;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(doy FROM ts) = 351;
SELECT ts FROM t5 WHERE EXTRACT(doy FROM ts) = 351;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(dow FROM ts) = 2;
SELECT ts FROM t5 WHERE EXTRACT(dow FROM ts) = 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(quarter FROM ts) = 1;
SELECT ts FROM t5 WHERE EXTRACT(quarter FROM ts) = 1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(isoyear FROM ts) = 2025;
SELECT ts FROM t5 WHERE EXTRACT(isoyear FROM ts) = 2025;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(week FROM ts) = 47;
SELECT ts FROM t5 WHERE EXTRACT(week FROM ts) = 47;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(epoch FROM ts) > 1866158180;
SELECT ts FROM t5 WHERE EXTRACT(epoch FROM ts) > 1866158180;

-- Check extract from date.
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(year FROM ts::date) = 2027;
SELECT ts FROM t5 WHERE EXTRACT(year FROM ts::date) = 2027;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(month FROM ts::date) = 11;
SELECT ts FROM t5 WHERE EXTRACT(month FROM ts::date) = 11;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE EXTRACT(day FROM ts::date) = 18;
SELECT ts FROM t5 WHERE EXTRACT(day FROM ts::date) = 18;


-- Check date_trunc mappings.
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('year', ts) = '2026-01-01'::date;
SELECT ts FROM t5 WHERE date_trunc('year', ts) = '2026-01-01'::date;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('month', ts) = '2027-12-01'::date;
SELECT ts FROM t5 WHERE date_trunc('month', ts) = '2027-12-01'::date;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('day', ts) = '2028-01-18'::date;
SELECT ts FROM t5 WHERE date_trunc('day', ts) = '2028-01-18'::date;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('hour', ts) = '2029-02-19T01:00:00';
SELECT ts FROM t5 WHERE date_trunc('hour', ts) = '2029-02-19T01:00:00';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('minute', ts) = '2025-10-15T20:12:00';
SELECT ts FROM t5 WHERE date_trunc('minute', ts) = '2025-10-15T20:12:00';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('second', ts) = '2027-12-17T22:14:27';
SELECT ts FROM t5 WHERE date_trunc('second', ts) = '2027-12-17T22:14:27';
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('week', ts) = '2028-01-17'::date;
SELECT ts FROM t5 WHERE date_trunc('week', ts) = '2028-01-17'::date;
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_trunc('quarter', ts) = '2027-10-01'::date;
SELECT ts FROM t5 WHERE date_trunc('quarter', ts) = '2027-10-01'::date;

-- check regexp_like.
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM t4 WHERE regexp_like('^val\d', val);
SELECT val FROM t4 WHERE regexp_like('^val\d', val);

DROP USER MAPPING FOR CURRENT_USER SERVER functions_loopback;
SELECT clickhouse_raw_query('DROP DATABASE functions_test');
DROP SERVER functions_loopback CASCADE;
