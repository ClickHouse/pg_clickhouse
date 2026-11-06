CREATE SERVER lw_delete_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'lw_delete_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER lw_delete_svr;
CREATE SERVER lw_delete_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'lw_delete_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER lw_delete_bin;
CREATE SERVER lw_delete_admin FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER lw_delete_admin;

CALL clickhouse_perform('lw_delete_admin', 'DROP DATABASE IF EXISTS lw_delete_test');
CALL clickhouse_perform('lw_delete_admin', 'CREATE DATABASE lw_delete_test');
CALL clickhouse_perform('lw_delete_admin', $$
    CREATE TABLE lw_delete_test.items (
        id Int32, name String, qty Int32, note Nullable(String)
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('lw_delete_admin', $$
    INSERT INTO lw_delete_test.items
    SELECT number, 'item' || toString(number), number * 10, if(number % 3 = 0, NULL, 'n')
    FROM numbers(1, 12)
$$);
CALL clickhouse_perform('lw_delete_admin', $$
    CREATE TABLE lw_delete_test.events (id Int32, ts Date) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('lw_delete_admin', $$
    INSERT INTO lw_delete_test.events VALUES (1, '2023-01-15'), (2, '2023-06-01')
$$);

CREATE SCHEMA lw_delete;
SET search_path = lw_delete, public;
CREATE FOREIGN TABLE items (id int, name text, qty int, note text)
    SERVER lw_delete_svr;
CREATE FOREIGN TABLE items_bin (id int, name text, qty int, note text)
    SERVER lw_delete_bin OPTIONS (table_name 'items');
CREATE FOREIGN TABLE items_renamed (key int OPTIONS (column_name 'id'), name text)
    SERVER lw_delete_svr OPTIONS (table_name 'items');

-- Run DELETE as one ClickHouse statement
EXPLAIN (VERBOSE, COSTS OFF) DELETE FROM items WHERE id = 1;
DELETE FROM items WHERE id = 1;
SELECT id FROM items ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
DELETE FROM items WHERE qty > 100 AND note IS NULL;
DELETE FROM items WHERE qty > 100 AND note IS NULL;
SELECT id FROM items ORDER BY id;

-- Binary driver
DELETE FROM items_bin WHERE name = 'item2';
SELECT id FROM items_bin ORDER BY id;

-- column_name option
EXPLAIN (VERBOSE, COSTS OFF) DELETE FROM items_renamed WHERE key = 3;
DELETE FROM items_renamed WHERE key = 3;
SELECT key FROM items_renamed ORDER BY key;

-- Send prepared statement argument as ClickHouse query parameter
SET plan_cache_mode = force_generic_plan;
PREPARE del(int) AS DELETE FROM items WHERE id = $1;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE del(4);
EXECUTE del(4);
PREPARE del_bin(int) AS DELETE FROM items_bin WHERE id = $1;
EXECUTE del_bin(5);
RESET plan_cache_mode;
SELECT id FROM items ORDER BY id;

-- Run subquery in Postgres and send result as query parameter
CREATE TABLE local_ids (id int);
INSERT INTO local_ids VALUES (6);
DELETE FROM items WHERE id = (SELECT max(id) FROM local_ids);
SELECT id FROM items ORDER BY id;

-- Delete from local and ClickHouse partitions in same statement
CREATE TABLE events (id int, ts date) PARTITION BY RANGE (ts);
CREATE FOREIGN TABLE events_cold PARTITION OF events
    FOR VALUES FROM ('2023-01-01') TO ('2024-01-01')
    SERVER lw_delete_svr OPTIONS (table_name 'events');
CREATE TABLE events_hot PARTITION OF events
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
INSERT INTO events_hot VALUES (3, '2024-01-10'), (4, '2024-02-15');
EXPLAIN (VERBOSE, COSTS OFF) DELETE FROM events WHERE id IN (2, 4);
DELETE FROM events WHERE id IN (2, 4);
SELECT id, ts FROM events ORDER BY id;

-- Reject statements that cannot run entirely in ClickHouse
DELETE FROM items WHERE id = 7 RETURNING id;
CREATE FUNCTION local_even(int) RETURNS bool LANGUAGE plpgsql AS 'BEGIN RETURN $1 % 2 = 0; END';
DELETE FROM items WHERE local_even(id);
DELETE FROM items USING local_ids WHERE items.id = local_ids.id;
DELETE FROM items WHERE id IN (SELECT id FROM local_ids);
CREATE FUNCTION no_op() RETURNS trigger LANGUAGE plpgsql AS 'BEGIN RETURN OLD; END';
CREATE TRIGGER items_del BEFORE DELETE ON items FOR EACH ROW EXECUTE FUNCTION no_op();
DELETE FROM items WHERE id = 7;
DROP TRIGGER items_del ON items;
SELECT id FROM items ORDER BY id;

-- ClickHouse requires WHERE
EXPLAIN (VERBOSE, COSTS OFF) DELETE FROM items;
DELETE FROM items;
SELECT count(*) FROM items;

-- Support ReplacingMergeTree and CollapsingMergeTree.
CALL clickhouse_perform('lw_delete_admin', $$
    CREATE TABLE lw_delete_test.replacing (id Int32, name String)
    ENGINE = ReplacingMergeTree ORDER BY id
$$);
CALL clickhouse_perform('lw_delete_admin', $$
    INSERT INTO lw_delete_test.replacing VALUES (1, 'Sushi'), (2, 'Bagel')
$$);
CALL clickhouse_perform('lw_delete_admin', $$
    CREATE TABLE lw_delete_test.collapsing (id Int32, name String, sign Int8)
    ENGINE = CollapsingMergeTree(sign) ORDER BY id
$$);
CALL clickhouse_perform('lw_delete_admin', $$
    INSERT INTO lw_delete_test.collapsing VALUES (1, 'Little Bit', 1), (2, 'Biscuit', 1)
$$);
CREATE FOREIGN TABLE replacing (id int, name text)
    SERVER lw_delete_bin OPTIONS (table_name 'replacing');
CREATE FOREIGN TABLE collapsing (id int, name text)
    SERVER lw_delete_bin OPTIONS (table_name 'collapsing');

DELETE FROM replacing WHERE id = 1;
SELECT * FROM replacing;
DELETE FROM collapsing WHERE id = 1;
SELECT * FROM collapsing;

CALL clickhouse_perform('lw_delete_admin', 'DROP DATABASE lw_delete_test');
SET client_min_messages = warning;
DROP SCHEMA lw_delete CASCADE;
DROP SERVER lw_delete_svr CASCADE;
DROP SERVER lw_delete_bin CASCADE;
DROP SERVER lw_delete_admin CASCADE;
