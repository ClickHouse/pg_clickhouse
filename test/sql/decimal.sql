SET datestyle = 'ISO';
CREATE SERVER binary_decimal_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'decimal_test', driver 'binary');
CREATE SERVER http_decimal_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'decimal_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_decimal_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_decimal_loopback;

CREATE SERVER decimal_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER decimal_admin;

CALL clickhouse_perform('decimal_admin', 'DROP DATABASE IF EXISTS decimal_test');
CALL clickhouse_perform('decimal_admin', 'CREATE DATABASE decimal_test');

\set ECHO errors
SELECT split_part(clickhouse_server_version('binary_decimal_loopback'), '.', 1)::int <= 23 AS ch23 \gset
\if :ch23
\set bare_dec (10, 0)
\else
\set bare_dec
\endif
\set ECHO all

CALL clickhouse_perform('decimal_admin', format($$
    CREATE TABLE decimal_test.decimals (
        id     Int32          NOT NULL,
        dec    Decimal(8, 0)  NOT NULL,
        dec32  Decimal32(4)   NOT NULL,
        dec64  Decimal64(6)   NOT NULL,
        dec128 Decimal128(8)  NOT NULL,
        dec0   Decimal%s      NOT NULL,
        f64    Float64        NOT NULL
    ) ENGINE = MergeTree PARTITION BY id ORDER BY (id);
$$, :'bare_dec'));

CREATE SCHEMA dec_bin;
CREATE SCHEMA dec_http;
IMPORT FOREIGN SCHEMA "decimal_test" FROM SERVER binary_decimal_loopback INTO dec_bin;
\d dec_bin.decimals
IMPORT FOREIGN SCHEMA "decimal_test" FROM SERVER http_decimal_loopback INTO dec_http;
\d dec_http.decimals

INSERT INTO dec_bin.decimals (id, dec, dec32, dec64, dec128, dec0, f64) VALUES
    (1, 42::NUMERIC, 98.6::NUMERIC, 102.4::NUMERIC, 1024.003::NUMERIC, 66::NUMERIC, 1024.003),
    (2, 9999, 9999.9999, 9999999.999999, 99999999999.99999999, 9999999999, 2048.006),
    (3, -9999, -9999.9999, -9999999.999999, -99999999999.99999999, -9999999999, -2048.006)
;

INSERT INTO dec_http.decimals VALUES
    (4, 1000000::NUMERIC, 10000::NUMERIC, 3000000000::NUMERIC, 400000000000::NUMERIC, 67::NUMERIC, 4096.012),
    (5, -1, -0.0001, -0.000001, -0.00000001, -1111111111, -1.5),
    (6, 0, 0, 0, 0, 0, 0)
;

SELECT * FROM dec_bin.decimals ORDER BY id;
SELECT * FROM dec_http.decimals ORDER BY id;

-- Casting to an unconstrained numeric pushes down to the max decimal size,
-- Decimal(76, 38), but results should not have trailing zeros.
EXPLAIN (VERBOSE, COSTS OFF) SELECT round(avg(f64)::numeric, 4) FROM dec_bin.decimals;
SELECT round(avg(f64)::numeric, 4) FROM dec_bin.decimals;
EXPLAIN (VERBOSE, COSTS OFF) SELECT round(avg(f64)::numeric) FROM dec_bin.decimals WHERE id = 5;
SELECT round(avg(f64)::numeric) FROM dec_bin.decimals WHERE id = 5;

EXPLAIN (VERBOSE, COSTS OFF) SELECT round(avg(f64)::numeric, 4) FROM dec_http.decimals;
SELECT round(avg(f64)::numeric, 4) FROM dec_http.decimals;
EXPLAIN (VERBOSE, COSTS OFF) SELECT round(avg(f64)::numeric) FROM dec_http.decimals WHERE id = 5;
SELECT round(avg(f64)::numeric) FROM dec_http.decimals WHERE id = 5;

CALL clickhouse_perform('decimal_admin', 'DROP DATABASE decimal_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_decimal_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_decimal_loopback;
DROP SERVER binary_decimal_loopback CASCADE;
DROP SERVER http_decimal_loopback CASCADE;
