SET datestyle = 'ISO';
CREATE SERVER binary_time_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'time_test', driver 'binary');
CREATE SERVER http_time_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'time_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_time_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_time_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS time_test');
SELECT clickhouse_raw_query('CREATE DATABASE time_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE time_test.times (
        id     Int32     NOT NULL,
        t32    Time      NOT NULL,
        t64    Time64    NOT NULL,
        t64n   Time64(6) NOT NULL
    ) ENGINE = MergeTree PARTITION BY id ORDER BY (id);
$$);

SELECT clickhouse_raw_query($$
    INSERT INTO time_test.times (id, t32, t64, t64n) VALUES
        (1, '12:43:53', '12:43:53.123', '12:43:53.123456'),
        (2, '03:27:09', '03:27:09.987', '03:27:09.987654'),
        (3, '12:43:53', '12:43:53.123', '12:43:53.123456')
$$);

CREATE SCHEMA time_bin;
CREATE SCHEMA time_http;
IMPORT FOREIGN SCHEMA time_test FROM SERVER binary_time_loopback INTO time_bin;
\d time_bin.times
IMPORT FOREIGN SCHEMA time_test FROM SERVER http_time_loopback INTO time_http;
\d time_http.times


SELECT * FROM time_http.times ORDER BY id;
SELECT * FROM time_bin.times ORDER BY id;

SELECT clickhouse_raw_query('DROP DATABASE time_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_time_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_time_loopback;
DROP SERVER binary_time_loopback CASCADE;
DROP SERVER http_time_loopback CASCADE;
