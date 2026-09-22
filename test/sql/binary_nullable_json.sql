CREATE SERVER binary_nullable_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw
	OPTIONS(dbname 'binary_nullable_json_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_nullable_json_loopback;

CREATE SERVER binary_nullable_json_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_nullable_json_admin;

\set ECHO errors
SELECT clickhouse_server_version('binary_nullable_json_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (25, 3) AS no_ch253 \gset
\if :no_ch253
\echo 'SKIP: JSON support incomplete prior to ClickHouse 25.3'
\quit
\endif
\set ECHO all

CALL clickhouse_perform('binary_nullable_json_admin', 'DROP DATABASE IF EXISTS binary_nullable_json_test');
CALL clickhouse_perform('binary_nullable_json_admin', 'CREATE DATABASE binary_nullable_json_test');
CALL clickhouse_perform('binary_nullable_json_admin', 'CREATE TABLE binary_nullable_json_test.json_vals (
	c1 Int32, c2 Nullable(JSON)
) ENGINE = MergeTree ORDER BY (c1);');

CREATE FOREIGN TABLE json_vals (c1 int, c2 jsonb)
	SERVER binary_nullable_json_loopback OPTIONS (table_name 'json_vals');
INSERT INTO json_vals VALUES (1, '{"a": 1}'), (2, NULL), (3, '{"b": 2}');
SELECT * FROM json_vals ORDER BY c1;

DROP FOREIGN TABLE json_vals;
DROP USER MAPPING FOR CURRENT_USER SERVER binary_nullable_json_loopback;
CALL clickhouse_perform('binary_nullable_json_admin', 'DROP DATABASE binary_nullable_json_test');
DROP SERVER binary_nullable_json_loopback CASCADE;
