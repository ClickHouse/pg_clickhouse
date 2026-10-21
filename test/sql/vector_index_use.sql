SELECT NOT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 'vector') AS no_vector \gset
\if :no_vector
\echo 'SKIP: vector extension not available'
\quit
\endif

-- Report whether ClickHouse can serve a vector search from a vector
-- similarity index, and why not, from the indexes ClickHouse reports.
SET client_min_messages = warning;
SELECT pg_advisory_lock(hashtext('vector'));
CREATE EXTENSION IF NOT EXISTS vector;
SELECT pg_advisory_unlock(hashtext('vector'));
RESET client_min_messages;

CREATE SERVER vector_index_use_svr FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER vector_index_use_svr;

\set ECHO errors
SELECT (regexp_split_to_array(clickhouse_server_version('vector_index_use_svr'), '\.'))[1:2]::int[]
    < '{25,8}'::int[] AS old_ch \gset
\set ECHO all
\if :old_ch
\echo 'SKIP: vector similarity indexes require ClickHouse 25.8'
\quit
\endif

CALL clickhouse_perform('vector_index_use_svr', 'DROP DATABASE IF EXISTS vector_index_use_test');
CALL clickhouse_perform('vector_index_use_svr', 'CREATE DATABASE vector_index_use_test');
CALL clickhouse_perform('vector_index_use_svr', $$
    CREATE TABLE vector_index_use_test.docs (
        id Int32, v Array(Float32), w Array(Float32),
        INDEX v_idx v TYPE vector_similarity('hnsw', 'L2Distance', 2)
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('vector_index_use_svr', $$
    INSERT INTO vector_index_use_test.docs
    SELECT number, [sin(number), cos(number)], [cos(number), sin(number)] FROM numbers(100)
$$);
CREATE FOREIGN TABLE docs (id int, v vector(2), w vector(2))
    SERVER vector_index_use_svr OPTIONS (database 'vector_index_use_test');

-- The index serves a single-key ascending L2 sort with a LIMIT under the cap.
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY v <-> '[1,0]' LIMIT 5;

-- Each of these makes ClickHouse search exactly, with a warning.
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY v <=> '[1,0]' LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY w <-> '[1,0]' LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY v <-> '[1,0]' DESC LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY v <-> '[1,0]', id LIMIT 5;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY v <-> '[1,0]' LIMIT 5000;
SET pg_clickhouse.pushdown_regex = off;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs WHERE id::text ~ '1' ORDER BY v <-> '[1,0]' LIMIT 5;
RESET pg_clickhouse.pushdown_regex;

-- The planner reads indexes added in ClickHouse once the cache is refreshed,
-- however long its time to live.
SET pg_clickhouse.vector_metadata_ttl = '30d';
CALL clickhouse_perform('vector_index_use_svr', $$
    ALTER TABLE vector_index_use_test.docs ADD INDEX w_idx w TYPE vector_similarity('hnsw', 'cosineDistance', 2)
$$);
SET pg_clickhouse.vector_planner_warnings = off;
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY w <=> '[1,0]' LIMIT 5;
SELECT clickhouse_refresh_vector_metadata();
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM docs ORDER BY w <=> '[1,0]' LIMIT 5;
RESET pg_clickhouse.vector_planner_warnings;
RESET pg_clickhouse.vector_metadata_ttl;

-- ClickHouse puts backticks around a column name that needs them.
CALL clickhouse_perform('vector_index_use_svr', $$
    ALTER TABLE vector_index_use_test.docs ADD COLUMN `my v` Array(Float32),
        ADD INDEX my_v_idx `my v` TYPE vector_similarity('hnsw', 'L2Distance', 2)
$$);
CREATE FOREIGN TABLE quoted_docs (id int, v vector(2) OPTIONS (column_name 'my v'))
    SERVER vector_index_use_svr OPTIONS (database 'vector_index_use_test', table_name 'docs');
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM quoted_docs ORDER BY v <-> '[1,0]' LIMIT 5;

-- A ClickHouse server that is down does not fail planning.
CREATE SERVER vector_index_use_down FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (host 'localhost', port '1');
CREATE USER MAPPING FOR PUBLIC SERVER vector_index_use_down;
CREATE FOREIGN TABLE down_docs (id int, v vector(2)) SERVER vector_index_use_down
    OPTIONS (table_name 'docs');
EXPLAIN (VERBOSE, COSTS OFF) SELECT id FROM down_docs ORDER BY v <-> '[1,0]' LIMIT 5;
DROP USER MAPPING FOR PUBLIC SERVER vector_index_use_down;
DROP SERVER vector_index_use_down CASCADE;

CALL clickhouse_perform('vector_index_use_svr', 'DROP DATABASE vector_index_use_test');
DROP USER MAPPING FOR CURRENT_USER SERVER vector_index_use_svr;
DROP SERVER vector_index_use_svr CASCADE;
