SET datestyle = 'ISO';
CREATE SERVER binary_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_test', driver 'binary');
CREATE SERVER http_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_json_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_json_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS json_test');
SELECT clickhouse_raw_query('CREATE DATABASE json_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE json_test.things (
        id   Int32 NOT NULL,
        data JSON NOT NULL
    ) ENGINE = MergeTree PARTITION BY id ORDER BY (id);
$$);

CREATE SCHEMA json_bin;
CREATE SCHEMA json_http;
IMPORT FOREIGN SCHEMA "json_test" FROM SERVER binary_json_loopback INTO json_bin;
\d json_bin.things
IMPORT FOREIGN SCHEMA "json_test" FROM SERVER http_json_loopback INTO json_http;
\d json_http.things

-- Fails pending https://github.com/ClickHouse/clickhouse-cpp/issues/422
INSERT INTO json_bin.things VALUES
    (1, '{"id": 1, "name": "widget", "size": "large", "stocked": true}'),
    (2, '{"id": 2, "name": "sprocket", "size": "small", "stocked": true}')
;

INSERT INTO json_http.things VALUES
    (1, '{"id": 1, "name": "widget", "size": "large", "stocked": true}'),
    (2, '{"id": 2, "name": "sprocket", "size": "small", "stocked": true}'),
    (3, '{"id": 3, "name": "gizmo", "size": "medium", "stocked": true}'),
    (4, '{"id": 4, "name": "doodad", "size": "large", "stocked": false}')
;

SELECT * FROM json_bin.things ORDER BY id;
SELECT * FROM json_http.things ORDER BY id;

SELECT clickhouse_raw_query('DROP DATABASE json_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_json_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_json_loopback;
DROP SERVER binary_json_loopback CASCADE;
DROP SERVER http_json_loopback CASCADE;
