CREATE SERVER operators_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'operators_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER operators_loopback;
CREATE SERVER operators_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER operators_admin;

CALL clickhouse_perform('operators_admin', 'DROP DATABASE IF EXISTS operators_test');
CALL clickhouse_perform('operators_admin', 'CREATE DATABASE operators_test');
CALL clickhouse_perform('operators_admin', $$
    CREATE TABLE operators_test.nums (
        id  Int32,
        u8  UInt8,
        i16 Int16,
        i32 Int32,
        i64 Int64,
        f64 Float64,
        n   Decimal(10, 2)
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('operators_admin', $$
    INSERT INTO operators_test.nums VALUES
        (1, 200, -3, -7, 9007199254740993, 8, -1.5),
        (2, 5, 16384, 1, -8, 4, 2.5)
$$);

CREATE FOREIGN TABLE nums (
    id  int,
    u8  smallint,
    i16 smallint,
    i32 int,
    i64 bigint,
    f64 float8,
    n   numeric(10, 2)
) SERVER operators_loopback;

-- Cast integer arguments to PostgreSQL types, as ClickHouse keeps UInt8 and Int16.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE u8 & -1::smallint = 200 AND ~u8 = -201 AND u8 # -1::smallint = -201;
SELECT id FROM nums WHERE u8 & -1::smallint = 200 AND ~u8 = -201 AND u8 # -1::smallint = -201;
SELECT id FROM nums WHERE i16 | 3::smallint = -1;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE u8 << 1 = 400;
SELECT id FROM nums WHERE u8 << 1 = 400;
SELECT id FROM nums WHERE i16 << 2 = 0;
SELECT id FROM nums WHERE i64 >> 1 = -4;

-- Push down variable counts and counts beyond integer width.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE i32 << 32 = -7;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE i32 << i32 = 2;

-- Divide integers exactly, rounding toward zero.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE i64 / 1 = 9007199254740993 AND i32 / 2 = -3;
SELECT id FROM nums WHERE i64 / 1 = 9007199254740993 AND i32 / 2 = -3;

-- Map math operators to ClickHouse functions.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE f64 ^ 2 = 64 AND ||/ f64 = 2 AND @ i32 = 7 AND @ n = 1.5;
SELECT id FROM nums WHERE f64 ^ 2 = 64 AND ||/ f64 = 2 AND @ i32 = 7 AND @ n = 1.5;
SELECT id FROM nums WHERE |/ f64 = 2;

CALL clickhouse_perform('operators_admin', 'DROP DATABASE operators_test');
DROP USER MAPPING FOR CURRENT_USER SERVER operators_loopback;
DROP SERVER operators_loopback CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER operators_admin;
DROP SERVER operators_admin;
