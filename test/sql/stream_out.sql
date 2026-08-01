CREATE SERVER try_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'try_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER try_http;

CREATE SERVER try_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER try_admin;

CALL clickhouse_perform('try_admin', 'DROP DATABASE IF EXISTS try_test');
CALL clickhouse_perform('try_admin', 'CREATE DATABASE try_test');
CALL clickhouse_perform('try_admin', 'CREATE TABLE try_test.t1 (c1 Int32)
    ENGINE = MergeTree ORDER BY c1');
CALL clickhouse_perform('try_admin', 'INSERT INTO try_test.t1
    SELECT number FROM numbers(3500)');

CREATE FOREIGN TABLE try_http_ft (c1 int)
    SERVER try_http OPTIONS (table_name 't1');

SELECT * FROM try_http_ft;

SELECT count(*), min(c1), max(c1), sum(c1::bigint) FROM try_http_ft;

DROP USER MAPPING FOR CURRENT_USER SERVER try_http;
DROP SERVER try_http CASCADE;
CALL clickhouse_perform('try_admin', 'DROP DATABASE try_test');
