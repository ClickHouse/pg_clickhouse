CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE SERVER hashing_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'hashing_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER hashing_loopback;

CREATE SERVER hashing_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER hashing_admin;

CALL clickhouse_perform('hashing_admin', 'DROP DATABASE IF EXISTS hashing_test');
CALL clickhouse_perform('hashing_admin', 'CREATE DATABASE hashing_test');
CALL clickhouse_perform('hashing_admin', $$
    CREATE TABLE hashing_test.inputs (
        id UInt8,
        text_data String,
        binary_data String,
        algorithm String
    ) ENGINE = TinyLog
$$);
CALL clickhouse_perform('hashing_admin', $$
    INSERT INTO hashing_test.inputs VALUES
        (1, 'abc', 'abc', 'sha256'),
        (2, 'hello', unhex('00FF8041424300'), 'sha512')
$$);

CREATE FOREIGN TABLE hash_inputs (
    id int,
    text_data text,
    binary_data bytea,
    algorithm text
) SERVER hashing_loopback OPTIONS (table_name 'inputs');

-- Check core SHA and pgcrypto digest pushdown without depending on EXPLAIN's
-- text formatting, which differs across supported PostgreSQL versions.
\unset ECHO
DO $$
DECLARE
    pg_names text[] := ARRAY['sha224', 'sha256', 'sha384', 'sha512'];
    ch_names text[] := ARRAY['SHA224', 'SHA256', 'SHA384', 'SHA512'];
    digest_names text[] := ARRAY['md5', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512'];
    digest_ch_names text[] :=
        ARRAY['MD5', 'SHA1', 'SHA224', 'SHA256', 'SHA384', 'SHA512'];
    output jsonb;
    remote_sql text;
BEGIN
    FOR i IN 1..array_length(pg_names, 1) LOOP
        EXECUTE format(
            'EXPLAIN (VERBOSE, FORMAT JSON) '
            'SELECT %I(binary_data) AS h FROM hash_inputs GROUP BY h',
            pg_names[i]
        ) INTO output;
        remote_sql := jsonb_path_query_first(output, '$[0].Plan')->>'Remote SQL';
        RAISE NOTICE '% PUSHED DOWN: %', pg_names[i],
            strpos(remote_sql, ch_names[i] || '(binary_data)') > 0;
    END LOOP;

    FOR i IN 1..array_length(digest_names, 1) LOOP
        EXECUTE format(
            'EXPLAIN (VERBOSE, FORMAT JSON) '
            'SELECT digest(binary_data, %L) AS h FROM hash_inputs GROUP BY h',
            digest_names[i]
        ) INTO output;
        remote_sql := jsonb_path_query_first(output, '$[0].Plan')->>'Remote SQL';
        RAISE NOTICE 'digest(bytea, ''%'') PUSHED DOWN: %', digest_names[i],
            strpos(remote_sql, digest_ch_names[i] || '(binary_data)') > 0 AND
            strpos(remote_sql, 'digest') = 0;
    END LOOP;

    EXECUTE
        'EXPLAIN (VERBOSE, FORMAT JSON) '
        'SELECT digest(text_data, ''SHA256'') AS h FROM hash_inputs GROUP BY h'
        INTO output;
    remote_sql := jsonb_path_query_first(output, '$[0].Plan')->>'Remote SQL';
    RAISE NOTICE 'CASE-INSENSITIVE digest(text) PUSHED DOWN: %',
        strpos(remote_sql, 'SHA256(text_data)') > 0;

    EXECUTE
        'EXPLAIN (VERBOSE, FORMAT JSON) '
        'SELECT id FROM hash_inputs '
        'WHERE digest(binary_data, algorithm) IS NOT NULL'
        INTO output;
    RAISE NOTICE 'DYNAMIC DIGEST KEPT LOCAL: %',
        jsonb_path_query_first(output, '$[0].Plan') ? 'Filter' AND
        strpos(
            jsonb_path_query_first(output, '$[0].Plan')->>'Remote SQL', 'digest'
        ) = 0;

    EXECUTE
        'EXPLAIN (VERBOSE, FORMAT JSON) '
        'SELECT id FROM hash_inputs '
        'WHERE digest(binary_data, ''unsupported'') IS NOT NULL'
        INTO output;
    RAISE NOTICE 'UNSUPPORTED DIGEST KEPT LOCAL: %',
        jsonb_path_query_first(output, '$[0].Plan') ? 'Filter' AND
        strpos(
            jsonb_path_query_first(output, '$[0].Plan')->>'Remote SQL', 'digest'
        ) = 0;

    EXECUTE
        'EXPLAIN (VERBOSE, FORMAT JSON) '
        'SELECT id FROM hash_inputs '
        'WHERE digest(binary_data, NULL::text) IS NOT NULL'
        INTO output;
    RAISE NOTICE 'NULL DIGEST NOT PUSHED DOWN: %',
        strpos(output::text, 'digest') = 0;
END;
$$;
\set ECHO all

-- Force the expressions into the remote target and verify raw bytea results,
-- including a value containing NUL and non-UTF-8 bytes.
SELECT id, sha256(binary_data) AS hash
FROM hash_inputs
GROUP BY id, hash
ORDER BY id;

SELECT id, digest(binary_data, 'sha512') AS hash
FROM hash_inputs
GROUP BY id, hash
ORDER BY id;

SELECT id, digest(text_data, 'sha1') AS hash
FROM hash_inputs
GROUP BY id, hash
ORDER BY id;

CALL clickhouse_perform('hashing_admin', 'DROP DATABASE hashing_test');
DROP FOREIGN TABLE hash_inputs;
DROP USER MAPPING FOR CURRENT_USER SERVER hashing_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER hashing_admin;
DROP SERVER hashing_loopback;
DROP SERVER hashing_admin;
