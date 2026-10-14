SELECT NOT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 'vector') AS no_vector \gset
\if :no_vector
\echo 'SKIP: vector extension not available'
\quit
\endif

-- Read and write pgvector vector columns stored in ClickHouse float arrays,
-- and push down vector constants, parameters, and equality.
-- Concurrent tests create pgvector; serialize and silence "already exists"
SET client_min_messages = warning;
SELECT pg_advisory_lock(hashtext('vector'));
CREATE EXTENSION IF NOT EXISTS vector;
SELECT pg_advisory_unlock(hashtext('vector'));
RESET client_min_messages;

CREATE SERVER vector_types_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'vector_types_test', driver 'binary');
CREATE SERVER vector_types_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'vector_types_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_types_bin;
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_types_http;

CREATE SERVER vector_types_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_types_admin;

CALL clickhouse_perform('vector_types_admin', 'DROP DATABASE IF EXISTS vector_types_test');
CALL clickhouse_perform('vector_types_admin', 'CREATE DATABASE vector_types_test');

CALL clickhouse_perform('vector_types_admin', $$
    CREATE TABLE vector_types_test.items (
        id  Int32,
        f32 Array(Float32),
        f64 Array(Float64)
    ) ENGINE = MergeTree ORDER BY (id);
$$);

CREATE FOREIGN TABLE items_bin (
    id  int NOT NULL,
    f32 vector(3) NOT NULL,
    f64 vector(3) NOT NULL
) SERVER vector_types_bin OPTIONS (table_name 'items');

CREATE FOREIGN TABLE items_http (
    id  int NOT NULL,
    f32 vector(3) NOT NULL,
    f64 vector(3) NOT NULL
) SERVER vector_types_http OPTIONS (table_name 'items');

-- Insert through both drivers.
INSERT INTO items_bin VALUES
    (1, '[0.1,0.2,0.3]', '[0.1,0.2,0.3]'),
    (2, '[-1.5,0,2.25]', '[-1.5,0,2.25]');
INSERT INTO items_http VALUES
    (3, '[1e-7,-3.4028235e38,123456.79]', '[1e-7,-3.4028235e38,123456.79]'),
    (4, '[0.1,0.2,0.3]', '[0.1,0.2,0.3]');

-- COPY and INSERT ... SELECT from a local table.
COPY items_bin FROM STDIN;
5	[7,8,9]	[7,8,9]
\.

CREATE TEMP TABLE local_items (id int, f32 vector(3), f64 vector(3));
INSERT INTO local_items VALUES (6, '[0.5,0.25,0.125]', '[0.5,0.25,0.125]');
INSERT INTO items_http SELECT * FROM local_items;

-- Read through both drivers. Values round-trip as the same float4s.
SELECT * FROM items_bin ORDER BY id;
SELECT * FROM items_http ORDER BY id;
SELECT f32::real[], f64::real[] FROM items_bin WHERE id = 1;

-- What ClickHouse stored.
SELECT * FROM clickhouse_query('vector_types_admin', $$
    SELECT id, toString(f32), toString(f64) FROM vector_types_test.items ORDER BY id
$$) AS t(id int, f32 text, f64 text);

-- Equality pushes down with a Float32-typed literal, so it matches the
-- Float32 values ClickHouse stored rather than comparing them to Float64.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM items_bin WHERE f32 = '[0.1,0.2,0.3]';
SELECT id FROM items_bin WHERE f32 = '[0.1,0.2,0.3]' ORDER BY id;
SELECT id FROM items_http WHERE f32 = '[0.1,0.2,0.3]' ORDER BY id;
SELECT id FROM items_bin WHERE f64 = '[0.1,0.2,0.3]' ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM items_bin WHERE f32 <> '[0.1,0.2,0.3]';
SELECT id FROM items_bin WHERE f32 <> '[0.1,0.2,0.3]' ORDER BY id;

-- Parameters go to ClickHouse as Array(Float32).
SET plan_cache_mode = force_generic_plan;
PREPARE vec_eq(vector) AS SELECT id FROM items_bin WHERE f32 = $1 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE vec_eq('[0.1,0.2,0.3]');
EXECUTE vec_eq('[0.1,0.2,0.3]');
EXECUTE vec_eq('[7,8,9]');
DEALLOCATE vec_eq;

PREPARE vec_eq_http(vector) AS SELECT id FROM items_http WHERE f32 = $1 ORDER BY id;
EXECUTE vec_eq_http('[0.5,0.25,0.125]');
DEALLOCATE vec_eq_http;
RESET plan_cache_mode;

-- Operators without a ClickHouse mapping stay local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM items_bin WHERE f32 < '[1,1,1]';
SELECT id FROM items_bin WHERE f32 < '[1,1,1]' ORDER BY id;

-- halfvec is not pushed down; Postgres evaluates it locally. Skip row 3,
-- whose values are out of the halfvec range.
CREATE FOREIGN TABLE items_half (
    id  int NOT NULL,
    f32 halfvec(3) NOT NULL
) SERVER vector_types_bin OPTIONS (table_name 'items');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM items_half WHERE id <> 3 AND f32 = '[7,8,9]';
SELECT id FROM items_half WHERE id <> 3 AND f32 = '[7,8,9]';

-- BFloat16 arrays read as vectors, but writes truncate float4 to bfloat16.
-- Use values bfloat16 represents exactly.
\set ECHO errors
SELECT (regexp_split_to_array(clickhouse_server_version('vector_types_admin'), '\.'))[1:2]::int[]
    >= '{25,8}'::int[] AS ch_bf16 \gset
\set ECHO all
\if :ch_bf16
CALL clickhouse_perform('vector_types_admin', $$
    CREATE TABLE vector_types_test.bf16 (id Int32, v Array(BFloat16)) ENGINE = MergeTree ORDER BY (id);
$$);
CREATE FOREIGN TABLE bf16_bin (id int, v vector(3)) SERVER vector_types_bin OPTIONS (table_name 'bf16');
CREATE FOREIGN TABLE bf16_http (id int, v vector(3)) SERVER vector_types_http OPTIONS (table_name 'bf16');
INSERT INTO bf16_bin VALUES (1, '[0.5,1.25,-2]');
INSERT INTO bf16_http VALUES (2, '[3,-0.75,1024]');
SELECT * FROM bf16_bin ORDER BY id;
SELECT * FROM bf16_http ORDER BY id;
SELECT id FROM bf16_bin WHERE v = '[0.5,1.25,-2]';
DROP FOREIGN TABLE bf16_bin, bf16_http;
\endif

CALL clickhouse_perform('vector_types_admin', 'DROP DATABASE vector_types_test');
DROP USER MAPPING FOR CURRENT_USER SERVER vector_types_bin;
DROP USER MAPPING FOR CURRENT_USER SERVER vector_types_http;
DROP USER MAPPING FOR CURRENT_USER SERVER vector_types_admin;
DROP SERVER vector_types_bin CASCADE;
DROP SERVER vector_types_http CASCADE;
DROP SERVER vector_types_admin CASCADE;
