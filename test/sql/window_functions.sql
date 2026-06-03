\unset ECHO
SET client_min_messages = notice;
SET datestyle = 'ISO';
SET session timezone = 'UTC';

-- Create servers for each engine.
CREATE SERVER wf_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER wf_bin_svr;

CREATE SERVER wf_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER wf_http_svr;

-- Create a ClickHouse table.
SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS wf_test');
SELECT clickhouse_raw_query('CREATE DATABASE wf_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE wf_test.events (
        id          UInt64,
        entity_id   String,
        event_name  String,
        ts_event    DateTime,
        amount      Int32
    ) ENGINE = MergeTree
    ORDER BY (event_name, ts_event)
$$);

SELECT clickhouse_raw_query($$
    INSERT INTO wf_test.events VALUES
    (1, 'lead_100', 'lead_created', '2026-03-01 10:00:00', 100),
    (2, 'lead_100', 'lead_created', '2026-03-15 14:00:00', 200),
    (3, 'lead_200', 'lead_created', '2026-03-10 09:00:00', 150),
    (4, 'lead_200', 'lead_created', '2026-03-20 11:00:00', 300),
    (5, 'lead_300', 'lead_created', '2026-03-05 08:00:00', 250),
    (6, 'lead_100', 'lead_updated', '2026-03-16 12:00:00', 210),
    (7, 'lead_200', 'lead_updated', '2026-03-21 13:00:00', 310),
    (8, 'lead_300', 'lead_updated', '2026-03-06 09:00:00', 260)
$$);

-- Create foreign tables via IMPORT FOREIGN SCHEMA.
CREATE SCHEMA wf_bin;
CREATE SCHEMA wf_http;
IMPORT FOREIGN SCHEMA "wf_test" FROM SERVER wf_bin_svr INTO wf_bin;
IMPORT FOREIGN SCHEMA "wf_test" FROM SERVER wf_http_svr INTO wf_http;

-- ============================================================
-- ROW_NUMBER() OVER (PARTITION BY ... ORDER BY ...)
-- ============================================================
\echo -- ROW_NUMBER pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created';

\echo -- ROW_NUMBER pushdown (http)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_http.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_http.events
WHERE event_name = 'lead_created';

-- ============================================================
-- MIN() OVER / MAX() OVER
-- ============================================================
\echo -- MIN/MAX OVER pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_bin.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_bin.events
WHERE event_name = 'lead_created';

\echo -- MIN/MAX OVER pushdown (http)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_http.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_http.events
WHERE event_name = 'lead_created';

-- ============================================================
-- LEAD() OVER
-- ============================================================
\echo -- LEAD pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event,
       lead(ts_event) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS next_event
FROM wf_bin.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event,
       lead(ts_event) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS next_event
FROM wf_bin.events
WHERE event_name = 'lead_created';

\echo -- LEAD pushdown (http)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event,
       lead(ts_event) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS next_event
FROM wf_http.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event,
       lead(ts_event) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS next_event
FROM wf_http.events
WHERE event_name = 'lead_created';

-- ============================================================
-- LAG() OVER
-- ============================================================
\echo -- LAG pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event,
       lag(amount) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS prev_amount
FROM wf_bin.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event,
       lag(amount) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS prev_amount
FROM wf_bin.events
WHERE event_name = 'lead_created';

-- ============================================================
-- RANK() and DENSE_RANK()
-- ============================================================
\echo -- rank pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, amount,
       rank() OVER (PARTITION BY entity_id ORDER BY amount) AS rk
FROM wf_bin.events
ORDER BY entity_id, amount;

SELECT entity_id, amount,
       rank() OVER (PARTITION BY entity_id ORDER BY amount) AS rk
FROM wf_bin.events
ORDER BY entity_id, amount;

\echo -- dense_rank pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, amount,
       dense_rank() OVER (PARTITION BY entity_id ORDER BY amount) AS dr
FROM wf_bin.events
ORDER BY entity_id, amount;

SELECT entity_id, amount,
       dense_rank() OVER (PARTITION BY entity_id ORDER BY amount) AS dr
FROM wf_bin.events
ORDER BY entity_id, amount;

-- ============================================================
-- Ranking window functions (ntile, cume_dist, percent_rank)
-- ============================================================
\echo -- ntile pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event,
       ntile(2) OVER (ORDER BY ts_event) AS bucket
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY ts_event;

SELECT entity_id, ts_event,
       ntile(2) OVER (ORDER BY ts_event) AS bucket
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY ts_event;

\echo -- cume_dist pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, amount,
       cume_dist() OVER (ORDER BY amount) AS cd
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY amount;

SELECT entity_id, amount,
       cume_dist() OVER (ORDER BY amount) AS cd
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY amount;

\echo -- percent_rank pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, amount,
       percent_rank() OVER (ORDER BY amount) AS pr
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY amount;

SELECT entity_id, amount,
       percent_rank() OVER (ORDER BY amount) AS pr
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY amount;

-- ============================================================
-- ORDER BY pushdown with window functions
-- ============================================================
\echo -- ROW_NUMBER + ORDER BY pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id;

SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id;

\echo -- ROW_NUMBER + ORDER BY pushdown (http)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_http.events
WHERE event_name = 'lead_created'
ORDER BY entity_id;

SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_http.events
WHERE event_name = 'lead_created'
ORDER BY entity_id;

\echo -- MIN/MAX OVER + ORDER BY pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id, ts_event;

SELECT entity_id, ts_event, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id, ts_event;

\echo -- Window + ORDER BY + LIMIT pushdown (binary)
EXPLAIN (COSTS OFF)
SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id, ts_event
LIMIT 3;

SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id, ts_event
LIMIT 3;

-- ============================================================
-- EXPLAIN VERBOSE with pushed-down window functions
-- Regression: deparse of WindowFunc in fdw_scan_tlist used to fail
-- with "could not find window clause for winref N".
-- ============================================================
\echo -- VERBOSE ROW_NUMBER pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created';

\echo -- VERBOSE ROW_NUMBER pushdown (http)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_http.events
WHERE event_name = 'lead_created';

\echo -- VERBOSE MIN/MAX OVER pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, amount,
       min(amount) OVER (PARTITION BY entity_id) AS min_amount,
       max(amount) OVER (PARTITION BY entity_id) AS max_amount
FROM wf_bin.events
WHERE event_name = 'lead_created';

\echo -- VERBOSE LEAD pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event,
       lead(ts_event) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS next_event
FROM wf_bin.events
WHERE event_name = 'lead_created';

-- ============================================================
-- LAG() OVER
-- ============================================================
\echo -- VERBOSE LAG pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event,
       lag(amount) OVER (PARTITION BY entity_id ORDER BY ts_event ASC) AS prev_amount
FROM wf_bin.events
WHERE event_name = 'lead_created';

-- ============================================================
-- RANK() and DENSE_RANK()
-- ============================================================
\echo -- VERBOSE rank pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, amount,
       rank() OVER (PARTITION BY entity_id ORDER BY amount) AS rk
FROM wf_bin.events
ORDER BY entity_id, amount;

\echo -- VERBOSE dense_rank pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, amount,
       dense_rank() OVER (PARTITION BY entity_id ORDER BY amount) AS dr
FROM wf_bin.events
ORDER BY entity_id, amount;

\echo -- VERBOSE ntile pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event,
       ntile(2) OVER (ORDER BY ts_event) AS bucket
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY ts_event;

\echo -- VERBOSE window + ORDER BY + LIMIT pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event, amount,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event DESC) AS rn
FROM wf_bin.events
WHERE event_name = 'lead_created'
ORDER BY entity_id, ts_event
LIMIT 3;

\echo -- VERBOSE duplicate window call (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT entity_id, ts_event,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event) AS rn1,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event) AS rn2
FROM wf_bin.events
WHERE event_name = 'lead_created';

SELECT entity_id, ts_event,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event) AS rn1,
       row_number() OVER (PARTITION BY entity_id ORDER BY ts_event) AS rn2
FROM wf_bin.events
WHERE event_name = 'lead_created';

-- Clean up.
SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS wf_test');
DROP USER MAPPING FOR CURRENT_USER SERVER wf_bin_svr;
DROP SERVER wf_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER wf_http_svr;
DROP SERVER wf_http_svr CASCADE;
