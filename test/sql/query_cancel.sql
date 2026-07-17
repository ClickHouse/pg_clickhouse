-- Test that query cancellation (e.g. Ctrl+C / statement_timeout) works for
-- both the HTTP and binary drivers during remote query execution.

-- HTTP driver
CREATE SERVER cancel_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'cancel_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER cancel_http;

-- Binary driver
CREATE SERVER cancel_binary FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'cancel_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER cancel_binary;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS cancel_test');
SELECT clickhouse_raw_query('CREATE DATABASE cancel_test');
SELECT clickhouse_raw_query('CREATE TABLE cancel_test.t1 (c1 Int32)
    ENGINE = MergeTree ORDER BY c1');
SELECT clickhouse_raw_query('INSERT INTO cancel_test.t1
    SELECT number FROM numbers(100)');

CREATE FOREIGN TABLE cancel_http_ft (c1 int)
    SERVER cancel_http OPTIONS (table_name 't1');
CREATE FOREIGN TABLE cancel_binary_ft (c1 int)
    SERVER cancel_binary OPTIONS (table_name 't1');

-- Warm up connections so the cancel test only covers query execution.
SELECT count(*) FROM cancel_http_ft;
SELECT count(*) FROM cancel_binary_ft;

-- The repeated CROSS JOINs intentionally create a large remote result set,
-- so the 10ms timeout fires while the remote query is still running.
-- HTTP streaming: exercises the curl progress-callback cancel path via
-- the streaming implementation.
BEGIN;
SET LOCAL statement_timeout = '10ms';
SELECT count(*) FROM cancel_http_ft a CROSS JOIN cancel_http_ft b
    CROSS JOIN cancel_http_ft c CROSS JOIN cancel_http_ft d;
COMMIT;

-- Binary: same test, exercising the OnProgress cancel path.
BEGIN;
SET LOCAL statement_timeout = '10ms';
SELECT count(*) FROM cancel_binary_ft a CROSS JOIN cancel_binary_ft b
    CROSS JOIN cancel_binary_ft c CROSS JOIN cancel_binary_ft d;
COMMIT;

-- Verify each connection is still usable after the canceled query tears down
-- any remote state.
SELECT count(*) FROM cancel_http_ft;
SELECT count(*) FROM cancel_binary_ft;

-- Cleanup
DROP FOREIGN TABLE cancel_http_ft;
DROP FOREIGN TABLE cancel_binary_ft;
DROP USER MAPPING FOR CURRENT_USER SERVER cancel_http;
DROP USER MAPPING FOR CURRENT_USER SERVER cancel_binary;
DROP SERVER cancel_http;
DROP SERVER cancel_binary;
SELECT clickhouse_raw_query('DROP DATABASE cancel_test');
