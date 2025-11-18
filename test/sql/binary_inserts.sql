SET datestyle = 'ISO';
CREATE SERVER binary_inserts_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'binary_inserts_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_inserts_loopback;

SELECT clickhouse_raw_query('drop database if exists binary_inserts_test');
SELECT clickhouse_raw_query('create database binary_inserts_test');
SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.ints (
    c1 Int8, c2 Int16, c3 Int32, c4 Int64
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.uints (
    c1 UInt8, c2 UInt16, c3 UInt32, c4 UInt64, c5 Bool
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.floats (
    c1 Float32, c2 Float64
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1) SETTINGS allow_floating_point_partition_key=1;
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.null_ints (
    c1 Int8, c2 Nullable(Int32)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.complex (
    c1 Int32, c2 Date, c3 DateTime, c4 String, c5 FixedString(10), c6 LowCardinality(String), c7 DateTime64(3)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.arrays (
    c1 Int32, c2 Array(Int32)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

IMPORT FOREIGN SCHEMA "binary_inserts_test" FROM SERVER binary_inserts_loopback INTO public;

/* ints */
INSERT INTO ints
	SELECT i, i + 1, i + 2, i+ 3 FROM generate_series(1, 3) i;
SELECT * FROM ints ORDER BY c1;
INSERT INTO ints (c1, c4, c3, c2)
	SELECT i, i + 1, i + 2, i+ 3 FROM generate_series(4, 6) i;
SELECT * FROM ints ORDER BY c1;

/* check dropping columns (that will change attnums) */
ALTER TABLE ints DROP COLUMN c1;
ALTER TABLE ints ADD COLUMN c1 SMALLINT;
INSERT INTO ints (c1, c2, c3, c4)
	SELECT i, i + 1, i + 2, i+ 3 FROM generate_series(7, 8) i;
SELECT c1, c2, c3, c4 FROM ints ORDER BY c1;

/* check other number types */
INSERT INTO uints
	SELECT i, i + 1, i + 2, i+ 3, (i % 2)::bool FROM generate_series(1, 3) i;
SELECT * FROM uints ORDER BY c1;
INSERT INTO floats
	SELECT i * 1.1, i + 2.1 FROM generate_series(1, 3) i;
SELECT * FROM floats ORDER BY c1;

/* check nullable */
INSERT INTO null_ints SELECT i, case WHEN i % 2 = 0 THEN NULL ELSE i END FROM generate_series(1, 10) i;
INSERT INTO null_ints(c1) SELECT i FROM generate_series(11, 13) i;
SELECT * FROM null_ints ORDER BY c1;
SELECT * FROM null_ints ORDER BY c1;

/* check dates and strings */
ALTER TABLE complex ALTER COLUMN c7 SET DATA TYPE timestamp(3);
\d+ complex
INSERT INTO complex VALUES
	(1, '2020-06-01', '2020-06-02 10:01:02', 't1', 'fix_t1', 'low1', '2020-06-02 10:01:02.123'),
	(2, '2020-06-02', '2020-06-03 10:01:02', 5, 'fix_t2', 'low2', '2020-06-03 11:01:02.234'),
	(3, '2020-06-03', '2020-06-04 10:01:02', 5, 'fix_t3', 'low3', '2020-06-04 12:01:02');
SELECT * FROM complex ORDER BY c1;

/* check arrays */
INSERT INTO arrays VALUES
	(1, ARRAY[1,2]),
	(2, ARRAY[3,4,5]),
	(3, ARRAY[6,4]);
SELECT * FROM arrays ORDER BY c1;

DROP USER MAPPING FOR CURRENT_USER SERVER binary_inserts_loopback;
SELECT clickhouse_raw_query('DROP DATABASE binary_inserts_test');
DROP SERVER binary_inserts_loopback CASCADE;
