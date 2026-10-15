SET datestyle = 'ISO';

CREATE SERVER tsvector_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'tsvector_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER tsvector_loopback;
CREATE SERVER tsvector_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER tsvector_admin;

CALL clickhouse_perform('tsvector_admin', 'DROP DATABASE IF EXISTS tsvector_test');
CALL clickhouse_perform('tsvector_admin', 'CREATE DATABASE tsvector_test');
CALL clickhouse_perform('tsvector_admin', $$
    CREATE TABLE tsvector_test.documents (
        id Int32,
        search_tsv Nullable(String)
    ) ENGINE = MergeTree ORDER BY id
$$);

CREATE SCHEMA tsvector_raw;
IMPORT FOREIGN SCHEMA tsvector_test
    FROM SERVER tsvector_loopback INTO tsvector_raw;
CREATE VIEW tsvector_documents AS
SELECT id, pg_catalog.tsvectorin(search_tsv::pg_catalog.cstring) AS search_tsv
FROM tsvector_raw.documents;

INSERT INTO tsvector_raw.documents VALUES
    (1, to_tsvector('russian', 'Девятый батальон 28')::text),
    (2, to_tsvector('russian', 'Восьмой батальон')::text),
    (3, to_tsvector('simple', 'foo-bar')::text),
    (4, strip(to_tsvector('simple', 'foo bar'))::text),
    (5, NULL),
    (6, $$'other':28$$),
    (7, to_tsvector('simple', 'foo x bar')::text),
    (8, to_tsvector('simple', 'foo bar baz')::text);

CREATE TABLE local_tsvector_documents AS
SELECT id, pg_catalog.tsvectorin(search_tsv::pg_catalog.cstring) AS search_tsv
FROM tsvector_raw.documents;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('russian', 'Девятый')
ORDER BY id;

SELECT array_agg(id ORDER BY id) FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('russian', 'Девятый');

SELECT array_agg(id ORDER BY id) FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('simple', '28');

SELECT array_agg(id ORDER BY id) FROM tsvector_documents
WHERE search_tsv @@ to_tsquery('russian', 'девят:*');

SELECT array_agg(id ORDER BY id) FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('russian', 'Девятый OR Восьмой');

SELECT array_agg(id ORDER BY id) FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('russian', 'батальон -Восьмой');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('simple', 'foo-bar')
ORDER BY id;

SELECT
    (SELECT array_agg(id ORDER BY id) FROM tsvector_documents
     WHERE search_tsv @@ websearch_to_tsquery('simple', 'foo-bar')) =
    (SELECT array_agg(id ORDER BY id) FROM local_tsvector_documents
     WHERE search_tsv @@ websearch_to_tsquery('simple', 'foo-bar'))
    AS phrase_matches_postgres;

SELECT
    (SELECT array_agg(id ORDER BY id) FROM tsvector_documents
     WHERE search_tsv @@ to_tsquery('simple', 'foo <2> bar')) =
    (SELECT array_agg(id ORDER BY id) FROM local_tsvector_documents
     WHERE search_tsv @@ to_tsquery('simple', 'foo <2> bar'))
    AS phrase_distance_matches_postgres;

SELECT id, search_tsv @@ to_tsquery('simple', 'foo & bar') AS matched
FROM tsvector_documents
WHERE id IN (3, 4, 5)
ORDER BY id;

SELECT array_agg(id ORDER BY id) FROM tsvector_documents
WHERE to_tsquery('simple', 'foo') @@ search_tsv;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tsvector_documents
WHERE search_tsv @@ to_tsquery('simple', 'foo:A');

SET plan_cache_mode = force_custom_plan;
PREPARE tsvector_search(text) AS
SELECT id FROM tsvector_documents
WHERE search_tsv @@ websearch_to_tsquery('russian', $1);
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE tsvector_search('Девятый');

SET plan_cache_mode = force_generic_plan;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE tsvector_search('Девятый');
DEALLOCATE tsvector_search;
RESET plan_cache_mode;

CALL clickhouse_perform('tsvector_admin', 'DROP DATABASE tsvector_test');
DROP VIEW tsvector_documents;
DROP TABLE local_tsvector_documents;
DROP SERVER tsvector_loopback CASCADE;
DROP SERVER tsvector_admin CASCADE;
DROP SCHEMA tsvector_raw CASCADE;
