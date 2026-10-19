-- Read, write, and search ClickHouse QBit columns as float arrays.
CREATE SERVER vector_qbit_svr FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE SERVER vector_qbit_http_svr FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS (driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_qbit_svr;
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_qbit_http_svr;

\set ECHO errors
SELECT count(*) = 0 OR NOT EXISTS (
    SELECT 1 FROM pg_available_extensions WHERE name = 'vector'
) AS no_qbit FROM clickhouse_query('vector_qbit_svr', $$
    SELECT name FROM system.data_type_families WHERE name = 'QBit'
$$) AS t(name text) \gset
\set ECHO all
\if :no_qbit
\echo 'SKIP: ClickHouse has no QBit type or pgvector is not available'
\quit
\endif

CALL clickhouse_perform('vector_qbit_svr', 'DROP DATABASE IF EXISTS vector_qbit_test');
CALL clickhouse_perform('vector_qbit_svr', 'CREATE DATABASE vector_qbit_test');
CALL clickhouse_perform('vector_qbit_svr', $$
    CREATE TABLE vector_qbit_test.docs (
        id Int32, f QBit(Float32, 9), b QBit(BFloat16, 3), d QBit(Float64, 3)
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('vector_qbit_svr', $$
    INSERT INTO vector_qbit_test.docs VALUES
        (1, [1.5, -2, 3, 0, 0, 0, 0, 0, 0.1], [0.5, 1.25, -2], [0.1, 2.5, -3]),
        (2, [0, 0, 0, 0, 0, 0, 0, 0, 1], [1, 0, 0], [1, 0, 0])
$$);

-- IMPORT FOREIGN SCHEMA maps QBit to an array of its element type.
CREATE SCHEMA vector_qbit;
IMPORT FOREIGN SCHEMA vector_qbit_test FROM SERVER vector_qbit_svr INTO vector_qbit;
SELECT attname, format_type(atttypid, atttypmod) FROM pg_attribute
 WHERE attrelid = 'vector_qbit.docs'::regclass AND attnum > 0 ORDER BY attnum;
CREATE FOREIGN TABLE qbit_http (id int, f real[], b real[], d float8[])
    SERVER vector_qbit_http_svr OPTIONS (database 'vector_qbit_test', table_name 'docs');

-- Both drivers read and write QBit values, which ClickHouse reads back.
SELECT * FROM vector_qbit.docs ORDER BY id;
SELECT * FROM qbit_http ORDER BY id;
INSERT INTO vector_qbit.docs VALUES (3, '{0,1,0,0,0,0,0,0,-1}', '{0,1,0}', '{0,1,0}');
INSERT INTO qbit_http VALUES (4, '{0,0,1,0,0,0,0,0,-2}', '{0,0,1}', '{0,0,1}');
SELECT * FROM clickhouse_query('vector_qbit_svr', $$
    SELECT id, toString(f), toString(b), toString(d)
      FROM vector_qbit_test.docs WHERE id > 2 ORDER BY id
$$) AS t(id int, f text, b text, d text);

-- Transposed distances push down. ClickHouse computes the sort key, since
-- PostgreSQL cannot, and does not return the vectors.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vector_qbit.docs ORDER BY L2DistanceTransposed(d, '{1,0,0.1}'::float8[], 64) LIMIT 2;
SELECT id FROM vector_qbit.docs ORDER BY L2DistanceTransposed(d, '{1,0,0.1}'::float8[], 64) LIMIT 2;
SELECT id, round(distance::numeric, 4) FROM (
    SELECT id, cosineDistanceTransposed(b, '{0,1,0}'::real[], 16) AS distance
      FROM vector_qbit.docs ORDER BY distance LIMIT 2
) AS nearest;

-- The bits argument can be a parameter, as in a generic plan.
PREPARE qbit_search(int) AS
    SELECT id FROM vector_qbit.docs ORDER BY L2DistanceTransposed(d, '{1,0,0.1}'::float8[], $1) LIMIT 2;
SET plan_cache_mode = force_generic_plan;
EXECUTE qbit_search(64);
RESET plan_cache_mode;

-- pgvector distances on a vector column over QBit push down as transposed
-- functions with the column first, reading every bit. Others stay local:
-- L1Distance has no transposed form, older ClickHouse versions lack
-- dotProductTransposed(), and a BFloat16 QBit rounds the query vector.
SET client_min_messages = warning;
SELECT pg_advisory_lock(hashtext('vector'));
CREATE EXTENSION IF NOT EXISTS vector;
SELECT pg_advisory_unlock(hashtext('vector'));
RESET client_min_messages;
CREATE FOREIGN TABLE qbit_vec (id int, f vector(9), b vector(3), d vector(3))
    SERVER vector_qbit_svr OPTIONS (database 'vector_qbit_test', table_name 'docs');
CREATE TEMP TABLE qbit_local AS SELECT * FROM qbit_vec;
\set q '[0.3,0.9,0.1]'
\set q9 '[1,0,1,0,0,0,0,0,0.5]'
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, d <-> :'q' AS dist FROM qbit_vec WHERE cosine_distance(:'q', d) < 0.9 ORDER BY dist LIMIT 3;
SET pg_clickhouse.vector_planner_warnings = off;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM qbit_vec WHERE b <-> :'q' < 2 AND f <+> :'q9' < 9 ORDER BY f <=> :'q9' LIMIT 3;

-- The distances match pgvector's on a local copy.
SELECT id, round((d <-> :'q')::numeric, 4) AS l2, round((f <=> :'q9')::numeric, 4) AS cosine,
       round((f <#> :'q9')::numeric, 4) AS neg_ip, round(inner_product(f, :'q9')::numeric, 4) AS ip
  FROM qbit_vec ORDER BY d <-> :'q', f <=> :'q9', f <#> :'q9', inner_product(f, :'q9') LIMIT 4;
SELECT id, round((d <-> :'q')::numeric, 4) AS l2, round((f <=> :'q9')::numeric, 4) AS cosine,
       round((f <#> :'q9')::numeric, 4) AS neg_ip, round(inner_product(f, :'q9')::numeric, 4) AS ip
  FROM qbit_local ORDER BY d <-> :'q', f <=> :'q9', f <#> :'q9', inner_product(f, :'q9') LIMIT 4;
SELECT (SELECT count(*) FROM qbit_vec WHERE b <-> :'q' < 1.2 AND f <#> :'q9' < 0 AND l1_distance(d, :'q') < 2)
     = (SELECT count(*) FROM qbit_local WHERE b <-> :'q' < 1.2 AND f <#> :'q9' < 0 AND l1_distance(d, :'q') < 2) AS filters;
RESET pg_clickhouse.vector_planner_warnings;

CALL clickhouse_perform('vector_qbit_svr', 'DROP DATABASE vector_qbit_test');
DROP SCHEMA vector_qbit CASCADE;
DROP SERVER vector_qbit_svr, vector_qbit_http_svr CASCADE;
