SET datestyle = 'ISO';
CREATE SERVER geo_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER geo_bin_svr;

CREATE SERVER geo_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER geo_http_svr;

CALL clickhouse_perform('geo_http_svr', 'DROP DATABASE IF EXISTS geo_test');
CALL clickhouse_perform('geo_http_svr', 'CREATE DATABASE geo_test');
CALL clickhouse_perform('geo_http_svr', $$
    CREATE TABLE geo_test.shapes (
        id Int32,
        p  Point,
        r  Ring
    ) ENGINE = MergeTree ORDER BY id
$$);

CALL clickhouse_perform('geo_http_svr', $$
    INSERT INTO geo_test.shapes VALUES
        (1, (1.5, 2), [(0, 0), (1, 0), (1, 1)]),
        (2, (-0., 1e-7), []),
        (3, (inf, -3), [(2, 2)])
$$);

-- ===================================================================
-- binary
-- ===================================================================
CREATE SCHEMA geo_bin;
CREATE FOREIGN TABLE geo_bin.shapes (id int, p point, r polygon)
    SERVER geo_bin_svr OPTIONS (database 'geo_test', table_name 'shapes');

SELECT id, p, r FROM geo_bin.shapes ORDER BY id;

-- point subscripts push down as tupleElement
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, p[0], p[1] FROM geo_bin.shapes WHERE p[0] > 1 ORDER BY id;
SELECT id, p[0], p[1] FROM geo_bin.shapes WHERE p[0] > 1 ORDER BY id;

-- out of range subscript is NULL, evaluated locally
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_bin.shapes WHERE p[2] IS NULL ORDER BY id;
SELECT id FROM geo_bin.shapes WHERE p[2] IS NULL ORDER BY id;

-- point literal
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_bin.shapes
 WHERE (CASE WHEN id = 1 THEN p ELSE point '(-0.5,1e300)' END)[1] > 2 ORDER BY id;
SELECT id FROM geo_bin.shapes
 WHERE (CASE WHEN id = 1 THEN p ELSE point '(-0.5,1e300)' END)[1] > 2 ORDER BY id;

-- point parameter
SET plan_cache_mode = force_generic_plan;
PREPARE geo1(point) AS SELECT id FROM geo_bin.shapes WHERE p[0] = $1[0] ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE geo1('(1.5,7)');
EXECUTE geo1('(1.5,7)');
EXECUTE geo1('(Infinity,0)');
DEALLOCATE geo1;
RESET plan_cache_mode;

-- geometric operators, casts and NULL tests on polygon run locally
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_bin.shapes WHERE p ~= point '(1.5000001,2)' ORDER BY id;
SELECT id FROM geo_bin.shapes WHERE p ~= point '(1.5000001,2)' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_bin.shapes WHERE p::text = '(1.5,2)' ORDER BY id;
SELECT id FROM geo_bin.shapes WHERE p::text = '(1.5,2)' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_bin.shapes WHERE r IS NULL ORDER BY id;
SELECT id FROM geo_bin.shapes WHERE r IS NULL ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_bin.shapes WHERE npoints(r) = 1 ORDER BY id;
SELECT id FROM geo_bin.shapes WHERE npoints(r) = 1 ORDER BY id;

-- ===================================================================
-- http
-- ===================================================================
CREATE SCHEMA geo_http;
CREATE FOREIGN TABLE geo_http.shapes (id int, p point, r polygon)
    SERVER geo_http_svr OPTIONS (database 'geo_test', table_name 'shapes');

SELECT id, p, r FROM geo_http.shapes ORDER BY id;

-- point subscripts push down as tupleElement
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, p[0], p[1] FROM geo_http.shapes WHERE p[0] > 1 ORDER BY id;
SELECT id, p[0], p[1] FROM geo_http.shapes WHERE p[0] > 1 ORDER BY id;

-- out of range subscript is NULL, evaluated locally
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_http.shapes WHERE p[2] IS NULL ORDER BY id;
SELECT id FROM geo_http.shapes WHERE p[2] IS NULL ORDER BY id;

-- point literal
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_http.shapes
 WHERE (CASE WHEN id = 1 THEN p ELSE point '(-0.5,1e300)' END)[1] > 2 ORDER BY id;
SELECT id FROM geo_http.shapes
 WHERE (CASE WHEN id = 1 THEN p ELSE point '(-0.5,1e300)' END)[1] > 2 ORDER BY id;

-- point parameter
SET plan_cache_mode = force_generic_plan;
PREPARE geo1(point) AS SELECT id FROM geo_http.shapes WHERE p[0] = $1[0] ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE geo1('(1.5,7)');
EXECUTE geo1('(1.5,7)');
EXECUTE geo1('(Infinity,0)');
DEALLOCATE geo1;
RESET plan_cache_mode;

-- geometric operators, casts and NULL tests on polygon run locally
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_http.shapes WHERE p ~= point '(1.5000001,2)' ORDER BY id;
SELECT id FROM geo_http.shapes WHERE p ~= point '(1.5000001,2)' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_http.shapes WHERE p::text = '(1.5,2)' ORDER BY id;
SELECT id FROM geo_http.shapes WHERE p::text = '(1.5,2)' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_http.shapes WHERE r IS NULL ORDER BY id;
SELECT id FROM geo_http.shapes WHERE r IS NULL ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM geo_http.shapes WHERE npoints(r) = 1 ORDER BY id;
SELECT id FROM geo_http.shapes WHERE npoints(r) = 1 ORDER BY id;

-- Clean up.
CALL clickhouse_perform('geo_http_svr', 'DROP DATABASE geo_test');
DROP SCHEMA geo_bin CASCADE;
DROP SCHEMA geo_http CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER geo_bin_svr;
DROP SERVER geo_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER geo_http_svr;
DROP SERVER geo_http_svr CASCADE;
