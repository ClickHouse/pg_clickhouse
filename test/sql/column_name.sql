-- Verify column_name FDW option is honored by INSERT on both engines.
-- Regression: SELECT primed the column-name cache via GetForeignRelSize, but
-- INSERT-only sessions never primed it, so deparse fell back to PG attnames
-- and the binary engine could not match the CH block's columns.

SET datestyle = 'ISO';
CREATE SERVER cn_http_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'cn_test', driver 'http');
CREATE SERVER cn_bin_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'cn_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER cn_http_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER cn_bin_loopback;

SELECT clickhouse_raw_query('drop database if exists cn_test');
SELECT clickhouse_raw_query('create database cn_test');
SELECT clickhouse_raw_query('CREATE TABLE cn_test.t_http (
    ch_id Int32, ch_val String, plain Nullable(Int32)
) ENGINE = MergeTree ORDER BY (ch_id);');
SELECT clickhouse_raw_query('CREATE TABLE cn_test.t_bin (
    ch_id Int32, ch_val String, plain Nullable(Int32)
) ENGINE = MergeTree ORDER BY (ch_id);');

CREATE FOREIGN TABLE cn_http (
    pg_id int OPTIONS (column_name 'ch_id'),
    pg_val text OPTIONS (column_name 'ch_val'),
    plain int
) SERVER cn_http_loopback OPTIONS (table_name 't_http');

CREATE FOREIGN TABLE cn_bin (
    pg_id int OPTIONS (column_name 'ch_id'),
    pg_val text OPTIONS (column_name 'ch_val'),
    plain int
) SERVER cn_bin_loopback OPTIONS (table_name 't_bin');

-- INSERT before any SELECT keeps the column-name cache cold and exercises
-- the lazy populate path inside chfdw_get_custom_column_info.
INSERT INTO cn_http VALUES (1, 'http one', 100);
INSERT INTO cn_bin  VALUES (2, 'bin one',  200);

-- Reordered column list exercises deparse_insert_sql and, for binary,
-- ch_binary_make_tuple_map's name matching against the CH block columns.
INSERT INTO cn_http (plain, pg_val, pg_id) VALUES (300, 'http reorder', 3);
INSERT INTO cn_bin  (plain, pg_val, pg_id) VALUES (400, 'bin reorder',  4);

-- Subset INSERT leaves plain unset.
INSERT INTO cn_http (pg_id, pg_val) VALUES (5, 'http subset');
INSERT INTO cn_bin  (pg_id, pg_val) VALUES (6, 'bin subset');

SELECT * FROM cn_http ORDER BY pg_id;
SELECT * FROM cn_bin  ORDER BY pg_id;

-- Dropped attnum: forces lazy populator to skip an attisdropped slot when
-- materializing the cache entry on first INSERT.
SELECT clickhouse_raw_query('CREATE TABLE cn_test.td (
    ch_id Int32, ch_val String
) ENGINE = MergeTree ORDER BY (ch_id);');

CREATE FOREIGN TABLE cn_drop (
    discarded int,
    pg_id int OPTIONS (column_name 'ch_id'),
    pg_val text OPTIONS (column_name 'ch_val')
) SERVER cn_bin_loopback OPTIONS (table_name 'td');
ALTER FOREIGN TABLE cn_drop DROP COLUMN discarded;

INSERT INTO cn_drop VALUES (7, 'after drop');
SELECT * FROM cn_drop ORDER BY pg_id;

-- Drop column after the cache is primed by an INSERT and SELECT, to confirm
-- the ATTNUM syscache invalidation purges stale CustomColumnInfo entries.
SELECT clickhouse_raw_query('CREATE TABLE cn_test.tlate (
    discarded Nullable(Int32), ch_id Int32, ch_val String
) ENGINE = MergeTree ORDER BY (ch_id);');

CREATE FOREIGN TABLE cn_drop_late (
    discarded int,
    pg_id int OPTIONS (column_name 'ch_id'),
    pg_val text OPTIONS (column_name 'ch_val')
) SERVER cn_bin_loopback OPTIONS (table_name 'tlate');

INSERT INTO cn_drop_late VALUES (800, 8, 'before drop');
SELECT * FROM cn_drop_late ORDER BY pg_id;

ALTER FOREIGN TABLE cn_drop_late DROP COLUMN discarded;

INSERT INTO cn_drop_late VALUES (9, 'after drop');
SELECT * FROM cn_drop_late ORDER BY pg_id;

DROP USER MAPPING FOR CURRENT_USER SERVER cn_http_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER cn_bin_loopback;
SELECT clickhouse_raw_query('DROP DATABASE cn_test');
DROP SERVER cn_http_loopback CASCADE;
DROP SERVER cn_bin_loopback CASCADE;
