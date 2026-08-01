CREATE SERVER casts_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'casts_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER casts_loopback;

CREATE SERVER casts_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER casts_admin;

CALL clickhouse_perform('casts_admin', 'DROP DATABASE IF EXISTS casts_test');
CALL clickhouse_perform('casts_admin', 'CREATE DATABASE casts_test');

CALL clickhouse_perform('casts_admin', $$
	CREATE TABLE casts_test.things (
        num  integer,
        name text
    ) ENGINE = MergeTree ORDER BY (num);
$$);

CALL clickhouse_perform('casts_admin', $$
	INSERT INTO casts_test.things
	SELECT number, toString(number)
	  FROM numbers(10);
$$);

CREATE SCHEMA casts_test;
IMPORT FOREIGN SCHEMA casts_test FROM SERVER casts_loopback INTO casts_test;
SET search_path = casts_test, public;

EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUInt8(num) IN (8, 3, 5);
SELECT num FROM things WHERE toUInt8(num) IN (8, 3, 5);
EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUInt8(name) IN (1, 2, 3);
SELECT num FROM things WHERE toUInt8(name) IN (1, 2, 3);

EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint16(num) IN (8, 3, 5);
SELECT num FROM things WHERE toUint16(num) IN (8, 3, 5);
EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint16(name) IN (1, 2, 3);
SELECT num FROM things WHERE toUint16(name) IN (1, 2, 3);

EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint32(num) IN (8, 3, 5);
SELECT num FROM things WHERE toUint32(num) IN (8, 3, 5);
EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint32(name) IN (1, 2, 3);
SELECT num FROM things WHERE toUint32(name) IN (1, 2, 3);

EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint64(num) IN (8, 3, 5);
SELECT num FROM things WHERE toUint64(num) IN (8, 3, 5);
EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint64(name) IN (1, 2, 3);
SELECT num FROM things WHERE toUint64(name) IN (1, 2, 3);

EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint128(num) IN (8, 3, 5);
SELECT num FROM things WHERE toUint128(num) IN (8, 3, 5);
EXPLAIN (VERBOSE, COSTS OFF) SELECT num FROM things WHERE toUint128(name) IN (1, 2, 3);
SELECT num FROM things WHERE toUint128(name) IN (1, 2, 3);

DROP USER MAPPING FOR CURRENT_USER SERVER casts_loopback;
CALL clickhouse_perform('casts_admin', 'DROP DATABASE casts_test');
DROP SERVER casts_loopback CASCADE;
