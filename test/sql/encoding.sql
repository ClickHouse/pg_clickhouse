\unset ECHO
/*
 * This test must be run in a database with UTF-8 or SQL_ASCII encoding, as
 * we have expected output files only for thowse encodings.
 */
SELECT getdatabaseencoding() NOT IN ('UTF8', 'SQL_ASCII') AS skip_test
  FROM pg_database
 WHERE datname=current_database() \gset
\if :skip_test
\echo 'SKIP: can only test UTF8 or SQL_ASCII encodings'
\quit
\endif
\set ECHO all

-- Create servers for each engine.
CREATE SERVER encoding_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER encoding_bin_svr;

CREATE SERVER encoding_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER encoding_http_svr;

-- Create the schema in ClickHouse.
CREATE SERVER encoding_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER encoding_admin;

CALL clickhouse_perform('encoding_admin', 'DROP DATABASE IF EXISTS encoding_test');
CALL clickhouse_perform('encoding_admin', 'CREATE DATABASE encoding_test');
CALL clickhouse_perform('encoding_admin', $$
    CREATE TABLE encoding_test.things (
        id    Int,
        name  String,
        value String
    ) ENGINE = MergeTree
    PRIMARY KEY id
$$);

-- Insert some data.
CALL clickhouse_perform('encoding_admin', $$
    INSERT INTO encoding_test.things
    VALUES (1, 'valid', 'acn'),
           (2, 'nul byte', 'a\x00n')
           (3, 'nul & invalid octet', 'a\x00c\x80n'),
           (4, 'invalid octet & nul', 'a\x80n\x00e'),
           (5, 'valid 2-octet sequence', 'a\x\xc3\xb1b'),
           (6, 'invalid 2-octet sequence', 'a\xc\x28b')
$$);

-- ===================================================================
-- Create Foreign tables.
-- ===================================================================
CREATE SCHEMA encoding_bin;
CREATE SCHEMA encoding_http;
IMPORT FOREIGN SCHEMA encoding_test FROM SERVER encoding_bin_svr INTO encoding_bin;
\d encoding_bin.*
IMPORT FOREIGN SCHEMA encoding_test FROM SERVER encoding_http_svr INTO encoding_http;
\d encoding_http.*

-- Should fail on invalid bytes (tests assume UTF-8 encoding).
SELECT * FROM encoding_bin.things ORDER BY id;
SELECT * FROM encoding_http.things ORDER BY id;

-- Explicit fail.
ALTER SERVER encoding_bin_svr OPTIONS (ADD encoding_check 'fail');
ALTER SERVER encoding_http_svr OPTIONS (ADD encoding_check 'FAIL');
SELECT * FROM encoding_bin.things ORDER BY id;
SELECT * FROM encoding_http.things ORDER BY id;

-- Truncate.
ALTER SERVER encoding_bin_svr OPTIONS (SET encoding_check 'truncate');
ALTER SERVER encoding_http_svr OPTIONS (SET encoding_check 'TRUNCATE');
SELECT * FROM encoding_bin.things ORDER BY id;
SELECT * FROM encoding_http.things ORDER BY id;

-- Replace.
ALTER SERVER encoding_bin_svr OPTIONS (SET encoding_check 'replace');
ALTER SERVER encoding_http_svr OPTIONS (SET encoding_check 'Replace');
SELECT * FROM encoding_bin.things ORDER BY id;
SELECT * FROM encoding_http.things ORDER BY id;

-- Remove.
ALTER SERVER encoding_bin_svr OPTIONS (SET encoding_check 'remove');
ALTER SERVER encoding_http_svr OPTIONS (SET encoding_check 'reMove');
SELECT * FROM encoding_bin.things ORDER BY id;
SELECT * FROM encoding_http.things ORDER BY id;

-- Invalid encoding_check.
ALTER SERVER encoding_bin_svr OPTIONS (SET encoding_check 'nonesuch');
ALTER SERVER encoding_http_svr OPTIONS (SET encoding_check 'nonesuch');

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER encoding_bin_svr;
DROP SERVER encoding_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER encoding_http_svr;
DROP SERVER encoding_http_svr CASCADE;
