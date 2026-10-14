SELECT NOT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 'vector') AS no_vector \gset
\if :no_vector
\echo 'SKIP: vector extension not available'
\quit
\endif

-- Create, rebuild, and drop ClickHouse vector similarity indexes with
-- CREATE INDEX ... USING clickhouse, REINDEX, and DROP INDEX.
SET client_min_messages = warning;
SELECT pg_advisory_lock(hashtext('vector'));
CREATE EXTENSION IF NOT EXISTS vector;
SELECT pg_advisory_unlock(hashtext('vector'));
RESET client_min_messages;

CREATE SERVER vector_index_ddl_svr FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR PUBLIC SERVER vector_index_ddl_svr;

\set ECHO errors
SELECT (regexp_split_to_array(clickhouse_server_version('vector_index_ddl_svr'), '\.'))[1:2]::int[]
    < '{25,8}'::int[] AS old_ch \gset
\set ECHO all
\if :old_ch
\echo 'SKIP: vector similarity indexes require ClickHouse 25.8'
\quit
\endif

CALL clickhouse_perform('vector_index_ddl_svr', 'DROP DATABASE IF EXISTS vector_index_ddl_test');
CALL clickhouse_perform('vector_index_ddl_svr', 'CREATE DATABASE vector_index_ddl_test');
CALL clickhouse_perform('vector_index_ddl_svr', $$
    CREATE TABLE vector_index_ddl_test.docs (id Int32, v Array(Float32)) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('vector_index_ddl_svr', $$
    INSERT INTO vector_index_ddl_test.docs SELECT number, [sin(number), cos(number)] FROM numbers(100)
$$);
CREATE FOREIGN TABLE ddl_docs (id int, v vector(2))
    SERVER vector_index_ddl_svr OPTIONS (database 'vector_index_ddl_test', table_name 'docs');


-- The operator class picks the metric, WITH the build options, and the column
-- type the dimensions. Other columns are ignored: ClickHouse filters on any.
CREATE INDEX ddl_docs_v ON ddl_docs USING clickhouse (id, v vector_cosine_ops)
    WITH (quantization = 'f32', m = 16, ef_construction = 64);
SELECT name, column_name, definition, compressed_bytes > 0 AS built, pending_mutations
  FROM clickhouse_vector_indexes('ddl_docs');
EXPLAIN (COSTS OFF) SELECT id FROM ddl_docs ORDER BY v <=> '[1,0]' LIMIT 3;

-- explain_remote shows ClickHouse's own plan, which names the index it uses.
CREATE FUNCTION ddl_plan(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$
BEGIN RETURN QUERY EXECUTE 'EXPLAIN (COSTS OFF) ' || q; END $$;
SET pg_clickhouse.explain_remote = on;
SELECT count(*) > 0 AS uses_index FROM ddl_plan($$
    SELECT id FROM ddl_docs ORDER BY v <=> '[1,0]' LIMIT 3
$$) line WHERE line LIKE '%Name: ddl_docs_v%';
RESET pg_clickhouse.explain_remote;
CREATE INDEX IF NOT EXISTS ddl_docs_v ON ddl_docs USING clickhouse (v vector_cosine_ops);

-- Only the table owner may index it, and only with USING clickhouse.
CREATE INDEX ON ddl_docs (v);
CREATE INDEX ON ddl_docs USING clickhouse (nope vector_l2_ops);
CREATE ROLE vector_index_ddl_other;
SET ROLE vector_index_ddl_other;
CREATE INDEX ddl_docs_l2 ON ddl_docs USING clickhouse (v vector_l2_ops);
RESET ROLE;
DROP ROLE vector_index_ddl_other;

-- REINDEX rebuilds the index in every part, through a ClickHouse mutation.
REINDEX INDEX ddl_docs_v;
CREATE INDEX ddl_docs_v2 ON ddl_docs USING clickhouse (v vector_l2_ops) WITH (materialize = false);
REINDEX TABLE ddl_docs;
SELECT command FROM clickhouse_query('vector_index_ddl_svr', $$
    SELECT command FROM system.mutations WHERE database = 'vector_index_ddl_test' ORDER BY create_time
$$) AS t(command text);

DROP INDEX ddl_docs_v2;

-- Reject a mixed DROP before changing ClickHouse, in either name order.
CREATE TABLE ddl_local (id int);
CREATE INDEX ddl_local_idx ON ddl_local (id);
DROP INDEX ddl_docs_v, ddl_local_idx;
DROP INDEX ddl_local_idx, ddl_docs_v;
SELECT name FROM clickhouse_vector_indexes('ddl_docs');
SELECT to_regclass('ddl_local_idx') IS NOT NULL AS local_index_exists;
SELECT * FROM clickhouse_vector_indexes('ddl_local');
DROP TABLE ddl_local;

-- ClickHouse reads a backslash in a quoted name as an escape.
CREATE INDEX "ddl\idx" ON ddl_docs USING clickhouse (v vector_l2_ops) WITH (materialize = false);
SELECT name FROM clickhouse_vector_indexes('ddl_docs') WHERE name LIKE '%\\%';
DROP INDEX "ddl\idx";
SELECT count(*) FROM clickhouse_vector_indexes('ddl_docs') WHERE name LIKE '%\\%';

-- A missing schema is reported, or skipped, as Postgres does.
DROP INDEX IF EXISTS ddl_no_schema.ddl_docs_v;
DROP INDEX ddl_no_schema.ddl_docs_v;

-- An index name on two remote tables does not say which to drop, but two foreign
-- tables of one remote table share their indexes.
CALL clickhouse_perform('vector_index_ddl_svr', $$
    CREATE TABLE vector_index_ddl_test.docs2 (id Int32, v Array(Float32)) ENGINE = MergeTree ORDER BY id
$$);
CREATE FOREIGN TABLE ddl_docs2 (id int, v vector(2))
    SERVER vector_index_ddl_svr OPTIONS (database 'vector_index_ddl_test', table_name 'docs2');
CREATE INDEX ddl_docs_v ON ddl_docs2 USING clickhouse (v vector_l2_ops);
DROP INDEX ddl_docs_v;
REINDEX INDEX ddl_docs_v;
DROP FOREIGN TABLE ddl_docs2;
CREATE FOREIGN TABLE ddl_docs_dup (id int, v vector(2))
    SERVER vector_index_ddl_svr OPTIONS (database 'vector_index_ddl_test', table_name 'docs');
REINDEX INDEX ddl_docs_v;
DROP FOREIGN TABLE ddl_docs_dup;

DROP INDEX ddl_docs_v;
SELECT count(*) FROM clickhouse_vector_indexes('ddl_docs');

-- ClickHouse prints a column name that needs quoting in backticks.
CALL clickhouse_perform('vector_index_ddl_svr',
    'ALTER TABLE vector_index_ddl_test.docs ADD COLUMN `my v` Array(Float32)');
CREATE FOREIGN TABLE ddl_quoted (id int, v vector(2) OPTIONS (column_name 'my v'))
    SERVER vector_index_ddl_svr OPTIONS (database 'vector_index_ddl_test', table_name 'docs');
CREATE INDEX ON ddl_quoted USING clickhouse (v vector_l2_ops) WITH (materialize = false);
EXPLAIN (COSTS OFF) SELECT id FROM ddl_quoted ORDER BY v <-> '[1,0]' LIMIT 3;

-- A mutation still building one index is not pending for an index whose name it
-- starts with. This one fails on the new column's empty arrays and stays pending.
CREATE INDEX ddl_pm ON ddl_quoted USING clickhouse (v vector_l2_ops) WITH (materialize = false);
CREATE INDEX ddl_pm2 ON ddl_quoted USING clickhouse (v vector_l2_ops) WITH (materialize = false);
CALL clickhouse_perform('vector_index_ddl_svr',
    'ALTER TABLE vector_index_ddl_test.docs MATERIALIZE INDEX ddl_pm2');
SELECT name, pending_mutations FROM clickhouse_vector_indexes('ddl_quoted') WHERE name LIKE 'ddl_pm%';

DROP FUNCTION ddl_plan;
CALL clickhouse_perform('vector_index_ddl_svr', 'DROP DATABASE vector_index_ddl_test');
DROP SERVER vector_index_ddl_svr CASCADE;
