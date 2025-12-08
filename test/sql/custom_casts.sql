CREATE SERVER casts_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'casts_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER casts_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS casts_test');
SELECT clickhouse_raw_query('CREATE DATABASE casts_test');

SELECT clickhouse_raw_query($$
	CREATE TABLE casts_test.things (
        num  integer,
        name text
    ) ENGINE = MergeTree ORDER BY (num);
$$);

SELECT clickhouse_raw_query($$
	INSERT INTO casts_test.things
	SELECT number, toString(number)
	  FROM numbers(10);
$$);

IMPORT FOREIGN SCHEMA casts_test FROM SERVER casts_loopback INTO public;

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
SELECT clickhouse_raw_query('DROP DATABASE casts_test');
DROP SERVER casts_loopback CASCADE;
