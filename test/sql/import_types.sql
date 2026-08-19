-- Import every ClickHouse type whose PostgreSQL column takes a modifier or
-- more than one dimension. Read columns from the catalog rather than \d+, whose
-- footer differs between PostgreSQL versions
SET TimeZone = 'UTC';
SET DateStyle = 'ISO, MDY';

CREATE SERVER import_types_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'import_types_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER import_types_loopback;

CREATE SERVER import_types_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER import_types_admin;

CALL clickhouse_perform('import_types_admin', 'DROP DATABASE IF EXISTS import_types_test');
CALL clickhouse_perform('import_types_admin', 'CREATE DATABASE import_types_test');

CALL clickhouse_perform('import_types_admin', 'CREATE TABLE import_types_test.mapped (
    id          Int32,
    dec         Decimal(12,6),
    dec_null    Nullable(Decimal(12,6)),
    dec_arr     Array(Decimal(9,4)),
    dec_nest    Array(Array(Decimal(9,4))),
    stamp       DateTime64(3),
    stamp_arr   Array(DateTime64(6)),
    stamp_cap   DateTime64(9),
    fixed       FixedString(5),
    fixed_arr   Array(FixedString(3)),
    label       Enum8(''one'' = 1, ''two'' = 2),
    card        LowCardinality(Nullable(String)),
    pairs       Map(String, Int64),
    pair        Tuple(Int32, String)
) ENGINE = MergeTree ORDER BY (id);
');

CALL clickhouse_perform('import_types_admin', 'INSERT INTO import_types_test.mapped VALUES (
    1, 1.5, NULL, [1.25, -2.5], [[1.25], [2.5]],
    ''2026-08-19 03:04:05.678'', [''2026-08-19 03:04:05.678901''],
    ''2026-08-19 03:04:05.678901234'',
    ''abcde'', [''xy'', ''z''], ''two'', ''card'', {''k'': 42}, (7, ''seven'')
)');

CREATE SCHEMA import_types;
IMPORT FOREIGN SCHEMA import_types_test FROM SERVER import_types_loopback
    INTO import_types;

-- Leaf modifiers survive Array nesting, and attndims keeps ClickHouse depth
SELECT attname, format_type(atttypid, atttypmod) AS type, attndims, attnotnull
  FROM pg_attribute
 WHERE attrelid = 'import_types.mapped'::regclass AND attnum > 0
 ORDER BY attnum;

SELECT * FROM import_types.mapped;

-- Reject a ClickHouse type PostgreSQL cannot hold
CALL clickhouse_perform('import_types_admin', 'CREATE TABLE import_types_test.wide (
    id Int32, big Int128
) ENGINE = MergeTree ORDER BY (id);
');
CREATE SCHEMA import_types_wide;
IMPORT FOREIGN SCHEMA import_types_test LIMIT TO (wide)
    FROM SERVER import_types_loopback INTO import_types_wide;

DROP USER MAPPING FOR CURRENT_USER SERVER import_types_loopback;
CALL clickhouse_perform('import_types_admin', 'DROP DATABASE import_types_test');
DROP SERVER import_types_loopback CASCADE;
