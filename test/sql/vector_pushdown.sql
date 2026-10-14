SELECT NOT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 'vector') AS no_vector \gset
\if :no_vector
\echo 'SKIP: vector extension not available'
\quit
\endif

-- Push down pgvector distance operators and functions, comparing results with
-- pgvector on a local copy of the same rows.
-- Concurrent tests create pgvector; serialize and silence "already exists"
SET client_min_messages = warning;
SELECT pg_advisory_lock(hashtext('vector'));
CREATE EXTENSION IF NOT EXISTS vector;
SELECT pg_advisory_unlock(hashtext('vector'));
RESET client_min_messages;

CREATE SERVER vector_pushdown_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'vector_pushdown_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_pushdown_svr;

CREATE SERVER vector_pushdown_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_pushdown_admin;

CALL clickhouse_perform('vector_pushdown_admin', 'DROP DATABASE IF EXISTS vector_pushdown_test');
CALL clickhouse_perform('vector_pushdown_admin', 'CREATE DATABASE vector_pushdown_test');
CALL clickhouse_perform('vector_pushdown_admin', $$
    CREATE TABLE vector_pushdown_test.items (
        id       Int32,
        category String,
        v        Array(Float32)
    ) ENGINE = MergeTree ORDER BY (id);
$$);

CREATE FOREIGN TABLE vec_items (
    id       int NOT NULL,
    category text NOT NULL,
    v        vector(4) NOT NULL
) SERVER vector_pushdown_svr OPTIONS (table_name 'items');

-- Deterministic vectors with distinct distances, plus a zero vector, whose
-- cosine distance is NaN.
INSERT INTO vec_items
SELECT i, (ARRAY['a', 'b', 'c'])[i % 3 + 1],
       format('[%s,%s,%s,%s]', sin(i), cos(i), sin(i * 2), cos(i * 3))::vector
  FROM generate_series(1, 60) i;
INSERT INTO vec_items VALUES (0, 'a', '[0,0,0,0]');

CREATE TEMP TABLE local_items AS SELECT * FROM vec_items;

\set q '[0.3,-0.2,0.5,0.1]'

-- ORDER BY each distance operator with LIMIT. <#> is the negative inner
-- product, so ascending order sorts dotProduct descending.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <-> :'q' LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <=> :'q' LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <#> :'q' LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <+> :'q' LIMIT 5;

SELECT id, round((v <-> :'q')::numeric, 4) AS dist FROM vec_items ORDER BY v <-> :'q' LIMIT 5;
SELECT id, round((v <-> :'q')::numeric, 4) AS dist FROM local_items ORDER BY v <-> :'q' LIMIT 5;

-- The top ten match pgvector's for every operator.

SELECT (SELECT array_agg(id) FROM (SELECT id FROM vec_items ORDER BY v <-> :'q' LIMIT 10) r)
     = (SELECT array_agg(id) FROM (SELECT id FROM local_items ORDER BY v <-> :'q' LIMIT 10) l) AS l2,
       (SELECT array_agg(id) FROM (SELECT id FROM vec_items ORDER BY v <=> :'q' LIMIT 10) r)
     = (SELECT array_agg(id) FROM (SELECT id FROM local_items ORDER BY v <=> :'q' LIMIT 10) l) AS cosine,
       (SELECT array_agg(id) FROM (SELECT id FROM vec_items ORDER BY v <#> :'q' LIMIT 10) r)
     = (SELECT array_agg(id) FROM (SELECT id FROM local_items ORDER BY v <#> :'q' LIMIT 10) l) AS neg_ip,
       (SELECT array_agg(id) FROM (SELECT id FROM vec_items ORDER BY v <+> :'q' LIMIT 10) r)
     = (SELECT array_agg(id) FROM (SELECT id FROM local_items ORDER BY v <+> :'q' LIMIT 10) l) AS l1;

-- Descending order. NaN sorts first, as in Postgres.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <#> :'q' DESC LIMIT 3;
SELECT id FROM vec_items ORDER BY v <=> :'q' DESC LIMIT 3;
SELECT id FROM local_items ORDER BY v <=> :'q' DESC LIMIT 3;
SELECT (SELECT array_agg(id) FROM (SELECT id FROM vec_items ORDER BY v <#> :'q' DESC LIMIT 10) r)
     = (SELECT array_agg(id) FROM (SELECT id FROM local_items ORDER BY v <#> :'q' DESC LIMIT 10) l) AS neg_ip_desc;

-- Distances in the select list and WHERE clause.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, v <=> :'q' AS dist FROM vec_items ORDER BY v <=> :'q' LIMIT 3;
SELECT id, round((v <=> :'q')::numeric, 4) AS dist FROM vec_items ORDER BY v <=> :'q' LIMIT 3;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM vec_items WHERE v <-> :'q' < 1.2;
SELECT (SELECT count(*) FROM vec_items WHERE v <-> :'q' < 1.2)
     = (SELECT count(*) FROM local_items WHERE v <-> :'q' < 1.2) AS l2,
       (SELECT count(*) FROM vec_items WHERE v <#> :'q' < -0.3)
     = (SELECT count(*) FROM local_items WHERE v <#> :'q' < -0.3) AS neg_ip,
       (SELECT count(*) FROM vec_items WHERE v <+> :'q' < 2)
     = (SELECT count(*) FROM local_items WHERE v <+> :'q' < 2) AS l1;

-- Filters, OFFSET, and tiebreakers push down with the sort and limit.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items WHERE category = 'b' ORDER BY v <-> :'q' LIMIT 3 OFFSET 1;
SELECT id FROM vec_items WHERE category = 'b' ORDER BY v <-> :'q' LIMIT 3 OFFSET 1;
SELECT id FROM local_items WHERE category = 'b' ORDER BY v <-> :'q' LIMIT 3 OFFSET 1;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <=> :'q', id LIMIT 5;

-- Parameters for the vector and the limit.
SET plan_cache_mode = force_generic_plan;
PREPARE knn(vector, int) AS SELECT id FROM vec_items ORDER BY v <-> $1 LIMIT $2;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE knn('[0.3,-0.2,0.5,0.1]', 3);
EXECUTE knn('[0.3,-0.2,0.5,0.1]', 3);
DEALLOCATE knn;
RESET plan_cache_mode;

-- pgvector functions push down in conditions and sorts. Those without an
-- exact ClickHouse equivalent stay local: l2_normalize() of a zero vector is
-- NaN in ClickHouse, L2Norm() takes arrays only in newer ClickHouse versions,
-- and arraySlice() reads negative offsets from the end.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM vec_items
 WHERE l2_distance(v, :'q') < 1.2 AND cosine_distance(v, :'q') < 0.9
   AND l1_distance(v, :'q') < 2.5 AND vector_dims(v) = 4;
SELECT (SELECT count(*) FROM vec_items
         WHERE l2_distance(v, :'q') < 1.2 AND cosine_distance(v, :'q') < 0.9
           AND l1_distance(v, :'q') < 2.5 AND vector_dims(v) = 4)
     = (SELECT count(*) FROM local_items
         WHERE l2_distance(v, :'q') < 1.2 AND cosine_distance(v, :'q') < 0.9
           AND l1_distance(v, :'q') < 2.5 AND vector_dims(v) = 4) AS same_count;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY inner_product(v, :'q') DESC LIMIT 3;
SELECT id FROM vec_items ORDER BY inner_product(v, :'q') DESC LIMIT 3;
SELECT id FROM local_items ORDER BY inner_product(v, :'q') DESC LIMIT 3;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items
 WHERE vector_norm(v) > 1 AND l2_normalize(v) <> v AND subvector(v, 1, 2) <> '[0,0]';

-- A sort on a non-shippable expression stays local.
SET vector_pushdown.q = '[0.3,-0.2,0.5,0.1]';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items ORDER BY v <-> current_setting('vector_pushdown.q')::vector LIMIT 3;
RESET vector_pushdown.q;

-- halfvec distances stay local.
CREATE FOREIGN TABLE vec_items_half (
    id int NOT NULL,
    v  halfvec(4) NOT NULL
) SERVER vector_pushdown_svr OPTIONS (table_name 'items');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM vec_items_half ORDER BY v <-> '[0.3,-0.2,0.5,0.1]' LIMIT 3;

CALL clickhouse_perform('vector_pushdown_admin', 'DROP DATABASE vector_pushdown_test');
DROP USER MAPPING FOR CURRENT_USER SERVER vector_pushdown_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER vector_pushdown_admin;
DROP SERVER vector_pushdown_svr CASCADE;
DROP SERVER vector_pushdown_admin CASCADE;
