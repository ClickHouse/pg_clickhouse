SET datestyle = 'ISO';
CREATE SERVER http_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'http_test', driver 'http');
CREATE SERVER http_loopback2 FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'http_test');

CREATE USER MAPPING FOR CURRENT_USER SERVER http_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_loopback2;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS http_test');
SELECT clickhouse_raw_query('CREATE DATABASE http_test', '');
SELECT clickhouse_raw_query('CREATE TABLE http_test.t1
	(c1 Int, c2 Int, c3 String, c4 Date, c5 Date, c6 String, c7 String, c8 String)
	ENGINE = MergeTree PARTITION BY c4 ORDER BY (c1);
');
SELECT clickhouse_raw_query('CREATE TABLE http_test.t2 (c1 Int, c2 String)
	ENGINE = MergeTree PARTITION BY c1 % 10000 ORDER BY (c1);');
SELECT clickhouse_raw_query('CREATE TABLE http_test.t3 (c1 Int, c3 String)
	ENGINE = MergeTree PARTITION BY c1 % 10000 ORDER BY (c1);');
SELECT clickhouse_raw_query('CREATE TABLE http_test.t4 (c1 Int, c2 Int, c3 String, c4 Bool)
	ENGINE = MergeTree PARTITION BY c1 % 10000 ORDER BY (c1);');
SELECT clickhouse_raw_query('
	CREATE TABLE tcopy
		(c1 Int32, c2 Int64, c3 Date, c4 Nullable(DateTime), c5 DateTime, c6 String)
	ENGINE = MergeTree
	PARTITION BY c3
	ORDER BY (c1, c2, c3);
', 'dbname=http_test');

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
) SERVER http_loopback OPTIONS (table_name 't1');

CREATE FOREIGN TABLE ft1_stream (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 date,
	c5 date,
	c6 varchar(10),
	c7 char(10) default 'ft1',
	c8 text
) SERVER http_loopback OPTIONS (table_name 't1', fetch_size '100');

ALTER FOREIGN TABLE ft1 DROP COLUMN c0;

CREATE FOREIGN TABLE ft2 (
	c1 int NOT NULL,
	c2 text NOT NULL
) SERVER http_loopback OPTIONS (table_name 't2');

CREATE FOREIGN TABLE ft3 (
	c1 int NOT NULL,
	c3 text
) SERVER http_loopback OPTIONS (table_name 't3');

CREATE FOREIGN TABLE ft4 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 bool
) SERVER http_loopback OPTIONS (table_name 't4');

CREATE FOREIGN TABLE ft5 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 bool
) SERVER http_loopback OPTIONS (table_name 't4');

CREATE FOREIGN TABLE ft6 (
	c1 int NOT NULL,
	c2 int NOT NULL,
	c3 text,
	c4 bool
) SERVER http_loopback2 OPTIONS (table_name 't4');

CREATE FOREIGN TABLE ftcopy (
	c1 int,
	c2 int8,
	c3 date,
	c4 timestamp without time zone,
	c5 time,
	c6 text
) SERVER http_loopback OPTIONS (table_name 'tcopy');

INSERT INTO ft1
	SELECT id,
	       id % 10,
	       to_char(id, 'FM00000'),
	       '1990-01-01',
	       '1990-01-01',
	       id % 10,
	       id % 10,
	       'foo'
	FROM generate_series(1, 110) id;

INSERT INTO ft2
	SELECT id,
	       'AAA' || to_char(id, 'FM000')
	FROM generate_series(1, 100) id;

INSERT INTO ft3 VALUES (1, E'lf\ntab\t\b\f\r');
SELECT c3, (c3 = E'lf\ntab\t\b\f\r') AS true FROM ft3 WHERE c1 = 1;
INSERT INTO ft3 VALUES (2, 'lf\ntab\t\b\f\r');
SELECT c3, (c3 = 'lf\ntab\t\b\f\r') AS true FROM ft3 WHERE c1 = 2;
INSERT INTO ft3 VALUES (3, '');
SELECT c3, (c3 = '') AS true FROM ft3 WHERE c1 = 3;

INSERT INTO ft4
	SELECT id,
		   id + 1,
		   'AAA' || to_char(id, 'FM000'),
		   (id % 2)::bool
	FROM generate_series(1, 100) id;

-- 15 rows with fetch_size 100 bytes forces multiple streaming batches.
SELECT c1, c3 FROM ft1_stream WHERE c1 <= 15 ORDER BY c1;

SELECT * FROM ft5 ORDER BY c1 LIMIT 5;

COPY ftcopy FROM stdin;
1	2	1990-01-01	1990-01-01 10:01:02	10:01:02	val1
2	3	1990-02-02	1990-02-02 11:02:03	11:01:02	val2
\.

COPY ftcopy (c1, c2, c3, c4, c5, c6) FROM stdin;
\.

INSERT INTO ftcopy VALUES
	(3, 4, '1990-03-03', '1990-03-03 12:02:02', '12:02:02', 'val3'),
	(4, 5, '1991-04-04', '1990-04-04 12:04:04', '12:02:04', 'val4'),
	(5, 6, '1991-04-04', NULL, '12:02:05', 'val5');

EXPLAIN (VERBOSE) SELECT * FROM ftcopy ORDER BY c1;
SELECT * FROM ftcopy ORDER BY c1;

SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work

ALTER SERVER http_loopback OPTIONS (SET dbname 'no such database');

SELECT c3, c4 FROM ft1 ORDER BY c3, c1;  -- should fail

ALTER USER MAPPING FOR CURRENT_USER SERVER http_loopback OPTIONS (ADD user 'no such user');

SELECT c3, c4 FROM ft1 ORDER BY c3, c1;  -- should fail

ALTER SERVER http_loopback OPTIONS (SET dbname 'http_test');
ALTER USER MAPPING FOR CURRENT_USER SERVER http_loopback OPTIONS (DROP user);

SELECT c3, c4 FROM ft1 ORDER BY c3, c1 LIMIT 1;  -- should work again

ANALYZE ft1;

EXPLAIN (COSTS OFF) SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;
SELECT * FROM ft1 ORDER BY c3, c1 OFFSET 100 LIMIT 10;

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1, t1.tableoid OFFSET 100 LIMIT 10;

SELECT * FROM ft1 t1 ORDER BY t1.c3, t1.c1, t1.tableoid OFFSET 100 LIMIT 10;

EXPLAIN (VERBOSE, COSTS OFF) SELECT t1 FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;

SELECT t1 FROM ft1 t1 ORDER BY t1.c3, t1.c1 OFFSET 100 LIMIT 10;

SELECT * FROM ft1 WHERE false;

EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ft1 t1 WHERE t1.c1 = 101 AND t1.c6 = '1' AND t1.c7 >= '1';

SELECT COUNT(*) FROM ft1 t1;

SELECT * FROM ft1 t1 WHERE t1.c3 IN (SELECT c2 FROM ft2 t2 WHERE c1 <= 10) ORDER BY c1;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM ft1 t1 WHERE t1.c3 = (SELECT MAX(c2) FROM ft2 t2) ORDER BY c1;
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

/* Disallow line endings in database names. */
SELECT clickhouse_raw_query('SELECT 1', E'dbname=''http_test\r\nX-My-Header: 123''');
-- SELECT clickhouse_raw_query('SELECT 1', E$$dbname='test\rX-My-Header: 123'$$);

CREATE SERVER http_loopback_bad FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname E'http_test\r\nX-My-Header: 123');
CREATE USER MAPPING FOR CURRENT_USER SERVER http_loopback_bad;

CREATE FOREIGN TABLE bad_name (
	c1 int NOT NULL,
	c3 text
) SERVER http_loopback_bad OPTIONS ( table_name 't3' );
SELECT * FROM bad_name;

/* ===== fetch_size option tests ===== */

/* Server-level fetch_size: set to 0 to disable streaming. */
CREATE SERVER http_no_stream FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'http_test', driver 'http', fetch_size '0');
CREATE USER MAPPING FOR CURRENT_USER SERVER http_no_stream;

CREATE FOREIGN TABLE ft_no_stream (
    c1 int NOT NULL,
    c2 int NOT NULL,
    c3 text
) SERVER http_no_stream OPTIONS (table_name 't1');

/* Query with streaming disabled (fetch_size = 0). */
SELECT c3 FROM ft_no_stream ORDER BY c1 LIMIT 3;

/* Table-level fetch_size overrides server-level. */
CREATE FOREIGN TABLE ft_override_stream (
    c1 int NOT NULL,
    c2 int NOT NULL,
    c3 text
) SERVER http_no_stream OPTIONS (table_name 't1', fetch_size '100');

/* Query with table-level streaming override (fetch_size = 100). */
SELECT c3 FROM ft_override_stream ORDER BY c1 LIMIT 3;

/* Default (no fetch_size set) uses streaming — already tested via ft1. */
SELECT c3 FROM ft1 ORDER BY c1 LIMIT 3;

/* Negative fetch_size should be rejected. */
CREATE SERVER http_bad_fetch FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'http_test', driver 'http', fetch_size '-1');

CREATE FOREIGN TABLE ft_bad_fetch (
    c1 int NOT NULL,
    c3 text
) SERVER http_loopback OPTIONS (table_name 't1', fetch_size '-1');

/*
 * TabSeparated does not escape `[` or `]` in String values, so a value like
 * `[foo]bar` is wire-indistinguishable from a CH array literal. The parser
 * uses the destination Postgres column type to decide.
 */
SELECT clickhouse_raw_query('CREATE TABLE http_test.t_brk
    (id String, term String) ENGINE = MergeTree ORDER BY id');

CREATE FOREIGN TABLE ft_brk (id text, term text)
    SERVER http_loopback OPTIONS (table_name 't_brk');

INSERT INTO ft_brk VALUES
    ('1', '[abc_def]_ghi.jkl_mno'),
    ('2', 'plainstring'),
    ('3', '[just_brackets]'),
    ('4', '[]'),
    ('5', '[a,b,c]_trailing'),
    ('6', 'leading text [bracketed] trailing');

SELECT id, term FROM ft_brk ORDER BY id;

/* Streaming path with a small fetch_size forces multi-batch parsing. */
CREATE FOREIGN TABLE ft_brk_stream (id text, term text)
    SERVER http_loopback OPTIONS (table_name 't_brk', fetch_size '32');
SELECT id, term FROM ft_brk_stream ORDER BY id;

/*
 * Mixed row: real Array(String) column alongside a String column whose value
 * starts with `[`. The parser must stay in sync across columns.
 */
SELECT clickhouse_raw_query('CREATE TABLE http_test.t_brk_mix
    (id String, tags Array(String), term String) ENGINE = MergeTree ORDER BY id');

CREATE FOREIGN TABLE ft_brk_mix (id text, tags text[], term text)
    SERVER http_loopback OPTIONS (table_name 't_brk_mix');

INSERT INTO ft_brk_mix VALUES
    ('1', ARRAY['x', 'y'], '[bracket_string]'),
    ('2', ARRAY[]::text[], '[]_not_array');

SELECT id, tags, term FROM ft_brk_mix ORDER BY id;

/*
 * The wire NULL marker is the 2-byte sequence `\N`. A non-NULL String value
 * whose unescaped content happens to start with `\N` (or be exactly `\N`)
 * must not be misread as NULL.
 */
SELECT clickhouse_raw_query('CREATE TABLE http_test.t_nullmark
    (id String, s String) ENGINE = MergeTree ORDER BY id');
SELECT clickhouse_raw_query(
    'INSERT INTO http_test.t_nullmark VALUES'
    || ' (''1'', ''\\N''),'             /* exactly the 2-char NULL marker as data */
    || ' (''2'', ''\\N foo bar''),'     /* starts with \N */
    || ' (''3'', ''leading text \\N''),'/* contains \N at end */
    || ' (''4'', ''ordinary'')');

CREATE FOREIGN TABLE ft_nullmark (id text, s text)
    SERVER http_loopback OPTIONS (table_name 't_nullmark');

SELECT id, s, length(s), s IS NULL AS is_null
FROM ft_nullmark ORDER BY id;

/* nested arrays via http (TabSeparated): rectangular maps to multi-dim,
 * jagged shapes route through array_in and surface its malformed-literal
 * error -- matching the binary path. */
SELECT clickhouse_raw_query('CREATE TABLE http_test.nested_arrays (
    c1 Int8, c2 Array(Array(Int32)), c3 Array(Array(String))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO http_test.nested_arrays VALUES
    (1, [[1,2],[3,4]], [[''a'',''b''],[''c'',''d'']]),
    (2, [[5,6],[7,8]], [[''e'',''f''],[''g'',''h'']]);
');

SELECT clickhouse_raw_query('CREATE TABLE http_test.ragged_arrays (
    c1 Int8, c2 Array(Array(Int32))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO http_test.ragged_arrays VALUES (1, [[1,2,3],[4]]);');

CREATE FOREIGN TABLE ft_nested_arrays (
    c1 int2,
    c2 int[],
    c3 text[]
) SERVER http_loopback OPTIONS (table_name 'nested_arrays');

CREATE FOREIGN TABLE ft_ragged_arrays (
    c1 int2,
    c2 int[]
) SERVER http_loopback OPTIONS (table_name 'ragged_arrays');

SELECT * FROM ft_nested_arrays ORDER BY c1;
SELECT * FROM ft_ragged_arrays ORDER BY c1;

DROP USER MAPPING FOR CURRENT_USER SERVER http_no_stream;
DROP SERVER http_no_stream CASCADE;

DROP USER MAPPING FOR CURRENT_USER SERVER http_loopback_bad;
DROP USER MAPPING FOR CURRENT_USER SERVER http_loopback2;
DROP USER MAPPING FOR CURRENT_USER SERVER http_loopback;
SELECT clickhouse_raw_query('DROP DATABASE http_test');
DROP SERVER http_loopback_bad CASCADE;
DROP SERVER http_loopback2 CASCADE;
DROP SERVER http_loopback CASCADE;
