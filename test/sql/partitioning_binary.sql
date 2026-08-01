-- Binary-driver counterpart of partitioning.sql, exercising binary decode of
-- the int8[]/float8[] partial transition states pushed to ClickHouse. Uses
-- distinct object names so it can run alongside partitioning.sql.
SET datestyle = 'ISO';
SET max_parallel_workers_per_gather = 0;

CREATE SERVER pwagg_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'pwagg_bin_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER pwagg_bin_svr;

-- ClickHouse holds cold data
CREATE SERVER pwagg_bin_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER pwagg_bin_admin;

CALL clickhouse_perform('pwagg_bin_admin', 'DROP DATABASE IF EXISTS pwagg_bin_test');
CALL clickhouse_perform('pwagg_bin_admin', 'CREATE DATABASE pwagg_bin_test');
CALL clickhouse_perform('pwagg_bin_admin', 'CREATE TABLE pwagg_bin_test.events (id Int32, ts Date, val Int32, amt Float64) ENGINE = MergeTree ORDER BY ts');
CALL clickhouse_perform('pwagg_bin_admin', $$INSERT INTO pwagg_bin_test.events VALUES (1,'2023-01-15',10,10),(2,'2023-02-10',20,20),(3,'2023-03-20',30,30),(4,'2023-04-05',40,40)$$);

-- Partitioned table whose cold 2023 range lives on ClickHouse as a foreign
-- partition while the hot 2024 range stays local: the layout a consumer builds
-- when offloading old partitions (offload itself is left to the consumer)
CREATE TABLE events_bin (id int, ts date, val int, amt float8) PARTITION BY RANGE (ts);
CREATE FOREIGN TABLE events_bin_cold PARTITION OF events_bin
    FOR VALUES FROM ('2023-01-01') TO ('2024-01-01')
    SERVER pwagg_bin_svr OPTIONS (table_name 'events');
CREATE TABLE events_bin_hot PARTITION OF events_bin
    FOR VALUES FROM ('2024-01-01') TO ('2025-01-01');
INSERT INTO events_bin_hot VALUES (100,'2024-01-10',5,5), (101,'2024-02-15',15,15);

SET enable_partitionwise_aggregate = on;

-- Decomposable aggregates push the cold partial straight to ClickHouse
EXPLAIN (VERBOSE, COSTS OFF) SELECT count(*), sum(val), min(ts), max(ts) FROM events_bin;
SELECT count(*), sum(val), min(ts), max(ts) FROM events_bin;

-- avg(int) pushes its transition state as int8[2] {count, sum}
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(val) FROM events_bin;
SELECT avg(val) FROM events_bin;

-- avg/var/stddev over float push float8[3] {N, sum, sum of squared deviations}
EXPLAIN (VERBOSE, COSTS OFF) SELECT var_samp(amt) FROM events_bin;
SELECT avg(amt), var_pop(amt), var_samp(amt), stddev_pop(amt), stddev_samp(amt) FROM events_bin;

-- var_samp(int) keeps an INTERNAL numeric state, so it falls back to fetching
-- the cold rows and aggregating locally
EXPLAIN (VERBOSE, COSTS OFF) SELECT var_samp(val) FROM events_bin;
SELECT var_samp(val) FROM events_bin;

-- FILTER pushes too, as ClickHouse -If on each transition-state component
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(val) FILTER (WHERE val > 15) FROM events_bin;
SELECT avg(val) FILTER (WHERE val > 15),
       avg(amt) FILTER (WHERE amt > 15),
       var_samp(amt) FILTER (WHERE amt > 15) FROM events_bin;

RESET enable_partitionwise_aggregate;

DROP TABLE events_bin;
DROP USER MAPPING FOR CURRENT_USER SERVER pwagg_bin_svr;
CALL clickhouse_perform('pwagg_bin_admin', 'DROP DATABASE pwagg_bin_test');
DROP SERVER pwagg_bin_svr CASCADE;
