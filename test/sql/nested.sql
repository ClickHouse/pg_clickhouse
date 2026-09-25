\set ECHO errors
SET datestyle = 'ISO';
CREATE SERVER binary_nested_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'nested_test', driver 'binary');
CREATE SERVER http_nested_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'nested_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_nested_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_nested_loopback;

CREATE SERVER nested_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER nested_admin;

CALL clickhouse_perform('nested_admin', 'DROP DATABASE IF EXISTS nested_test');
CALL clickhouse_perform('nested_admin', 'CREATE DATABASE nested_test');

CREATE SCHEMA nest_bin;
CREATE SCHEMA nest_http;

PREPARE describe(regclass) AS
SELECT attname, format_type(atttypid, atttypmod) AS type, attndims, attnotnull
  FROM pg_attribute
 WHERE attrelid = $1 AND attnum > 0
 ORDER BY attnum;

\set ECHO all

/****************************************************************************/
-- By default ClickHouse flattens a Nested column.
CALL clickhouse_perform('nested_admin', $$
    CREATE TABLE nested_test.visits(
        visit_id  UInt64,
        user_id   UInt64,
        goals     Nested(
            serial    UInt32,
            order_id  String
        )
    )
    ENGINE = MergeTree ORDER BY visit_id
$$);

-- By default, a Nested column is flattened into multiple columns.
IMPORT FOREIGN SCHEMA nested_test FROM SERVER binary_nested_loopback INTO nest_bin;
EXECUTE describe('nest_bin.visits');
IMPORT FOREIGN SCHEMA nested_test FROM SERVER http_nested_loopback INTO nest_http;
EXECUTE describe('nest_http.visits');

-- Insert values.
INSERT INTO nest_bin.visits
VALUES (1, 1, '{1,2}'::bigint[],'{xx,yy}'::text[]);

INSERT INTO nest_bin.visits
VALUES (2, 2, '{3,4}'::bigint[],'{aa,bb}'::text[]);

SELECT * FROM nest_bin.visits ORDER BY visit_id;
SELECT * FROM nest_http.visits ORDER BY visit_id;

-- Should pushdown array access.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT visit_id FROM nest_bin.visits WHERE "goals.serial"[1] = 1;
SELECT visit_id FROM nest_bin.visits WHERE "goals.serial"[1] = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT visit_id FROM nest_http.visits WHERE "goals.serial"[1] = 1;
SELECT visit_id FROM nest_http.visits WHERE "goals.serial"[1] = 1;

\set ECHO errors
/****************************************************************************/
-- Is flatten_nested available?
SELECT clickhouse_server_version('nested_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (25, 1) AS no_ch251 \gset
\if :no_ch251
\echo 'SKIP: flatten_nested not available prior to ClickHouse 25.1'
\quit
\endif
\set ECHO all

/****************************************************************************/
-- Recreate everything with flatten_nested=0
DROP FOREIGN TABLE nest_bin.visits;
DROP FOREIGN TABLE nest_http.visits;

CALL clickhouse_perform('nested_admin', 'DROP TABLE nested_test.visits');
CALL clickhouse_perform('nested_admin', $$
    CREATE TABLE nested_test.visits(
        visit_id  UInt64,
        user_id   UInt64,
        goals     Nested(
            serial    UInt32,
            order_id  String
        )
    )
    ENGINE = MergeTree ORDER BY visit_id SETTINGS flatten_nested = 0
$$);

IMPORT FOREIGN SCHEMA nested_test FROM SERVER binary_nested_loopback INTO nest_bin;
EXECUTE describe('nest_bin.visits');
IMPORT FOREIGN SCHEMA nested_test FROM SERVER http_nested_loopback INTO nest_http;
EXECUTE describe('nest_http.visits');

-- Insert values.
INSERT INTO nest_bin.visits
VALUES (1, 1, '{{1,xx},{2,yy}}'::text[]);

INSERT INTO nest_bin.visits
VALUES (2, 2, '{{3,aa},{4,bb}}'::text[]);

SELECT * FROM nest_bin.visits ORDER BY visit_id;
SELECT * FROM nest_http.visits ORDER BY visit_id;

-- Pushdown multidimensional array access fails. Failure expected: we would
-- need to know that the column is Nested and thus should be converted to
-- `tupleElement(goals[1], 1) = '1'`.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT visit_id FROM nest_bin.visits WHERE goals[1][1] = '1';
SELECT visit_id FROM nest_bin.visits WHERE goals[1][1] = '1';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT visit_id FROM nest_http.visits WHERE goals[1][1] = '1';
SELECT visit_id FROM nest_http.visits WHERE goals[1][1] = '1';

/****************************************************************************/
-- Create them manually with a composite type;
CREATE TYPE goal_type AS (serial bigint, order_id text);
DROP FOREIGN TABLE nest_bin.visits;

CREATE FOREIGN TABLE nest_bin.visits(
    visit_id    numeric(20,0) NOT NULL,
    user_id     numeric(20,0) NOT NULL,
    goals       goal_type[]   NOT NULL
) SERVER binary_nested_loopback OPTIONS(table_name 'visits');

DROP FOREIGN TABLE nest_http.visits;
CREATE FOREIGN TABLE nest_http.visits(
    visit_id    numeric(20,0) NOT NULL,
    user_id     numeric(20,0) NOT NULL,
    goals       goal_type[]    NOT NULL
) SERVER http_nested_loopback OPTIONS(table_name 'visits');

-- Insert values.
INSERT INTO nest_bin.visits
VALUES (3, 3, ARRAY[row(5, 'jj'), row(6, 'zz')]::goal_type[]);

INSERT INTO nest_bin.visits
VALUES (4, 4, ARRAY[row(7, 'mm'), row(8, 'uu')]::goal_type[]);

SELECT * FROM nest_bin.visits ORDER BY visit_id;
SELECT * FROM nest_http.visits ORDER BY visit_id;

-- Querying composite field access works, unlike for multidimensional arrays,
-- but remains local for now.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT visit_id FROM nest_bin.visits WHERE goals[1].serial = 1;
SELECT visit_id FROM nest_bin.visits WHERE goals[1].serial = 1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT visit_id FROM nest_http.visits WHERE goals[1].serial = 1;
SELECT visit_id FROM nest_http.visits WHERE goals[1].serial = 1;

/****************************************************************************/
-- Replicate the composites example from the docs.
CALL clickhouse_perform('nested_admin', $$
    CREATE TABLE nested_test.events (
        id     UInt32,
        status Enum8('new' = 1, 'done' = 2),
        point  Tuple(Int32, Int32),
        labels Map(String, Int64),
        items  Nested(id Int32, name String)
    ) ORDER BY id SETTINGS flatten_nested = 0
$$);

CALL clickhouse_perform('nested_admin', $$
    INSERT INTO nested_test.events
    VALUES(1, 'new', tuple(3, 4), {'k1': 5, 'k2': 6}, [tuple(100, 'xx'), tuple(101, 'yy')])
$$);

CREATE TYPE event_status AS ENUM ('new', 'done');
CREATE TYPE event_point AS (x integer, y integer);
CREATE TYPE event_label AS (key text, value bigint);
CREATE TYPE event_item AS (id integer, name text);

CREATE FOREIGN TABLE nest_bin.events (
    id     bigint,
    status event_status,
    point  event_point,
    labels event_label[],
    items  event_item[]
) SERVER binary_nested_loopback OPTIONS(table_name 'events');

SELECT * FROM nest_bin.events ORDER BY id;
SELECT (point).x, (point).y,
       (labels[1]).key, (labels[1]).value,
       (items[1]).id, (items[1]).name
  FROM nest_bin.events ORDER BY id;

-- Composite access does not yet push down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nest_bin.events WHERE (point).x = 3;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nest_bin.events WHERE (labels[1]).key = 'k2';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nest_bin.events WHERE (items[1]).id = 100;

CALL clickhouse_perform('nested_admin', 'DROP DATABASE nested_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_nested_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_nested_loopback;
DROP SERVER binary_nested_loopback CASCADE;
DROP SERVER http_nested_loopback CASCADE;
