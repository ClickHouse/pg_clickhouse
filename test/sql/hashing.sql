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

-- Core SHA functions are calculated in ClickHouse.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT sha224(binary_data) AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT sha256(binary_data) AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT sha384(binary_data) AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT sha512(binary_data) AS h FROM hash_inputs GROUP BY h;

-- Both digest() overloads and all recognized constant algorithms are pushed
-- down. Uppercase SHA256 exercises case-insensitive algorithm matching.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(binary_data, 'md5') AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(binary_data, 'sha1') AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(binary_data, 'sha224') AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(binary_data, 'sha256') AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(binary_data, 'sha384') AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(binary_data, 'sha512') AS h FROM hash_inputs GROUP BY h;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT digest(text_data, 'SHA256') AS h FROM hash_inputs GROUP BY h;

-- Dynamic and unsupported algorithms remain local to PostgreSQL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM hash_inputs
WHERE digest(binary_data, algorithm) IS NOT NULL;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM hash_inputs
WHERE digest(binary_data, 'unsupported') IS NOT NULL;

-- A NULL algorithm is folded away locally because digest() is strict.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM hash_inputs
WHERE digest(binary_data, NULL::text) IS NOT NULL;

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

-- Verify the identical hashed values between Postgres and ClickHouse.
CREATE TABLE pg_hash_inputs AS SELECT * FROM hash_inputs;

SELECT digest(binary_data, 'md5') AS md5 FROM hash_inputs
UNION ALL
SELECT digest(binary_data, 'md5') FROM pg_hash_inputs
ORDER BY 1;

SELECT digest(binary_data, 'sha1') AS sha1 FROM hash_inputs
UNION ALL
SELECT digest(binary_data, 'sha1') FROM pg_hash_inputs
ORDER BY 1;

SELECT digest(binary_data, 'sha224') AS sha224 FROM hash_inputs
UNION ALL
SELECT digest(binary_data, 'sha224') FROM pg_hash_inputs
UNION ALL
SELECT sha224(binary_data) FROM hash_inputs
UNION ALL
SELECT sha224(binary_data) FROM pg_hash_inputs
ORDER BY 1;

SELECT digest(binary_data, 'sha256') AS sha256 FROM hash_inputs
UNION ALL
SELECT digest(binary_data, 'sha256') FROM pg_hash_inputs
UNION ALL
SELECT sha256(binary_data) FROM hash_inputs
UNION ALL
SELECT sha256(binary_data) FROM pg_hash_inputs
ORDER BY 1;

SELECT digest(binary_data, 'sha384') AS sha384 FROM hash_inputs
UNION ALL
SELECT digest(binary_data, 'sha384') FROM pg_hash_inputs
UNION ALL
SELECT sha384(binary_data) FROM hash_inputs
UNION ALL
SELECT sha384(binary_data) FROM pg_hash_inputs
ORDER BY 1;

SELECT digest(binary_data, 'sha512') AS sha512 FROM hash_inputs
UNION ALL
SELECT digest(binary_data, 'sha512') FROM pg_hash_inputs
UNION ALL
SELECT sha512(binary_data) FROM hash_inputs
UNION ALL
SELECT sha512(binary_data) FROM pg_hash_inputs
ORDER BY 1;

CALL clickhouse_perform('hashing_admin', 'DROP DATABASE hashing_test');
DROP FOREIGN TABLE hash_inputs;
DROP USER MAPPING FOR CURRENT_USER SERVER hashing_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER hashing_admin;
DROP SERVER hashing_loopback;
DROP SERVER hashing_admin;
