-- Regression test for crashes/hangs when one query runs more than one
-- binary-driver foreign scan at once (correlated subquery, nested-loop join).
-- Previously such scans collided on a single shared connection (crash), and a
-- rescan reused a freed batch context (hang). See PR #296.
CREATE SERVER concur_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'concur_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER concur_loopback;

CREATE SERVER concur_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER concur_admin;

CALL clickhouse_perform('concur_admin', 'DROP DATABASE IF EXISTS concur_test');
CALL clickhouse_perform('concur_admin', 'CREATE DATABASE concur_test');
CALL clickhouse_perform('concur_admin', 'CREATE TABLE concur_test.sales (
    sale_id Int32, item_id Int32, amount Decimal(15, 2)
) ENGINE = MergeTree ORDER BY sale_id;');
CALL clickhouse_perform('concur_admin', $$INSERT INTO concur_test.sales VALUES
    (1, 1, 100), (2, 1, 150), (3, 2, 200), (8, 1, 250)$$);

CREATE SCHEMA concur;
IMPORT FOREIGN SCHEMA concur_test FROM SERVER concur_loopback INTO concur;

-- Correlated subquery: the inner foreign scan runs while the outer cursor is
-- still open.
SELECT s.sale_id
    FROM concur.sales s
    WHERE s.amount > (
        SELECT avg(s2.amount) FROM concur.sales s2 WHERE s2.item_id = s.item_id
    )
    ORDER BY s.sale_id;

-- Nested-loop join over the same foreign table rescans the inner side per row.
SET enable_hashjoin = off;
SET enable_mergejoin = off;
SELECT a.sale_id, b.sale_id
    FROM concur.sales a
    JOIN concur.sales b ON a.item_id = b.item_id
    ORDER BY a.sale_id, b.sale_id;
RESET enable_hashjoin;
RESET enable_mergejoin;

-- LIMIT exercises the binary driver's premature-close path.
SELECT sale_id FROM concur.sales ORDER BY sale_id LIMIT 2;

-- Changing the server invalidates its connection, which a scan still reading
-- must keep until it ends. The scan spans several blocks.
CALL clickhouse_perform('concur_admin', 'CREATE TABLE concur_test.big (n Int32)
    ENGINE = MergeTree ORDER BY n');
CALL clickhouse_perform('concur_admin', 'INSERT INTO concur_test.big SELECT number FROM numbers(300000)');
IMPORT FOREIGN SCHEMA concur_test LIMIT TO (big) FROM SERVER concur_loopback INTO concur;
BEGIN;
DECLARE big_scan CURSOR FOR SELECT n FROM concur.big ORDER BY n;
FETCH 2 FROM big_scan;
ALTER SERVER concur_loopback OPTIONS (SET dbname 'concur_test');
SELECT count(*) FROM concur.sales;
MOVE FORWARD ALL IN big_scan;
COMMIT;

-- A server error in the middle of a scan, caught by a PL/pgSQL block, leaves
-- the abandoned scan's response to be released when the block's portal ends.
-- Reconnect first, since a double free here crashed only in a new session.
CALL clickhouse_perform('concur_admin', 'CREATE VIEW concur_test.big_throws AS
    SELECT n, throwIf(n = 250000) AS t FROM concur_test.big');
IMPORT FOREIGN SCHEMA concur_test LIMIT TO (big_throws) FROM SERVER concur_loopback INTO concur;
\c
DO $$
DECLARE c bigint;
BEGIN
    BEGIN
        SELECT sum(t) INTO c FROM concur.big_throws;
    EXCEPTION WHEN OTHERS THEN
        RAISE NOTICE 'caught';
    END;
END $$;
DO $$
DECLARE c bigint;
BEGIN
    SELECT n INTO c FROM concur.big_throws WHERE n > 100000
        ORDER BY n LIMIT 150000 OFFSET 100100;
END $$;
SELECT count(*) FROM concur.sales;

DROP FOREIGN TABLE concur.sales, concur.big, concur.big_throws;
DROP USER MAPPING FOR CURRENT_USER SERVER concur_loopback;
DROP SERVER concur_loopback;
DROP SCHEMA concur;
CALL clickhouse_perform('concur_admin', 'DROP DATABASE concur_test');
