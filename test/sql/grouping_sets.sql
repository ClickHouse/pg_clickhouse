\unset ECHO
SET client_min_messages = notice;

CREATE SERVER gs_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'gs_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER gs_bin_svr;

CREATE SERVER gs_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'gs_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER gs_http_svr;

CREATE SERVER gs_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER gs_admin;

CALL clickhouse_perform('gs_admin', 'DROP DATABASE IF EXISTS gs_test');
CALL clickhouse_perform('gs_admin', 'CREATE DATABASE gs_test');
CALL clickhouse_perform('gs_admin', $$
    CREATE TABLE gs_test.sales (
        region  String,
        product String,
        qty     Int32,
        price   Nullable(Float64),
        tags    Array(Int32)
    ) ENGINE = MergeTree ORDER BY (region, product);
$$);
CALL clickhouse_perform('gs_admin', $$
    INSERT INTO gs_test.sales VALUES
        ('east', 'apple',  3, 1.5,  [1]),
        ('east', 'pear',   5, NULL, [2]),
        ('west', 'apple',  7, 2.5,  [1]),
        ('west', 'plum',   1, 4,    []),
        ('north', 'pear',  2, 3,    [2])
$$);
CALL clickhouse_perform('gs_admin', $$
    CREATE TABLE gs_test.empty (a Int32, b Int32) ENGINE = MergeTree ORDER BY a;
$$);

CREATE SCHEMA gs_bin;
CREATE SCHEMA gs_http;
IMPORT FOREIGN SCHEMA "gs_test" FROM SERVER gs_bin_svr INTO gs_bin;
IMPORT FOREIGN SCHEMA "gs_test" FROM SERVER gs_http_svr INTO gs_http;

-- Compare ClickHouse results with local PostgreSQL tables
CREATE TABLE gs_local_sales AS SELECT * FROM gs_bin.sales;
CREATE TABLE gs_local_empty (a int, b int);

-- Check ROLLUP, CUBE and nested grouping sets run in ClickHouse
EXPLAIN (VERBOSE, COSTS OFF)
SELECT region, product, sum(qty) FROM gs_bin.sales
 GROUP BY ROLLUP (region, product) ORDER BY 1, 2;
SELECT region, product, sum(qty) FROM gs_bin.sales
 GROUP BY ROLLUP (region, product) ORDER BY 1, 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT region, product, count(*) FROM gs_bin.sales
 GROUP BY CUBE (region, product) ORDER BY 1, 2;
SELECT region, product, count(*) FROM gs_bin.sales
 GROUP BY CUBE (region, product) ORDER BY 1, 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT region, product, qty, count(*) FROM gs_bin.sales
 GROUP BY region, GROUPING SETS ((product), ROLLUP (qty)) ORDER BY 1, 2, 3;
SELECT region, product, qty, count(*) FROM gs_bin.sales
 GROUP BY region, GROUPING SETS ((product), ROLLUP (qty)) ORDER BY 1, 2, 3;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT region, product, grouping(region, product), sum(price) FROM gs_bin.sales
 GROUP BY GROUPING SETS ((region, product), (region), ())
HAVING grouping(product) = 1 ORDER BY 1;
SELECT region, product, grouping(region, product), sum(price) FROM gs_bin.sales
 GROUP BY GROUPING SETS ((region, product), (region), ())
HAVING grouping(product) = 1 ORDER BY 1;

-- Evaluate expressions on keys left out of a grouping set
EXPLAIN (VERBOSE, COSTS OFF)
SELECT upper(region), coalesce(region, 'all'), max(qty) FROM gs_bin.sales
 GROUP BY ROLLUP (region) ORDER BY 1;
SELECT upper(region), coalesce(region, 'all'), max(qty) FROM gs_bin.sales
 GROUP BY ROLLUP (region) ORDER BY 1;

-- Compare results in both directions, including duplicates
SELECT count(*) AS mismatches FROM (
    (SELECT region, product, grouping(region, product) g, sum(qty), count(price),
            avg(price), min(product), max(qty)
       FROM gs_bin.sales GROUP BY CUBE (region, product)
     EXCEPT ALL
     SELECT region, product, grouping(region, product) g, sum(qty), count(price),
            avg(price), min(product), max(qty)
       FROM gs_local_sales GROUP BY CUBE (region, product))
    UNION ALL
    (SELECT region, product, grouping(region, product) g, sum(qty), count(price),
            avg(price), min(product), max(qty)
       FROM gs_local_sales GROUP BY CUBE (region, product)
     EXCEPT ALL
     SELECT region, product, grouping(region, product) g, sum(qty), count(price),
            avg(price), min(product), max(qty)
       FROM gs_bin.sales GROUP BY CUBE (region, product))
) d;

-- Keep duplicate groups for duplicate grouping sets
SELECT region, count(*) FROM gs_http.sales
 GROUP BY GROUPING SETS ((region), (region), (), ()) ORDER BY 1, 2;

-- Return one row with NULL sum for an empty grouping set on an empty table
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a, count(*), sum(b) FROM gs_bin.empty GROUP BY GROUPING SETS ((a), ());
SELECT a, count(*), sum(b) FROM gs_bin.empty GROUP BY GROUPING SETS ((a), ());
SELECT a, count(*), sum(b) FROM gs_local_empty GROUP BY GROUPING SETS ((a), ());
SELECT count(*) FROM gs_http.empty GROUP BY GROUPING SETS ((), ());

-- Return an empty array instead of NULL when a grouping set omits an array key
SELECT tags, count(*) FROM gs_bin.sales GROUP BY ROLLUP (tags) ORDER BY 2, 1;

DROP TABLE gs_local_sales, gs_local_empty;
CALL clickhouse_perform('gs_admin', 'DROP DATABASE gs_test');
DROP USER MAPPING FOR CURRENT_USER SERVER gs_bin_svr;
DROP SERVER gs_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER gs_http_svr;
DROP SERVER gs_http_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER gs_admin;
DROP SERVER gs_admin CASCADE;
