CREATE SERVER udf_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'udf_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER udf_loopback;

CREATE SERVER udf_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER udf_admin;

CALL clickhouse_perform('udf_admin', 'DROP DATABASE IF EXISTS udf_test');
CALL clickhouse_perform('udf_admin', 'CREATE DATABASE udf_test');
CALL clickhouse_perform('udf_admin', $$
    CREATE TABLE udf_test.vals (
        id UInt8
    ) ENGINE = TinyLog
$$);
CALL clickhouse_perform('udf_admin', $$
    INSERT INTO udf_test.vals VALUES (1), (2), (3)
$$);

CREATE FOREIGN TABLE udf_vals (
    id int
) SERVER udf_loopback OPTIONS (table_name 'vals');

CREATE FUNCTION keep_local(bigint) RETURNS bigint LANGUAGE plpgsql AS $$
BEGIN
    RETURN $1;
END;
$$;

-- pg_clickhouse keeps unknown functions local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE keep_local(id) = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT keep_local(sum(id)) FROM udf_vals;
SELECT keep_local(sum(id)) FROM udf_vals;

-- Keep pg_clickhouse's own un-shippable functions local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE pgch_version() = '';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE clickhouse_server_version('') = '';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE clickhouse_fdw_validator('{x}'::text[], 0) IS NULL;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE ch_noop_bigint('1') = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE ch_noop_float8('1') = 1.1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM udf_vals WHERE ch_noop_float8_float8('1', 1.1) = 1.1;

\unset ECHO
CREATE OR REPLACE FUNCTION remote_sql(TEXT) RETURNS SETOF JSONB LANGUAGE plpgsql  AS $$
DECLARE
	output JSONB;
BEGIN
    EXECUTE format('EXPLAIN (VERBOSE, FORMAT JSON) %s', $1) INTO output;
    RETURN QUERY SELECT * FROM jsonb_path_query(output, '$.**."Remote SQL"');
END;
$$;
\set ECHO all

-- Keep unknown extension's functions local.
CREATE EXTENSION IF NOT EXISTS pg_trgm;
SELECT * FROM remote_sql($$ SELECT id FROM udf_vals WHERE set_limit(id) = '1' $$);

-- Keep unknown extension's types and operators local.
CREATE EXTENSION IF NOT EXISTS citext;
SELECT * FROM remote_sql($$ SELECT id FROM udf_vals WHERE citext(id) = '1' $$);
SELECT * FROM remote_sql($$ SELECT id FROM udf_vals WHERE citext(id) IS NULL $$);

-- Wait for a lock before loading intarray.
SELECT pg_advisory_lock(hashtext('intarray'));
CREATE EXTENSION intarray;

-- Keep unknown functions from known extension local.
SELECT * FROM remote_sql('SELECT id FROM udf_vals WHERE intset(id) = ARRAY[1]');

-- Keep unmapped operators from known extension local.
SELECT * FROM remote_sql('SELECT id FROM udf_vals WHERE ARRAY[id] @> ARRAY[1]');

-- Drop intarray so it doesn't mess with other tests (array_functions.sql).
DROP EXTENSION intarray;
SELECT pg_advisory_unlock(hashtext('intarray'));

CALL clickhouse_perform('udf_admin', 'DROP DATABASE udf_test');
DROP USER MAPPING FOR CURRENT_USER SERVER udf_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER udf_admin;
DROP SERVER udf_loopback CASCADE;
DROP SERVER udf_admin CASCADE;
