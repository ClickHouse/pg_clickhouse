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
        f32 Float32,
        f64 Float64,
        n   Decimal(10, 2),
        ip  IPv4,
        ip6 IPv6,
        bin String,
        uid UUID
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('operators_admin', $$
    INSERT INTO operators_test.nums VALUES
        (1, 200, -3, -7, 9007199254740993, 0.1, 8, -1.5, '10.0.0.1', '::1', unhex('01'),
         '00000000-0000-0001-0000-000000000000'),
        (2, 5, 16384, 1, -8, 0.3, 4, 2.5, '10.0.0.2', '2001:db8::1', '',
         '00000000-0000-0000-0000-000000000001')
$$);

CREATE FOREIGN TABLE nums (
    id  int,
    u8  smallint,
    i16 smallint,
    i32 int,
    i64 bigint,
    f32 real,
    f64 float8,
    n   numeric(10, 2),
    ip  inet,
    ip6 inet,
    bin bytea,
    uid uuid
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

-- Round real arithmetic and constants to Float32.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE f32 + 0.2::real = 0.3::real;
SELECT id FROM nums WHERE f32 + 0.2::real = 0.3::real;
SELECT id FROM nums WHERE f32 = 0.3::real;
SELECT id FROM nums WHERE f32 IN (0.3::real, 0.7::real);

-- Use divideDecimal to keep decimal places when dividing numeric values.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE n / 3 > 0.83 AND n / 0.1 = 25;
SELECT id FROM nums WHERE n / 3 > 0.83 AND n / 0.1 = 25;

-- Use Decimal literals to avoid rounding fractions and integers beyond UInt64 range.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE n > 2.4999999999999999 OR i64::numeric * 10000 = 90071992547409930000;
SELECT id FROM nums WHERE n > 2.4999999999999999 OR i64::numeric * 10000 = 90071992547409930000 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE n * 2 = -3;

-- Compare inet only for equality.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE ip = '10.0.0.1' AND ip6 <> '2001:db8::1';
SELECT id FROM nums WHERE ip = '10.0.0.1' AND ip6 <> '2001:db8::1';
SELECT id FROM nums WHERE ip IN ('10.0.0.2', '10.0.0.9');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE ip < '10.0.0.2' OR ip::text = '10.0.0.2/32' ORDER BY id;
SELECT id FROM nums WHERE ip < '10.0.0.2' OR ip::text = '10.0.0.2/32' ORDER BY id;

-- Concatenate bytea.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE bin || '\x02'::bytea = '\x0102'::bytea;
SELECT id FROM nums WHERE bin || '\x02'::bytea = '\x0102'::bytea;

-- Compare UUIDs only for equality, as ClickHouse orders UUIDs differently.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE uid = '00000000-0000-0000-0000-000000000001';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM nums WHERE uid > '00000000-0000-0000-0000-000000000001';
SELECT id FROM nums WHERE uid > '00000000-0000-0000-0000-000000000001';

-- Plan other operators without ClickHouse equivalent locally.
CREATE FOREIGN TABLE plan_only (
    j  jsonb,
    tm time,
    ts timestamp,
    d  date
) SERVER operators_loopback OPTIONS (table_name 'nums');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 1 FROM plan_only WHERE j - 'a' = '{}' OR j < '{"a": 1}';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 1 FROM plan_only WHERE j = ANY (ARRAY['{}'::jsonb]);
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 1 FROM plan_only WHERE tm + interval '2 hours' < '01:30' OR d + tm > now();
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 1 FROM plan_only WHERE ts - ts > interval '1 month';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 1 FROM plan_only WHERE ts - ts > interval '1 hour' AND d + interval '1 month' > now();

CALL clickhouse_perform('operators_admin', 'DROP DATABASE operators_test');
DROP USER MAPPING FOR CURRENT_USER SERVER operators_loopback;
DROP SERVER operators_loopback CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER operators_admin;
DROP SERVER operators_admin;
