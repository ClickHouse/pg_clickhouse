CREATE SERVER jsonb_exists_binary FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'jsonb_exists', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER jsonb_exists_binary;

CREATE SERVER jsonb_exists_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER jsonb_exists_admin;

CALL clickhouse_perform('jsonb_exists_admin',
    'DROP DATABASE IF EXISTS jsonb_exists');
CALL clickhouse_perform('jsonb_exists_admin', 'CREATE DATABASE jsonb_exists');
CALL clickhouse_perform('jsonb_exists_admin', $$
    CREATE TABLE jsonb_exists.native_documents (
        id Int32,
        document JSON
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('jsonb_exists_admin', $$
    INSERT INTO jsonb_exists.native_documents VALUES
        (1, '{"key":1,"first":true}'),
        (2, '{"other":2}'),
        (3, '{"other":null}')
$$);
CALL clickhouse_perform('jsonb_exists_admin', $$
    CREATE TABLE jsonb_exists.string_documents (
        id Int32,
        document Nullable(String),
        candidate String
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('jsonb_exists_admin', $$
    INSERT INTO jsonb_exists.string_documents VALUES
        (1, '{"key":1,"first":true}', 'key'),
        (2, '["key","second",3]', 'key'),
        (3, '{"other":2}', 'key'),
        (4, '["other",3]', 'other'),
        (5, NULL, 'key'),
        (6, '"key"', 'key')
$$);

CREATE FOREIGN TABLE jsonb_exists_native (
    id integer,
    document jsonb
) SERVER jsonb_exists_binary OPTIONS (table_name 'native_documents');

CREATE FOREIGN TABLE jsonb_exists_string (
    id integer,
    document text,
    candidate text
) SERVER jsonb_exists_binary OPTIONS (table_name 'string_documents');

CREATE VIEW jsonb_exists_compatibility AS
SELECT id, pg_catalog.jsonb_in(document::pg_catalog.cstring) AS document,
       candidate
FROM jsonb_exists_string;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM jsonb_exists_native WHERE document ? 'key' ORDER BY id;
SELECT id FROM jsonb_exists_native WHERE document ? 'key' ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM jsonb_exists_compatibility
WHERE document ? 'key'
ORDER BY id;
SELECT id FROM jsonb_exists_compatibility
WHERE document ? 'key'
ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM jsonb_exists_compatibility
WHERE document ? 'first' OR document ? 'second'
ORDER BY id;
SELECT id FROM jsonb_exists_compatibility
WHERE document ? 'first' OR document ? 'second'
ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM jsonb_exists_compatibility
WHERE document ? candidate
ORDER BY id;
SELECT id FROM jsonb_exists_compatibility
WHERE document ? candidate
ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM jsonb_exists_compatibility
WHERE (document || '{}'::jsonb) ? 'key'
ORDER BY id;
SELECT id FROM jsonb_exists_compatibility
WHERE (document || '{}'::jsonb) ? 'key'
ORDER BY id;

CALL clickhouse_perform('jsonb_exists_admin', $$
    INSERT INTO jsonb_exists.string_documents VALUES (7, 'not json', 'key')
$$);
DO $$
BEGIN
    PERFORM id FROM jsonb_exists_compatibility
    WHERE id = 7 AND document ? 'key';
    RAISE NOTICE 'invalid JSON error includes document: f';
EXCEPTION WHEN OTHERS THEN
    RAISE NOTICE 'invalid JSON error includes document: %',
        position('not json' IN SQLERRM) > 0;
END
$$;

DROP VIEW jsonb_exists_compatibility;
DROP FOREIGN TABLE jsonb_exists_string;
DROP FOREIGN TABLE jsonb_exists_native;
DROP USER MAPPING FOR CURRENT_USER SERVER jsonb_exists_binary;
CALL clickhouse_perform('jsonb_exists_admin', 'DROP DATABASE jsonb_exists');
DROP USER MAPPING FOR CURRENT_USER SERVER jsonb_exists_admin;
DROP SERVER jsonb_exists_binary;
DROP SERVER jsonb_exists_admin;
