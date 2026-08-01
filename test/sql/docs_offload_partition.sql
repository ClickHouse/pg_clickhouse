-- Exercise consumer-facing offload helpers documented in
-- doc/offload-partition.sql: stage a ClickHouse destination from a partitioned
-- table's shape, then cut a contiguous span of local RANGE partitions over to a
-- single foreign partition. Distinct names keep clear of partitioning_{http,
-- binary} sharing this database
SET datestyle = 'ISO';
SET max_parallel_workers_per_gather = 0;

CREATE SERVER offload_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'offload_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER offload_svr;

CREATE SERVER offload_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER offload_admin;

CALL clickhouse_perform('offload_admin', 'DROP DATABASE IF EXISTS offload_test');
CALL clickhouse_perform('offload_admin', 'CREATE DATABASE offload_test');

-- Load documented helpers without echoing their bodies; round-trips below guard
-- against drift, a parse error still surfaces
\set ECHO none
\i doc/offload-partition.sql
\set ECHO all

-- All-local partitioned table: three contiguous monthly 2023 partitions to
-- offload, plus a 2024 partition kept local
CREATE TABLE offload_events (id int, ts date, val int, amt float8) PARTITION BY RANGE (ts);
CREATE TABLE offload_events_jan PARTITION OF offload_events FOR VALUES FROM ('2023-01-01') TO ('2023-02-01');
CREATE TABLE offload_events_feb PARTITION OF offload_events FOR VALUES FROM ('2023-02-01') TO ('2023-03-01');
CREATE TABLE offload_events_mar PARTITION OF offload_events FOR VALUES FROM ('2023-03-01') TO ('2023-04-01');
CREATE TABLE offload_events_2024 PARTITION OF offload_events FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
INSERT INTO offload_events VALUES
    (1, '2023-01-15', 10, 1.5), (2, '2023-02-10', 20, 2.5), (3, '2023-03-20', 30, 3.5),
    (100, '2024-01-10', 5, 4.5), (101, '2024-02-15', 15, 5.5);

-- Stage ClickHouse destination mirroring parent's columns; nullable columns
-- wrap in Nullable(), non-null RANGE key stays bare. Returns DDL run
SELECT clickhouse_offload_create_table('offload_events', 'offload_svr');

-- Cut the three 2023 partitions over to one foreign partition; returns local
-- row count moved
SELECT clickhouse_offload_range('offload_events',
    ARRAY['offload_events_jan', 'offload_events_feb', 'offload_events_mar']::regclass[],
    'offload_svr');

-- Offloaded locals gone; single foreign partition (relkind f) now covers 2023
-- beside the retained 2024 partition
SELECT c.relname, c.relkind
  FROM pg_inherits i JOIN pg_class c ON c.oid = i.inhrelid
 WHERE i.inhparent = 'offload_events'::regclass
 ORDER BY c.relname;

-- Rows survive cutover, foreign 2023 partition merging with local 2024
SELECT * FROM offload_events ORDER BY id;
SELECT count(*), sum(val), min(ts), max(ts) FROM offload_events;

-- Filter to 2023 prunes the local 2024 partition, pushes to ClickHouse
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, ts, val FROM offload_events WHERE ts < DATE '2024-01-01' ORDER BY id;
SELECT id, ts, val FROM offload_events WHERE ts < DATE '2024-01-01' ORDER BY id;

-- Filter to 2024 prunes the foreign 2023 partition, stays local
SELECT id, ts, val FROM offload_events WHERE ts >= DATE '2024-01-01' ORDER BY id;

-- Non-key predicate spans both: pushed to ClickHouse for the foreign 2023
-- partition, filtered locally for 2024
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, ts, val FROM offload_events WHERE val >= 15 ORDER BY id;
SELECT id, ts, val FROM offload_events WHERE val >= 15 ORDER BY id;

-- Guard rails ----------------------------------------------------------------
-- Assert error text, not plpgsql body line numbers that shift as the doc edits
\set VERBOSITY terse

-- create_table rejects unknown server before touching ClickHouse
SELECT clickhouse_offload_create_table('offload_events', 'no_such_svr');

-- create_table rejects non-partitioned table
CREATE TABLE offload_plain (id int, ts date);
SELECT clickhouse_offload_create_table('offload_plain', 'offload_svr');

-- create_table rejects column with no ClickHouse mapping
CREATE TABLE offload_badcol (ts date, addr inet) PARTITION BY RANGE (ts);
SELECT clickhouse_offload_create_table('offload_badcol', 'offload_svr');

-- Both helpers support single-column RANGE only
CREATE TABLE offload_bylist (id int, region text) PARTITION BY LIST (region);
SELECT clickhouse_offload_create_table('offload_bylist', 'offload_svr');
SELECT clickhouse_offload_range('offload_bylist', ARRAY[]::regclass[], 'offload_svr');

-- offload_range rejects a table that is not a partition of parent
CREATE TABLE offload_stray (id int, ts date, val int, amt float8);
SELECT clickhouse_offload_range('offload_events',
    ARRAY['offload_stray']::regclass[], 'offload_svr');

-- offload_range rejects a non-contiguous span (Feb missing leaves a gap)
CREATE TABLE offload_gap (id int, ts date) PARTITION BY RANGE (ts);
CREATE TABLE offload_gap_jan PARTITION OF offload_gap FOR VALUES FROM ('2023-01-01') TO ('2023-02-01');
CREATE TABLE offload_gap_mar PARTITION OF offload_gap FOR VALUES FROM ('2023-03-01') TO ('2023-04-01');
SELECT clickhouse_offload_range('offload_gap',
    ARRAY['offload_gap_jan', 'offload_gap_mar']::regclass[], 'offload_svr');

DROP TABLE offload_events, offload_plain, offload_badcol, offload_bylist,
           offload_stray, offload_gap;
DROP FUNCTION clickhouse_offload_range(regclass, regclass[], name, text, text, name);
DROP FUNCTION clickhouse_offload_create_table(regclass, name, text, text, text);
DROP USER MAPPING FOR CURRENT_USER SERVER offload_svr;
CALL clickhouse_perform('offload_admin', 'DROP DATABASE offload_test');
DROP SERVER offload_svr CASCADE;
