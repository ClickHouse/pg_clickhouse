SET datestyle = 'ISO';
CREATE SERVER binary_json_coldef_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_coldef_test', driver 'binary');
CREATE SERVER http_json_coldef_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_coldef_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_json_coldef_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_json_coldef_loopback;

CREATE SERVER json_coldef_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER json_coldef_admin;

\set ECHO errors
SELECT clickhouse_server_version('json_coldef_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (25, 3) AS no_ch253 \gset
\if :no_ch253
\echo 'SKIP: JSON support incomplete prior to ClickHouse 25.3'
\quit
\endif
\set ECHO all

CALL clickhouse_perform('json_coldef_admin', 'DROP DATABASE IF EXISTS json_coldef_test');
CALL clickhouse_perform('json_coldef_admin', 'CREATE DATABASE json_coldef_test');
CALL clickhouse_perform('json_coldef_admin', $$
    CREATE TABLE json_coldef_test.things (
        id   Int32 NOT NULL,
        data JSON(
          max_dynamic_paths=0,
          max_dynamic_types=0,
          id UInt32,
          name String,
          size Enum('small', 'medium', 'large'),
          stocked Bool,
          SKIP non.existent
        ) NOT NULL
    ) ENGINE = MergeTree PARTITION BY id ORDER BY (id);
$$);

CREATE SCHEMA json_coldef_bin;
CREATE SCHEMA json_coldef_http;
IMPORT FOREIGN SCHEMA "json_coldef_test" FROM SERVER binary_json_coldef_loopback INTO json_coldef_bin;
\d json_coldef_bin.things
IMPORT FOREIGN SCHEMA "json_coldef_test" FROM SERVER http_json_coldef_loopback INTO json_coldef_http;
\d json_coldef_http.things

INSERT INTO json_coldef_bin.things VALUES
    (1, '{"id": 1, "name": "widget", "size": "large", "stocked": true}'),
    (2, '{"id": 2, "name": "sprocket", "size": "small", "stocked": true}')
;

INSERT INTO json_coldef_http.things VALUES
    (3, '{"id": 3, "name": "gizmo", "size": "medium", "stocked": true}'),
    (4, '{"id": 4, "name": "doodad", "size": "large", "stocked": false}')
;

SELECT * FROM json_coldef_bin.things ORDER BY id;
SELECT * FROM json_coldef_http.things ORDER BY id;

-- ORDER BY with jsonb ->> pushdown.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_coldef_http.things ORDER BY data ->> 'name';
SELECT * FROM json_coldef_http.things ORDER BY data ->> 'name';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_coldef_bin.things ORDER BY data ->> 'name';
SELECT * FROM json_coldef_bin.things ORDER BY data ->> 'name';

SET pg_clickhouse.session_settings TO 'allow_suspicious_types_in_order_by 1';
SELECT * FROM json_coldef_http.things ORDER BY data ->> 'name' LIMIT 2;
SELECT * FROM json_coldef_bin.things ORDER BY data ->> 'name' LIMIT 2;
SET pg_clickhouse.session_settings TO '';

-- ORDER BY with json ->> pushdown.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_coldef_http.json_coldef_things ORDER BY data ->> 'name';
SELECT * FROM json_coldef_http.json_coldef_things ORDER BY data ->> 'name';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_coldef_bin.json_coldef_things ORDER BY data ->> 'name';
SELECT * FROM json_coldef_bin.json_coldef_things ORDER BY data ->> 'name';

CALL clickhouse_perform('json_coldef_admin', 'DROP DATABASE json_coldef_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_json_coldef_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_json_coldef_loopback;
DROP SERVER binary_json_coldef_loopback CASCADE;
DROP SERVER http_json_coldef_loopback CASCADE;
