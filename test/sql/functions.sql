SET datestyle = 'ISO';

SELECT pgch_version() ~ '^\d+\.\d+\.\d+$';

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
SELECT clickhouse_raw_query('CREATE TABLE functions_test.t6 (i64 Int64, f64 Float64) engine=TinyLog();');
SELECT clickhouse_raw_query('CREATE TABLE functions_test.t7(dt Date) engine=TinyLog();');

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t5 VALUES
		('2025-10-15T20:12:25'),
		('2026-11-16T32:13:26'),
		('2027-12-17T22:14:27'),
		('2028-01-18T23:15:28'),
		('2029-02-19T01:16:29'),
		('2030-03-20T02:16:30')
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t7 VALUES
		('2025-10-15'),
		('2024-11-16'),
		('2023-12-17'),
		('2022-01-18'),
		('2021-02-19'),
		('2020-03-20')
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO functions_test.t6 VALUES
		(20423, 20423.123),
		(2042323443, 2042323443.232),
		(0, 0),
		(1774996811, 1774996811.8384),
$$);

CREATE FOREIGN TABLE t1 (a int, b int, c timestamp) SERVER functions_loopback;
CREATE FOREIGN TABLE t2 (a int, b int, c timestamp with time zone) SERVER functions_loopback OPTIONS (table_name 't1');
CREATE FOREIGN TABLE t3 (a int, b int) SERVER functions_loopback;
CREATE FOREIGN TABLE t3_map (key1 int, key2 text, val text) SERVER functions_loopback;
CREATE FOREIGN TABLE t4 (val text) SERVER functions_loopback;
CREATE FOREIGN TABLE t5 (ts timestamp) SERVER functions_loopback;
CREATE FOREIGN TABLE t6 (i64 BIGINT, f64 FLOAT8) SERVER functions_loopback;
CREATE FOREIGN TABLE t7 (ts date) SERVER functions_loopback;

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

EXPLAIN (VERBOSE, COSTS OFF) SELECT quantile(0.25) WITHIN GROUP (ORDER BY a) FROM t1;
SELECT quantile(0.25) WITHIN GROUP (ORDER BY a) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT quantile(a) FROM t1;
SELECT quantile(a) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT quantileExact(0.75) WITHIN GROUP (ORDER BY a) FROM t1;
SELECT quantileExact(0.75) WITHIN GROUP (ORDER BY a) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT quantileExact(a) FROM t1;
SELECT quantileExact(a) FROM t1;

EXPLAIN (VERBOSE, COSTS OFF) SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a) FROM t1;
SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT percentile_cont(0.95) WITHIN GROUP (ORDER BY date_part('epoch', timezone('UTC', c))) FROM t1;
SELECT percentile_cont(0.95) WITHIN GROUP (ORDER BY date_part('epoch', timezone('UTC', c))) FROM t1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a) FILTER (WHERE b = 1) FROM t1;
SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a) FILTER (WHERE b = 1) FROM t1;

SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a NULLS LAST) FROM t1;
SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a DESC) FROM t1;
SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a NULLS FIRST) FROM t1;
SELECT percentile_cont(0.25) WITHIN GROUP (ORDER BY a USING >) FROM t1;

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
EXPLAIN (VERBOSE, COSTS OFF) SELECT ts FROM t5 WHERE date_part('year', ts) = '2027';
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

-- Check hashing functions.
EXPLAIN (VERBOSE, COSTS OFF) SELECT key1, val FROM t3_map WHERE md5(val) LIKE 'a%' ORDER BY key1;
SELECT key1 FROM t3_map WHERE md5(val) LIKE 'a%' ORDER BY key1;

-- Check to_timestamp(float8).
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t6 WHERE to_timestamp(i64) = to_timestamp(0);
SELECT * FROM t6 WHERE to_timestamp(i64) = to_timestamp(0);
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t6 WHERE to_timestamp(f64) = to_timestamp(0);
SELECT * FROM t6 WHERE to_timestamp(f64) = to_timestamp(0);
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t6 WHERE to_timestamp(i64) = to_timestamp(0);
SELECT * FROM t6 WHERE to_timestamp(i64) = to_timestamp(2042323443);
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t6 WHERE to_timestamp(f64) = to_timestamp(2042323443);
SELECT * FROM t6 WHERE to_timestamp(f64) = to_timestamp(2042323443);

-- Check current_*-type functions.
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < NOW();
SELECT * FROM t1 WHERE c < NOW() ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < statement_timestamp();
SELECT * FROM t1 WHERE c < statement_timestamp() ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < transaction_timestamp();
SELECT * FROM t1 WHERE c < transaction_timestamp() ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < clock_timestamp();
SELECT * FROM t1 WHERE c < clock_timestamp() ORDER BY a LIMIT 2;

-- Check SQL Value functions.
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < CURRENT_DATE;
SELECT * FROM t1 WHERE c < CURRENT_DATE ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < CURRENT_TIMESTAMP;
SELECT * FROM t1 WHERE c < CURRENT_TIMESTAMP ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < CURRENT_TIMESTAMP(3);
SELECT * FROM t1 WHERE c < CURRENT_TIMESTAMP(3) ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < LOCALTIMESTAMP;
SELECT * FROM t1 WHERE c < LOCALTIMESTAMP ORDER BY a LIMIT 2;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE c < LOCALTIMESTAMP(3);
SELECT * FROM t1 WHERE c < LOCALTIMESTAMP(3) ORDER BY a LIMIT 2;

-- Use with other functions.
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE date_part('year', c) < date_part('year', CURRENT_DATE);
SELECT * FROM t1 WHERE date_part('year', c) < date_part('year', CURRENT_DATE);

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE date_trunc('day', c) < date_trunc('day', CURRENT_TIMESTAMP) - INTERVAL '1 day' ORDER BY a LIMIT 2;
SELECT * FROM t1 WHERE date_trunc('day', c) < date_trunc('day', CURRENT_TIMESTAMP) - INTERVAL '1 day' ORDER BY a;

\unset ECHO
-- Use a DO block to test TIME SQL values; Time64 added in CLickHouse 25.6.
DO $$
DECLARE
	-- Capture the CLickHouse major an minor version parse.
	chv int[] := regexp_matches(clickhouse_raw_query('SELECT version()'), '^(\d+)\.(\d+)')::int[];
	opt TEXT = '';
	output JSONB;
    result record;
BEGIN
    IF chv[1] > 25 OR (chv[1] = 25 AND chv[2] >= 6) THEN
		if chv[1] = 25 AND chv[2] < 12 THEN
			-- Times64 supported but needs to be enabled.
			opt := ' SETTINGS enable_time_time64_type = 1';
		END IF;
		-- Set up a foreign table mapping timetz to Time64.
		PERFORM clickhouse_raw_query(
			'CREATE TABLE functions_test.times (t64 Time64) engine=TinyLog()' || opt
		);
		PERFORM clickhouse_raw_query($q$
			INSERT INTO functions_test.times
			VALUES ('16:14:50.922787819'),
				   ('00:00:00'),
				   ('23:59:59.999999999')
		$q$);
		CREATE FOREIGN TABLE times (t64 TIMETZ) SERVER functions_loopback;

		-- Test that CURRENT_TIME passes down properly.
		EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM times WHERE t64 <> CURRENT_TIME INTO output;
		RAISE NOTICE 'CURRENT_TIME PUSHED DOWN: %', jsonb_path_query(
			output, '$[0].Plan'
		)->>'Remote SQL' = 'SELECT t64 FROM functions_test.times WHERE ((t64 <> toTime64(now64(6, ''America/Los_Angeles''), 6)))';
		-- XXX Add Time64 support to binary the drivers.
		-- FOR result IN SELECT * FROM times WHERE t64 <> CURRENT_TIME ORDER BY t64 LOOP
		-- 	RAISE NOTICE '%', result;
		-- END LOOP;

		-- Test that CURRENT_TIME passes down properly.
		EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM times WHERE t64 <> CURRENT_TIME(3) INTO output;
		RAISE NOTICE 'CURRENT_TIME(n) PUSHED DOWN: %', jsonb_path_query(
			output, '$[0].Plan'
		)->>'Remote SQL' = 'SELECT t64 FROM functions_test.times WHERE ((t64 <> toTime64(now64(3, ''America/Los_Angeles''), 3)))';
		-- XXX Add Time64 support to binary the drivers.
		-- FOR result IN SELECT * FROM times WHERE t64 <> CURRENT_TIME(6) ORDER BY t64 LOOP
		-- 	RAISE NOTICE '%', result;
		-- END LOOP;
    ELSE
		-- Fake it on earlier versions.
		RAISE NOTICE 'CURRENT_TIME PUSHED DOWN: t';
		RAISE NOTICE 'CURRENT_TIME(n) PUSHED DOWN: t';
    END IF;
END;
$$;

-- Use a DO block to test functions that push down a locally-generated literal.
DO $$
DECLARE
	op TEXT;
	output JSONB;
BEGIN
	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> CURRENT_USER' INTO output;
	RAISE NOTICE 'CURRENT_USER PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', current_user);

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> USER' INTO output;
	RAISE NOTICE 'USER PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', USER);

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> CURRENT_ROLE' INTO output;
	RAISE NOTICE 'CURRENT_ROLE PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', CURRENT_ROLE);

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> SESSION_USER' INTO output;
	RAISE NOTICE 'SESSION_USER PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', SESSION_USER);

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> CURRENT_SCHEMA' INTO output;
	RAISE NOTICE 'CURRENT_SCHEMA PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', CURRENT_SCHEMA);

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> CURRENT_SCHEMA()' INTO output;
	RAISE NOTICE 'CURRENT_SCHEMA() PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', CURRENT_SCHEMA());

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> CURRENT_CATALOG' INTO output;
	RAISE NOTICE 'CURRENT_CATALOG PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', CURRENT_CATALOG);

	EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t4 WHERE val <> CURRENT_DATABASE()' INTO output;
	RAISE NOTICE 'CURRENT_DATABASE() PUSHED DOWN: %', jsonb_path_query(
		output, '$[0].Plan'
	)->>'Remote SQL' = format('SELECT val FROM functions_test.t4 WHERE ((val <> %L))', CURRENT_DATABASE());
END;
$$;
\set ECHO all

SELECT * FROM t4 WHERE val <> CURRENT_USER;
SELECT * FROM t4 WHERE val <> USER;
SELECT * FROM t4 WHERE val <> CURRENT_ROLE;
SELECT * FROM t4 WHERE val <> SESSION_USER;
SELECT * FROM t4 WHERE val <> CURRENT_SCHEMA;
SELECT * FROM t4 WHERE val <> CURRENT_SCHEMA();
SELECT * FROM t4 WHERE val <> CURRENT_CATALOG;
SELECT * FROM t4 WHERE val <> CURRENT_DATABASE();

-- Test concat_ws.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t3_map WHERE concat_ws('/', key2, val) = 'key4/val4';
SELECT * FROM t3_map WHERE concat_ws('/', key2, val) = 'key4/val4';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE concat_ws(',', a, b, 'foo', c) = '2,3,foo,2019-01-02 10:00:00';
SELECT * FROM t1 WHERE concat_ws(',', a, b, 'foo', c) = '2,3,foo,2019-01-02 10:00:00';

-- Test fuzzystrmatch pushdown.
CREATE EXTENSION IF NOT EXISTS fuzzystrmatch;

-- soundex pushes down with same name.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t4 WHERE soundex(val) = 'V400';
SELECT * FROM t4 WHERE soundex(val) = 'V400';

-- 2-arg levenshtein pushes down as editDistanceUTF8.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t4 WHERE levenshtein(val, 'val1') <= 1;
SELECT * FROM t4 WHERE levenshtein(val, 'val1') <= 1;

-- 5-arg levenshtein (custom costs) evaluates locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t4 WHERE levenshtein(val, 'val1', 1, 1, 2) <= 1;

DROP EXTENSION fuzzystrmatch;

-- to_char: pushdown of formats whose every keyword has a faithful
-- formatDateTime equivalent.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT to_char(ts, 'YYYY-MM-DD HH24:MI:SS') FROM t5 ORDER BY 1;
SELECT to_char(ts, 'YYYY-MM-DD HH24:MI:SS') FROM t5 ORDER BY 1;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT to_char(ts, 'YY/MM/DD HH12:MI AM Q DDD Mon Dy') FROM t5 ORDER BY 1;
SELECT to_char(ts, 'YY/MM/DD HH12:MI AM Q DDD Mon Dy') FROM t5 ORDER BY 1;

-- Quoted literal text and a literal % round-trip via formatDateTime escaping.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT to_char(ts, '"Year=" YYYY "%"') FROM t5 ORDER BY 1;
SELECT to_char(ts, '"Year=" YYYY "%"') FROM t5 ORDER BY 1;

-- to_char: refusal cases evaluate locally rather than pushing wrong output.
-- Padded month name (Month) -- CH formatDateTime cannot blank-pad.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ts FROM t5 WHERE to_char(ts, 'Month') = 'October   ';
-- Single-digit year token (Y) -- CH has no equivalent.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ts FROM t5 WHERE to_char(ts, 'Y') = '5';
-- Ordinal suffix (TH) -- CH has no ordinal output.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ts FROM t5 WHERE to_char(ts, 'DDTH') = '15TH';
-- FM modifier suppresses padding -- CH always pads.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ts FROM t5 WHERE to_char(ts, 'FMMM') = '10';
-- Lowercase am/pm -- CH %p is always uppercase.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT ts FROM t5 WHERE to_char(ts, 'HH12 am') = '08 pm';

-- Dynamic format -- not a Const, so cannot be validated.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT to_char(ts, t4.val) FROM t5, t4 LIMIT 1;

-- reverse pushes down as reverseUTF8 to preserve code-point order on
-- multi-byte input
SELECT clickhouse_raw_query($$
    INSERT INTO functions_test.t4 VALUES ('Ωαβ'), ('hello')
$$);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE reverse(val) = 'βαΩ';
SELECT val FROM t4 WHERE reverse(val) = 'βαΩ';

-- bit_count(bytea) pushes down as bitCount (PG14+).
SELECT current_setting('server_version_num')::int >= 140000 AS pg14 \gset
\if :pg14
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE bit_count(val::bytea) = 21 ORDER BY val;
SELECT val FROM t4 WHERE bit_count(val::bytea) = 21 ORDER BY val;
\endif

-- mod(int, int) pushes down as modulo.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE mod(a, 3) = 0 ORDER BY a;
SELECT a FROM t3 WHERE mod(a, 3) = 0 ORDER BY a;

-- pow(float8, float8) and power(float8, float8) both push down as pow.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE pow(f64, 2::float8) < 1 ORDER BY i64;
SELECT i64 FROM t6 WHERE pow(f64, 2::float8) < 1 ORDER BY i64;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE power(f64, 2::float8) < 1 ORDER BY i64;
SELECT i64 FROM t6 WHERE power(f64, 2::float8) < 1 ORDER BY i64;

-- mod / pow / power on numeric push down too.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE mod(a::numeric, 3::numeric) = 0 ORDER BY a;
SELECT a FROM t3 WHERE mod(a::numeric, 3::numeric) = 0 ORDER BY a;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE pow(a::numeric, 2::numeric) = 25;
SELECT a FROM t3 WHERE pow(a::numeric, 2::numeric) = 25;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE power(a::numeric, 2::numeric) = 25;
SELECT a FROM t3 WHERE power(a::numeric, 2::numeric) = 25;

-- abs() pushes down for int / float / numeric.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE abs(a - 5) = 2 ORDER BY a;
SELECT a FROM t3 WHERE abs(a - 5) = 2 ORDER BY a;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE abs(f64) = 0;
SELECT i64 FROM t6 WHERE abs(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE abs(a::numeric) = 5;
SELECT a FROM t3 WHERE abs(a::numeric) = 5;

-- factorial(int8) pushes down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE factorial(a) = 120;
SELECT a FROM t3 WHERE factorial(a) = 120;

-- round() pushes down for float8 and numeric.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE round(f64) = 0;
SELECT i64 FROM t6 WHERE round(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE round(a::numeric) = 5;
SELECT a FROM t3 WHERE round(a::numeric) = 5;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a FROM t3 WHERE round((a::numeric) / 3, 2) = 1.67;
SELECT a FROM t3 WHERE round((a::numeric) / 3, 2) = 1.67;

-- Trig functions push down at f64 = 0 where PG and CH agree exactly.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND sin(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND sin(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND cos(f64) = 1;
SELECT i64 FROM t6 WHERE i64 = 0 AND cos(f64) = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND tan(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND tan(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND atan(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND atan(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND atan2(f64, 1::float8) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND atan2(f64, 1::float8) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND sinh(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND sinh(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND cosh(f64) = 1;
SELECT i64 FROM t6 WHERE i64 = 0 AND cosh(f64) = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND tanh(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND tanh(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND asinh(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND asinh(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND degrees(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND degrees(f64) = 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE i64 = 0 AND radians(f64) = 0;
SELECT i64 FROM t6 WHERE i64 = 0 AND radians(f64) = 0;

-- pi() is immutable so PG constant-folds before deparse; the function name
-- itself is never sent to CH but the remote literal proves the call worked.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT i64 FROM t6 WHERE f64 < pi();

-- lower(text) / upper(text) push down as lowerUTF8 / upperUTF8.
SELECT clickhouse_raw_query($$
    INSERT INTO functions_test.t4 VALUES ('VAL3'), ('Mixed')
$$);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE lower(val) = 'val3';
SELECT val FROM t4 WHERE lower(val) = 'val3';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE upper(val) = 'VAL1';
SELECT val FROM t4 WHERE upper(val) = 'VAL1';

-- substring/substr (text) push down as substringUTF8 (counts code points).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE substring(val, 1, 3) = 'val' ORDER BY val;
SELECT val FROM t4 WHERE substring(val, 1, 3) = 'val' ORDER BY val;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE substr(val, 2) = 'αβ';
SELECT val FROM t4 WHERE substr(val, 2) = 'αβ';

-- substring/substr (bytea) push down as substring (byte-based).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE substring(val::bytea, 1, 2) = 'va'::bytea ORDER BY val;
SELECT val FROM t4 WHERE substring(val::bytea, 1, 2) = 'va'::bytea ORDER BY val;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE substring(val::bytea, 1, 2) = '\xcea9'::bytea;
SELECT val FROM t4 WHERE substring(val::bytea, 1, 2) = '\xcea9'::bytea;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE substr(val::bytea, 2) = 'al1'::bytea;
SELECT val FROM t4 WHERE substr(val::bytea, 2) = 'al1'::bytea;

-- length(text) pushes down as lengthUTF8 (counts code points).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE length(val) = 3;
SELECT val FROM t4 WHERE length(val) = 3;

-- length(bytea) pushes down as length (counts bytes).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE length(val::bytea) = 6;
SELECT val FROM t4 WHERE length(val::bytea) = 6;

-- octet_length(text) / octet_length(bytea) push down as length.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE octet_length(val) = 6;
SELECT val FROM t4 WHERE octet_length(val) = 6;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE octet_length(val::bytea) = 6;
SELECT val FROM t4 WHERE octet_length(val::bytea) = 6;

-- reverse(bytea) added in PG18, pushes down as CH reverse (byte-wise).
SELECT current_setting('server_version_num')::int >= 180000 AS pg18 \gset
\if :pg18
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val FROM t4 WHERE reverse(val::bytea) = 'olleh'::bytea;
SELECT val FROM t4 WHERE reverse(val::bytea) = 'olleh'::bytea;
\endif

-- date(timestamp) and date(timestamptz) push down as CH date (alias for toDate).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a, b FROM t1 WHERE date(c) = '2019-01-01'::date;
SELECT a, b FROM t1 WHERE date(c) = '2019-01-01'::date;
-- Pin TZ for the timestamptz variant so PG and CH interpret c identically.
SET timezone = 'UTC';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a, b FROM t2 WHERE date(c) = '2019-01-01'::date;
SELECT a, b FROM t2 WHERE date(c) = '2019-01-01'::date;
RESET timezone;

DROP USER MAPPING FOR CURRENT_USER SERVER functions_loopback;
SELECT clickhouse_raw_query('DROP DATABASE functions_test');
DROP SERVER functions_loopback CASCADE;
