\set ECHO errors
CREATE SERVER binary_tuple_map_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'tuple_map_test', driver 'binary');
CREATE SERVER http_tuple_map_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'tuple_map_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_tuple_map_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_tuple_map_loopback;

CREATE SERVER tuple_map_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER tuple_map_admin;

CALL clickhouse_perform('tuple_map_admin', 'DROP DATABASE IF EXISTS tuple_map_test');
CALL clickhouse_perform('tuple_map_admin', 'CREATE DATABASE tuple_map_test');

CREATE SCHEMA tupmap_bin;
CREATE SCHEMA tupmap_http;

PREPARE describe(regclass) AS
SELECT attname, format_type(atttypid, atttypmod) AS type, attndims, attnotnull
  FROM pg_attribute
 WHERE attrelid = $1 AND attnum > 0
 ORDER BY attnum;

\set ECHO all

/****************************************************************************/
CALL clickhouse_perform('tuple_map_admin', $$
    CREATE TABLE tuple_map_test.people (
        id UInt32,
        data Tuple(
            name   String,
            age    UInt8,
            gender Enum('male', 'female', 'enby', 'other')
        ),
        sizes Map(String, UInt8)
    ) ENGINE = MergeTree ORDER BY id
$$);

IMPORT FOREIGN SCHEMA tuple_map_test FROM SERVER binary_tuple_map_loopback INTO tupmap_bin;
EXECUTE describe('tupmap_bin.people');
IMPORT FOREIGN SCHEMA tuple_map_test FROM SERVER http_tuple_map_loopback INTO tupmap_http;
EXECUTE describe('tupmap_http.people');

-- Insert values.
INSERT INTO tupmap_bin.people
VALUES (1, ('{Barrack,47,male}'::text[]), '{{head,32}, {neck,17}, {waist,34}}');

INSERT INTO tupmap_http.people
VALUES (2, ('{Vivian,22,female}'::text[]), '{{head,29}, {neck,14}, {shoe,9}}');

SELECT * FROM tupmap_bin.people ORDER BY id;
SELECT * FROM tupmap_http.people ORDER BY id;

-- Pushdown array access fails. Failure expected: we would need to know that
-- the column is a Tuple and thus should be converted to `data.age = 22`, or a
-- Map and converted to `sizes['shoe'] = 9`.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_bin.people WHERE data[2] = '22';
SELECT id FROM tupmap_bin.people WHERE data[2] = '22';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_http.people WHERE data[2] = '22';
SELECT id FROM tupmap_http.people WHERE data[2] = '22';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_bin.people WHERE sizes[3] = '9';
SELECT id FROM tupmap_bin.people WHERE sizes[3] = '9';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_http.people WHERE sizes[3] = '9';
SELECT id FROM tupmap_http.people WHERE sizes[3] = '9';

/****************************************************************************/
-- Create them manually with a composite type;
CREATE TYPE gender AS ENUM('male', 'female', 'enby', 'other');
CREATE TYPE profile AS (name text, age smallint, gender gender);
CREATE TYPE pair AS (k text, v smallint);

DROP FOREIGN TABLE tupmap_bin.people;
CREATE FOREIGN TABLE tupmap_bin.people(
    id          BIGINT  NOT NULL,
    data        profile NOT NULL,
    sizes       pair[]  NOT NULL
) SERVER binary_tuple_map_loopback OPTIONS(table_name 'people');


DROP FOREIGN TABLE tupmap_http.people;
CREATE FOREIGN TABLE tupmap_http.people(
    id          BIGINT  NOT NULL,
    data        profile NOT NULL,
    sizes       pair[]  NOT NULL
) SERVER http_tuple_map_loopback OPTIONS(table_name 'people');

-- Insert values.
INSERT INTO tupmap_bin.people
VALUES (3, ('Judith', 70, 'enby'), ARRAY[('head', 34), ('shoe', 7)]::pair[]);
INSERT INTO tupmap_http.people
VALUES (4, ('Bagel', 9, 'other'), ARRAY[('head', 10), ('neck', 8)]::pair[]);

SELECT * FROM tupmap_bin.people ORDER BY id;
SELECT * FROM tupmap_http.people ORDER BY id;

-- Querying Tuple field access works but remains local for now.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_bin.people WHERE (data).age = 22;
SELECT id FROM tupmap_bin.people WHERE (data).age = 22;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_http.people WHERE (data).age = 22;
SELECT id FROM tupmap_http.people WHERE (data).age = 22;

-- Querying an array of composite fields also pushes down, but remains local
-- for now.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_bin.people WHERE (sizes[1]).v = 10;
SELECT id FROM tupmap_bin.people WHERE (sizes[1]).v = 10;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tupmap_http.people WHERE (sizes[1]).v = 10;
SELECT id FROM tupmap_http.people WHERE (sizes[1]).v = 10;

-- Add queries used for docs.
CALL clickhouse_perform('tuple_map_admin', $$
    CREATE TABLE tuple_map_test.named_tuples (a Tuple(s String, i Int64)) ENGINE = Memory;
$$);

CALL clickhouse_perform('tuple_map_admin', $$
    INSERT INTO tuple_map_test.named_tuples VALUES (('y', 10)), (('x',-10));
$$);

IMPORT FOREIGN SCHEMA tuple_map_test LIMIT TO (named_tuples)
  FROM SERVER binary_tuple_map_loopback INTO tupmap_bin ;

EXECUTE describe('tupmap_bin.named_tuples');
SELECT * FROM tupmap_bin.named_tuples;

CALL clickhouse_perform('tuple_map_admin', 'DROP DATABASE tuple_map_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_tuple_map_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_tuple_map_loopback;
DROP SERVER binary_tuple_map_loopback CASCADE;
DROP SERVER http_tuple_map_loopback CASCADE;
