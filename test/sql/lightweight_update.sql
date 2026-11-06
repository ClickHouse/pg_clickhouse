CREATE SERVER lw_update_admin FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER lw_update_admin;

\set ECHO errors
SELECT (regexp_split_to_array(clickhouse_server_version('lw_update_admin'), '\.'))[1:2]::int[]
    < '{25,7}'::int[] AS old_ch \gset
\set ECHO all
\if :old_ch
SET client_min_messages = warning;
DROP SERVER lw_update_admin CASCADE;
\echo 'SKIP: lightweight UPDATE requires ClickHouse 25.7'
\quit
\endif

CREATE SERVER lw_update_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'lw_update_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER lw_update_svr;
CREATE SERVER lw_update_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'lw_update_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER lw_update_bin;

CALL clickhouse_perform('lw_update_admin', 'DROP DATABASE IF EXISTS lw_update_test');
CALL clickhouse_perform('lw_update_admin', 'CREATE DATABASE lw_update_test');

CALL clickhouse_perform('lw_update_admin', $$
    CREATE TABLE lw_update_test.items (
        id Int32, name String, qty Int32, note Nullable(String)
    ) ENGINE = MergeTree ORDER BY id
    SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
$$);
CALL clickhouse_perform('lw_update_admin', $$
    INSERT INTO lw_update_test.items
    SELECT number, 'item' || toString(number), number * 10, NULL FROM numbers(1, 6)
$$);

CREATE SCHEMA lw_update;
SET search_path = lw_update, public;
CREATE FOREIGN TABLE items (id int, name text, qty int, note text)
    SERVER lw_update_svr;
CREATE FOREIGN TABLE items_bin (id int, name text, qty int, note text)
    SERVER lw_update_bin OPTIONS (table_name 'items');
CREATE FOREIGN TABLE items_renamed (key int OPTIONS (column_name 'id'), label text OPTIONS (column_name 'name'))
    SERVER lw_update_svr OPTIONS (table_name 'items');

-- Run UPDATE as one ClickHouse statement
EXPLAIN (VERBOSE, COSTS OFF) UPDATE items SET qty = qty + 1 WHERE id = 1;
UPDATE items SET qty = qty + 1 WHERE id = 1;
SELECT * FROM items ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
UPDATE items SET name = upper(name), note = 'it''s' WHERE qty > 30 AND note IS NULL;
UPDATE items SET name = upper(name), note = 'it''s' WHERE qty > 30 AND note IS NULL;
SELECT * FROM items ORDER BY id;

UPDATE items SET note = NULL WHERE id = 4;
SELECT * FROM items WHERE id = 4;

-- Binary driver
UPDATE items_bin SET qty = -qty WHERE name = 'item2';
SELECT * FROM items_bin ORDER BY id;

-- column_name option
EXPLAIN (VERBOSE, COSTS OFF) UPDATE items_renamed SET label = 'third' WHERE key = 3;
UPDATE items_renamed SET label = 'third' WHERE key = 3;
SELECT * FROM items_renamed ORDER BY key;

-- Send prepared statement arguments as ClickHouse query parameters
SET plan_cache_mode = force_generic_plan;
PREPARE upd(int, text) AS UPDATE items SET note = $2 WHERE id = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE upd(5, 'five');
EXECUTE upd(5, 'five');
PREPARE upd_bin(int, text) AS UPDATE items_bin SET note = $2 WHERE id = $1;
EXECUTE upd_bin(6, 'six');
RESET plan_cache_mode;
SELECT * FROM items ORDER BY id;

-- Run subquery in Postgres and send result as query parameter
CREATE TABLE local_ids (id int, qty int);
INSERT INTO local_ids VALUES (1, 100);
UPDATE items SET qty = (SELECT max(qty) FROM local_ids) WHERE id = 1;
SELECT * FROM items WHERE id = 1;

-- Update local and ClickHouse partitions in same statement
SET datestyle = 'ISO';
CALL clickhouse_perform('lw_update_admin', $$
    CREATE TABLE lw_update_test.events (id Int32, ts Date, qty Int32)
    ENGINE = MergeTree ORDER BY id
    SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
$$);
CALL clickhouse_perform('lw_update_admin', $$
    INSERT INTO lw_update_test.events VALUES (1, '2023-01-15', 10), (2, '2023-06-01', 20)
$$);
CREATE TABLE events (id int, ts date, qty int) PARTITION BY RANGE (ts);
CREATE FOREIGN TABLE events_cold PARTITION OF events
    FOR VALUES FROM ('2023-01-01') TO ('2024-01-01')
    SERVER lw_update_svr OPTIONS (table_name 'events');
CREATE TABLE events_hot PARTITION OF events
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
INSERT INTO events_hot VALUES (3, '2024-01-10', 30), (4, '2024-02-15', 40);
EXPLAIN (VERBOSE, COSTS OFF) UPDATE events SET qty = qty + 1 WHERE id IN (2, 4);
UPDATE events SET qty = qty + 1 WHERE id IN (2, 4);
SELECT id, ts, qty FROM events ORDER BY id;

-- Reject partition key updates through parent and leaf
UPDATE events SET ts = '2024-06-01' WHERE ts = '2023-06-01' AND id = 2;
UPDATE events_cold SET ts = '2024-06-01' WHERE id = 2;
SELECT tableoid::regclass, id, ts, qty FROM events WHERE ts = '2023-06-01';
SELECT id FROM events WHERE ts = '2024-06-01';

-- Include ancestor expressions and default partition constraints
CREATE TABLE events_expr (id int, ts date, qty int)
    PARTITION BY RANGE ((extract(year FROM ts)));
CREATE TABLE events_expr_2023 PARTITION OF events_expr
    FOR VALUES FROM (2023) TO (2024) PARTITION BY LIST (id);
CREATE TABLE events_expr_other PARTITION OF events_expr_2023 FOR VALUES IN (99);
CREATE FOREIGN TABLE events_expr_default PARTITION OF events_expr_2023 DEFAULT
    SERVER lw_update_bin OPTIONS (table_name 'events');
UPDATE events_expr_default SET ts = '2024-06-01' WHERE id = 2;
UPDATE events_expr_default SET id = 99 WHERE id = 2;
UPDATE events_expr SET qty = qty + 1 WHERE id = 1;
SELECT id, ts, qty FROM events_expr WHERE id = 1;

-- Reject statements that cannot run entirely in ClickHouse
UPDATE items SET qty = 0 WHERE id = 1 RETURNING id;
CREATE FUNCTION local_double(int) RETURNS int LANGUAGE plpgsql AS 'BEGIN RETURN $1 * 2; END';
UPDATE items SET qty = local_double(qty) WHERE id = 1;
UPDATE items SET qty = 0 WHERE local_double(id) = 2;
UPDATE items SET qty = local_ids.qty FROM local_ids WHERE items.id = local_ids.id;
CREATE FUNCTION no_op() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN NEW; END';
CREATE TRIGGER items_upd BEFORE UPDATE ON items FOR EACH ROW EXECUTE FUNCTION no_op();
UPDATE items SET qty = 0 WHERE id = 1;
DROP TRIGGER items_upd ON items;
SELECT * FROM items ORDER BY id;

-- ClickHouse requires WHERE
EXPLAIN (VERBOSE, COSTS OFF) UPDATE items SET qty = 0;
UPDATE items SET qty = 0;
SELECT id, qty FROM items ORDER BY id;

-- Support ReplacingMergeTree and CollapsingMergeTree.
CALL clickhouse_perform('lw_update_admin', $$
    CREATE TABLE lw_update_test.replacing (id Int32, name String)
    ENGINE = ReplacingMergeTree ORDER BY id
    SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
$$);
CALL clickhouse_perform('lw_update_admin', $$
    INSERT INTO lw_update_test.replacing VALUES (1, 'Sushi'), (2, 'Bagel')
$$);
CALL clickhouse_perform('lw_update_admin', $$
    CREATE TABLE lw_update_test.collapsing (id Int32, name String, sign Int8)
    ENGINE = CollapsingMergeTree(sign) ORDER BY id
    SETTINGS enable_block_number_column = 1, enable_block_offset_column = 1
$$);
CALL clickhouse_perform('lw_update_admin', $$
    INSERT INTO lw_update_test.collapsing VALUES (1, 'Little Bit', 1), (2, 'Biscuit', 1)
$$);
CREATE FOREIGN TABLE replacing (id int, name text)
    SERVER lw_update_bin OPTIONS (table_name 'replacing');
CREATE FOREIGN TABLE collapsing (id int, name text)
    SERVER lw_update_bin OPTIONS (table_name 'collapsing');

UPDATE replacing SET name = 'Sushi May' WHERE id = 1;
SELECT * FROM replacing ORDER BY id;
UPDATE collapsing SET name = 'Sweet Pea' WHERE id = 1;
SELECT * FROM collapsing ORDER BY id;

CALL clickhouse_perform('lw_update_admin', 'DROP DATABASE lw_update_test');
SET client_min_messages = warning;
DROP SCHEMA lw_update CASCADE;
DROP SERVER lw_update_svr CASCADE;
DROP SERVER lw_update_bin CASCADE;
DROP SERVER lw_update_admin CASCADE;
