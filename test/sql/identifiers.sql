SET standard_conforming_strings = on;
CREATE SERVER qi_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER qi_admin;

CALL clickhouse_perform('qi_admin', $$CREATE DATABASE "qi\\n""db"$$);
CALL clickhouse_perform('qi_admin', $$
    CREATE TABLE "qi\\n""db"."ta\\ble""" (
        id Int32, "co\\n""l" Int32, "end\\" Int32, "CamelCase" Int32,
        "null" Int32, "true" Int32, "false" Int32,
        "nan" Int32, "inf" Int32, "not" Int32, "all" Int32, "distinct" Int32
    ) ENGINE = MergeTree ORDER BY id
$$);

CREATE SERVER qi_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'qi\n"db', driver 'http');
CREATE SERVER qi_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'qi\n"db', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER qi_http;
CREATE USER MAPPING FOR CURRENT_USER SERVER qi_bin;

CREATE FOREIGN TABLE qi_http (
    id integer, escaped integer OPTIONS (column_name 'co\n"l'),
    escaped_end integer OPTIONS (column_name 'end\'), "CamelCase" integer,
    "null" integer, "true" integer, "false" integer,
    nan integer, inf integer, "not" integer, "all" integer, "distinct" integer
) SERVER qi_http OPTIONS (table_name 'ta\ble"');
CREATE FOREIGN TABLE qi_bin (
    id integer, escaped integer OPTIONS (column_name 'co\n"l'),
    escaped_end integer OPTIONS (column_name 'end\'), "CamelCase" integer,
    "null" integer, "true" integer, "false" integer,
    nan integer, inf integer, "not" integer, "all" integer, "distinct" integer
) SERVER qi_bin OPTIONS (table_name 'ta\ble"');

INSERT INTO qi_http VALUES (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
INSERT INTO qi_bin VALUES (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13);
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM qi_http ORDER BY id;
SELECT * FROM qi_http ORDER BY id;
SELECT * FROM qi_bin ORDER BY id;

CREATE SCHEMA qi_http;
CREATE SCHEMA qi_bin;
IMPORT FOREIGN SCHEMA "qi\n""db" FROM SERVER qi_http INTO qi_http;
IMPORT FOREIGN SCHEMA "qi\n""db" FROM SERVER qi_bin INTO qi_bin;
SELECT "co\n""l", "end\", "CamelCase", "null", "true", "false", nan, inf
FROM qi_http."ta\ble""" ORDER BY id;
SELECT "co\n""l", "end\", "CamelCase", "null", "true", "false", nan, inf
FROM qi_bin."ta\ble""" ORDER BY id;

DROP FOREIGN TABLE qi_http, qi_bin, qi_http."ta\ble""", qi_bin."ta\ble""";
DROP SCHEMA qi_http, qi_bin;
DROP USER MAPPING FOR CURRENT_USER SERVER qi_http;
DROP USER MAPPING FOR CURRENT_USER SERVER qi_bin;
DROP SERVER qi_http, qi_bin;
CALL clickhouse_perform('qi_admin', $$DROP DATABASE "qi\\n""db"$$);
DROP USER MAPPING FOR CURRENT_USER SERVER qi_admin;
DROP SERVER qi_admin;
