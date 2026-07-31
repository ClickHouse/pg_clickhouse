CREATE SERVER jsonb_exists_binary FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'jsonb_exists', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER jsonb_exists_binary;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS jsonb_exists');
SELECT clickhouse_raw_query('CREATE DATABASE jsonb_exists');
SELECT clickhouse_raw_query($$
    CREATE TABLE jsonb_exists.native_documents (
        id Int32,
        document JSON
    ) ENGINE = MergeTree ORDER BY id
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO jsonb_exists.native_documents VALUES
        (1, '{"key":1,"first":true}'),
        (2, '{"other":2}'),
        (3, '{"other":null}')
$$);
SELECT clickhouse_raw_query($$
    CREATE TABLE jsonb_exists.string_documents (
        id Int32,
        document Nullable(String)
    ) ENGINE = MergeTree ORDER BY id
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO jsonb_exists.string_documents VALUES
        (1, '{"key":1,"first":true}'),
        (2, '["key","second",3]'),
        (3, '{"other":2}'),
        (4, '["other",3]'),
        (5, NULL),
        (6, '"key"')
$$);

CREATE FOREIGN TABLE jsonb_exists_native (
    id integer,
    document jsonb
) SERVER jsonb_exists_binary OPTIONS (table_name 'native_documents');

CREATE FOREIGN TABLE jsonb_exists_string (
    id integer,
    document text
) SERVER jsonb_exists_binary OPTIONS (table_name 'string_documents');

CREATE VIEW jsonb_exists_compatibility AS
SELECT id, pg_catalog.jsonb_in(document::pg_catalog.cstring) AS document
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
WHERE (document || '{}'::jsonb) ? 'key'
ORDER BY id;
SELECT id FROM jsonb_exists_compatibility
WHERE (document || '{}'::jsonb) ? 'key'
ORDER BY id;

DROP VIEW jsonb_exists_compatibility;
DROP FOREIGN TABLE jsonb_exists_string;
DROP FOREIGN TABLE jsonb_exists_native;
DROP USER MAPPING FOR CURRENT_USER SERVER jsonb_exists_binary;
SELECT clickhouse_raw_query('DROP DATABASE jsonb_exists');
DROP SERVER jsonb_exists_binary;
