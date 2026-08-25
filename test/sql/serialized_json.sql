\pset format unaligned

CREATE SERVER serialized_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw
OPTIONS (dbname 'serialized_json_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER serialized_json_loopback;

CREATE SERVER serialized_json_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER serialized_json_admin;

CALL clickhouse_perform(
    'serialized_json_admin',
    'DROP DATABASE IF EXISTS serialized_json_test'
);
CALL clickhouse_perform(
    'serialized_json_admin',
    'CREATE DATABASE serialized_json_test'
);
CALL clickhouse_perform('serialized_json_admin', $$
    CREATE TABLE serialized_json_test.documents (
        id Int32 NOT NULL,
        body Nullable(String)
    ) ENGINE = MergeTree ORDER BY id
$$);

CREATE SCHEMA serialized_json;
IMPORT FOREIGN SCHEMA serialized_json_test
FROM SERVER serialized_json_loopback INTO serialized_json;

CREATE VIEW serialized_json.documents_jsonb AS
SELECT id, jsonb_in(body::cstring) AS data
FROM serialized_json.documents;

CREATE VIEW serialized_json.documents_json AS
SELECT id, json_in(body::cstring) AS data
FROM serialized_json.documents;

INSERT INTO serialized_json.documents VALUES
    (
        1,
        '{"body":"Девятый вал","empty":"","number":28,"flag":true,"object":{"x":1},"nested":{"value":"глубина"},"null":null}'
    ),
    (2, '{"body":"другой"}'),
    (3, NULL);

-- String-backed jsonb extraction in a filter is pushed down, including ILIKE.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id
FROM serialized_json.documents_jsonb
WHERE COALESCE(jsonb_extract_path_text(data, 'body'), '') ILIKE '%девятый%';

SELECT id
FROM serialized_json.documents_jsonb
WHERE COALESCE(jsonb_extract_path_text(data, 'body'), '') ILIKE '%девятый%'
ORDER BY id;

SELECT
    id,
    jsonb_extract_path_text(data, 'body') AS body,
    jsonb_extract_path_text(data, 'empty') AS empty,
    jsonb_extract_path_text(data, 'number') AS number,
    jsonb_extract_path_text(data, 'flag') AS flag,
    jsonb_extract_path_text(data, 'object') AS object,
    jsonb_extract_path_text(data, 'nested', 'value') AS nested,
    jsonb_extract_path_text(data, 'null') AS null_value,
    jsonb_extract_path_text(data, 'missing') AS missing
FROM serialized_json.documents_jsonb
ORDER BY id;

-- The same compatibility cast is supported for PostgreSQL json.
SELECT id
FROM serialized_json.documents_json
WHERE json_extract_path_text(data, 'nested', 'value') = 'глубина'
ORDER BY id;

-- Numeric path components have different array semantics in ClickHouse.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id
FROM serialized_json.documents_jsonb
WHERE jsonb_extract_path_text(data, '0') = 'value';

-- Empty paths and non-text extraction remain local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id
FROM serialized_json.documents_jsonb
WHERE jsonb_extract_path_text(data, VARIADIC ARRAY[]::text[]) IS NOT NULL;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id
FROM serialized_json.documents_jsonb
WHERE jsonb_extract_path(data, 'body') = '"value"'::jsonb;

DROP VIEW serialized_json.documents_json;
DROP VIEW serialized_json.documents_jsonb;
DROP FOREIGN TABLE serialized_json.documents;
DROP SCHEMA serialized_json;
CALL clickhouse_perform(
    'serialized_json_admin',
    'DROP DATABASE serialized_json_test'
);
DROP SERVER serialized_json_loopback CASCADE;
DROP SERVER serialized_json_admin CASCADE;
