CREATE SERVER try_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'try_test', driver 'http', fetch_size '1');
CREATE USER MAPPING FOR CURRENT_USER SERVER try_http;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS try_test');
SELECT clickhouse_raw_query('CREATE DATABASE try_test');
SELECT clickhouse_raw_query('CREATE TABLE try_test.t1 (c1 Int32)
    ENGINE = MergeTree ORDER BY c1');
SELECT clickhouse_raw_query('INSERT INTO try_test.t1
    SELECT number FROM numbers(3500)');

CREATE FOREIGN TABLE try_http_ft (c1 int)
    SERVER try_http OPTIONS (table_name 't1');

SELECT * FROM try_http_ft;

ALTER SERVER try_http OPTIONS (SET fetch_size '3');

SELECT count(*), min(c1), max(c1), sum(c1::bigint) FROM try_http_ft;

DROP USER MAPPING FOR CURRENT_USER SERVER try_http;
DROP SERVER try_http CASCADE;
SELECT clickhouse_raw_query('DROP DATABASE try_test');
