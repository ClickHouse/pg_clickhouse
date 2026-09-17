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

-- Integers wider than bigint import as numeric
CALL clickhouse_perform('import_types_admin', 'CREATE TABLE import_types_test.wide (
    id Int32, i128 Int128, u128 UInt128, i256 Int256, u256 UInt256
) ENGINE = MergeTree ORDER BY (id);
');
CALL clickhouse_perform('import_types_admin', 'INSERT INTO import_types_test.wide VALUES (
    1,
    toInt128(''-170141183460469231731687303715884105728''),
    toUInt128(''340282366920938463463374607431768211455''),
    toInt256(''-57896044618658097711785492504343953926634992332820282019728792003956564819968''),
    toUInt256(''115792089237316195423570985008687907853269984665640564039457584007913129639935'')
)');
CREATE SCHEMA import_types_wide;
IMPORT FOREIGN SCHEMA import_types_test LIMIT TO (wide)
    FROM SERVER import_types_loopback INTO import_types_wide;

SELECT attname, format_type(atttypid, atttypmod) AS type
  FROM pg_attribute
 WHERE attrelid = 'import_types_wide.wide'::regclass AND attnum > 0
 ORDER BY attnum;

SELECT * FROM import_types_wide.wide;

-- ClickHouse flattens Nested at creation, even when import disables flattening
BEGIN;
DO $$
BEGIN
    PERFORM set_config(
        'pg_clickhouse.session_settings',
        concat_ws(', ', nullif(current_setting('pg_clickhouse.session_settings'), ''),
                  'flatten_nested 1'),
        true
    );
END
$$;
CALL clickhouse_perform('import_types_admin', 'CREATE TABLE import_types_test.flattened (
    id Int32, items Nested(a Int32, b Decimal(9,4))
) ENGINE = MergeTree ORDER BY (id)');
CALL clickhouse_perform('import_types_admin', 'INSERT INTO import_types_test.flattened
    VALUES (1, [10, 20], [1.5, -2.25])');
COMMIT;

-- Keep Nested fields together for import
BEGIN;
DO $$
BEGIN
    PERFORM set_config(
        'pg_clickhouse.session_settings',
        concat_ws(', ', nullif(current_setting('pg_clickhouse.session_settings'), ''),
                  'flatten_nested 0'),
        true
    );
END
$$;
CALL clickhouse_perform('import_types_admin', 'CREATE TABLE import_types_test.opened (
    id      Int32,
    items   Nested(a Int32, b Decimal(9,4)),
    pairs   Array(Nested(k String, v Int64)),
    total   SimpleAggregateFunction(sum, Int64),
    latest  SimpleAggregateFunction(max, Nullable(DateTime64(9))),
    seen    AggregateFunction(count),
    spread  AggregateFunction(quantiles(0.5, 0.9), Int32)
) ENGINE = MergeTree ORDER BY (id);
');
CREATE SCHEMA import_types_opened;
IMPORT FOREIGN SCHEMA import_types_test LIMIT TO (flattened)
    FROM SERVER import_types_loopback INTO import_types_opened;
IMPORT FOREIGN SCHEMA import_types_test LIMIT TO (opened)
    FROM SERVER import_types_loopback INTO import_types_opened;
COMMIT;

SELECT attname, format_type(atttypid, atttypmod) AS type, attndims
  FROM pg_attribute
 WHERE attrelid = 'import_types_opened.flattened'::regclass AND attnum > 0
 ORDER BY attnum;

SELECT id, "items.a", "items.b" FROM import_types_opened.flattened;

SELECT attname, format_type(atttypid, atttypmod) AS type, attndims, attnotnull,
       attfdwoptions
  FROM pg_attribute
 WHERE attrelid = 'import_types_opened.opened'::regclass AND attnum > 0
 ORDER BY attnum;

CALL clickhouse_perform('import_types_admin', 'INSERT INTO import_types_test.opened
    (id, items, pairs, total, latest) VALUES (
    1, [(10, 1.5), (20, -2.25)], [[(''k'', 42)]], 7,
    ''2026-08-19 03:04:05.678901234''
)');
SELECT id, items, pairs, total, latest FROM import_types_opened.opened;

CREATE TYPE import_types_item AS (a integer, b numeric(9,4));
CREATE TYPE import_types_label AS ENUM ('one', 'two');
CREATE TYPE import_types_map_pair AS (key text, value bigint);
CREATE TYPE import_types_tuple AS (number integer, name text);
CREATE FOREIGN TABLE import_types_opened.records (
    id    integer,
    items import_types_item[]
) SERVER import_types_loopback OPTIONS (table_name 'opened');
CREATE FOREIGN TABLE import_types_opened.mapped_records (
    id    integer,
    label import_types_label,
    pairs import_types_map_pair[],
    pair  import_types_tuple
) SERVER import_types_loopback OPTIONS (table_name 'mapped');
SELECT * FROM import_types_opened.records;
SELECT label, pairs, pair FROM import_types_opened.mapped_records;

-- JSON parameters require ClickHouse 25.1 or newer
\set ECHO errors
SELECT split_part(clickhouse_server_version('import_types_loopback'), '.', 1)::int >= 25
    AS json_params \gset
\if :json_params
CALL clickhouse_perform('import_types_admin', 'CREATE TABLE import_types_test.doc (
    id Int32,
    doc JSON(
        data Array(Tuple(field String, value String)),
        fallback String,
        filters Array(Tuple(field String, type String, value String)),
        pricing_plan_subscription_id UInt32,
        rules Array(Tuple(field String, type String))
    )
) ENGINE = MergeTree ORDER BY (id);
');
IMPORT FOREIGN SCHEMA import_types_test LIMIT TO (doc)
    FROM SERVER import_types_loopback INTO import_types_opened;
SELECT format_type(atttypid, atttypmod) AS doc_type
  FROM pg_attribute
 WHERE attrelid = 'import_types_opened.doc'::regclass AND attname = 'doc';
DROP FOREIGN TABLE import_types_opened.doc;
\else
SELECT 'jsonb' AS doc_type;
\endif
\set ECHO all

DROP USER MAPPING FOR CURRENT_USER SERVER import_types_loopback;
CALL clickhouse_perform('import_types_admin', 'DROP DATABASE import_types_test');
DROP SERVER import_types_loopback CASCADE;
