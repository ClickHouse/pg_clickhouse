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
    c1 Int32, c2 Date, c3 DateTime, c4 String, c5 FixedString(10), c6 LowCardinality(String), c7 Date32, c8 DateTime64(3)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.arrays (
    c1 Int32, c2 Array(Int32), c3 Array(String)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.nested_arrays (
    c1 Int32, c2 Array(Array(Int32)), c3 Array(Array(String))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.addr (
    c1 UUID, c2 IPv4, c3 IPv6
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.bytes (
	c1 Int8, c2 String, c3 FixedString(16)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);');

SELECT clickhouse_raw_query('CREATE TABLE binary_inserts_test.not_nullable (
	c1 Int8, c2 Nullable(Enum(''x''=1)), c3 Nullable(Enum16(''x''=1)), c4 LowCardinality(Nullable(String))
) ENGINE = MergeTree ORDER BY (c1);');

CREATE SCHEMA binary_inserts_test;
IMPORT FOREIGN SCHEMA binary_inserts_test FROM SERVER binary_inserts_loopback INTO binary_inserts_test;
SET search_path = binary_inserts_test, public;

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
ALTER TABLE complex ALTER COLUMN c8 SET DATA TYPE timestamp(3);
\d+ complex
INSERT INTO complex VALUES
	(1, '2020-06-01', '2020-06-02 10:01:02', 't1', 'fix_t1', 'low1', '2020-06-01', '2020-06-02 10:01:02.123'),
	(2, '2020-06-02', '2020-06-03 10:01:02', 5, 'fix_t2', 'low2', '2020-06-02', '2020-06-03 11:01:02.234'),
	(3, '2020-06-03', '2020-06-04 10:01:02', 5, 'fix_t3', 'low3', '2020-06-03', '2020-06-04 12:01:02');
	
SET session timezone = 'UTC';
INSERT INTO complex VALUES
	(4, '1970-01-01', '1970-01-01 00:00:00', 5, 'fix_t4', 'low4', '1970-01-01', '1970-01-01 00:00:00'),
	(5, '2000-01-01', '2000-01-01 00:00:00', 5, 'fix_t5', 'low5', '2000-01-01', '2000-01-01 00:00:00');
SELECT * FROM complex ORDER BY c1;

RESET timezone;
SELECT * FROM complex ORDER BY c1;

/* check arrays */
INSERT INTO arrays VALUES
	(1, ARRAY[1,2], ARRAY['x']),
	(2, ARRAY[3,4,5], ARRAY['😀', '🦁']),
	(3, ARRAY[6,4], ARRAY['Zippy', 'Lisa "Skippy" Taylor']),
	(4, '{}'::int[], '{"[bracket''d]"}'::text[]),
	(5, ARRAY[0], ARRAY['[]']),
	(5, ARRAY[0], ARRAY[E'\\\b\f\r\n\t\a\'']);
SELECT * FROM arrays ORDER BY c1;

/* check nested arrays: postgres multi-dim arrays must be hyper-rectangular,
 * which maps cleanly to ClickHouse Array(Array(...)). */
INSERT INTO nested_arrays VALUES
	(1, ARRAY[[1,2],[3,4]], ARRAY[['a','b'],['c','d']]),
	(2, ARRAY[[5,6],[7,8],[9,10]], ARRAY[['e','f'],['g','h'],['i','j']]),
	(3, '{}'::int[], '{}'::text[]);
SELECT * FROM nested_arrays ORDER BY c1;

/* shape mismatch with column type must error rather than silently corrupt */
INSERT INTO nested_arrays VALUES (4, ARRAY[1,2,3], ARRAY['x']);

/* Check UUIDs and IPs */
\d addr
INSERT INTO addr VALUES
	('61f0c404-5cb3-11e7-907b-a6006ad3dba0', '116.106.34.242', '2001:44c8:129:2632:33:0:252:2'),
	('00000000-0000-0000-0000-000000000000', '127.0.0.1',      '::ffff:127.0.0.1'),
	('C62848ED-7316-4D15-92F3-9BB71EB69640', '183.247.232.58', '2a02:e980:1e::1')
;
SELECT * FROM addr ORDER BY c1;

/* Check BYTEA */
CREATE FOREIGN TABLE bbytes(
    c1 int,
    c2 BYTEA,
    c3 BYTEA
) SERVER binary_inserts_loopback OPTIONS (table_name 'bytes');
INSERT INTO bbytes
SELECT n, sha224(bytea('val'||n)), decode(md5('int'||n), 'hex')
  FROM generate_series(1, 4) n;

-- Should have full binary values, including nul bytes, from BYTEA columns.
SELECT * FROM bbytes ORDER BY c1;

-- Nul bytes should truncate TEXT columns.
SELECT c1, encode(c2::bytea, 'hex'), encode(c3::bytea, 'hex') FROM bytes ORDER BY c1;

SELECT clickhouse_raw_query('TRUNCATE binary_inserts_test.bytes');

-- Should fail.
INSERT INTO bytes
SELECT n, sha224(bytea('val'||n)), decode(md5('int'||n), 'hex')
  FROM generate_series(1, 4) n;

-- Remove FixedString length.
ALTER FOREIGN TABLE bytes ALTER c3 TYPE TEXT;
SELECT clickhouse_raw_query('ALTER TABLE binary_inserts_test.bytes MODIFY COLUMN c3 String');

-- Should succeed.
INSERT INTO bytes
SELECT n, sha224(bytea('val'||n)), decode(md5('int'||n), 'hex')
  FROM generate_series(1, 4) n;

SELECT * FROM bbytes ORDER BY c1;
SELECT c1, encode(c2::bytea, 'hex'), encode(c3::bytea, 'hex') FROM bytes ORDER BY c1;

-- Test NULL values.
SELECT clickhouse_raw_query($$
	CREATE TABLE binary_inserts_test.null_vals (
		c1 UInt8,
		-- INT2OID
		c2 Nullable(UInt8), c3 Nullable(Int8), c4 Nullable(Int16),
		-- INT4OID
		c5 Nullable(Int32), c6 Nullable(UInt16),
		-- INT8OID
		c7 Nullable(Int64), c8 Nullable(UInt32), c9 Nullable(UInt64),
		-- FLOAT4OID, FLOAT8OID
		c10 Nullable(Float32), c11 Nullable(Float64),
		-- NUMERICOID
		c12 Nullable(Decimal128(1)), c13 Nullable(Decimal64(1)),
		c14 Nullable(Decimal32(1)), c15 Nullable(Decimal(1, 0)),
		-- TEXTOID
		c16 Nullable(String), c17 Nullable(FixedString(1)),
		c18 Nullable(Enum('x'=1)), c19 Nullable(Enum16('x'=1)),
		c20 LowCardinality(Nullable(String)),
		-- DATEOID
		c21 Nullable(Date), c22 Nullable(Date32),
		-- TIMESTAMPOID, TIMESTAMPTZOID
		c23 Nullable(DateTime), c24 Nullable(DateTime64),
		-- ANYARRAYOID
		-- c25 Array(Nullable(Int32)), -- pg_clickhouse: nested Nullable is not supported
		-- UUIDOID
		c26 Nullable(UUID),
		-- INETOID
		c27 Nullable(IPv4), c28 Nullable(IPv6)
	) ENGINE = MergeTree ORDER BY (c1)
$$);

IMPORT FOREIGN SCHEMA binary_inserts_test LIMIT TO (null_vals)
FROM SERVER binary_inserts_loopback INTO binary_inserts_test;

INSERT INTO null_vals VALUES(
	1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	   NULL, NULL, NULL, NULL, -- ARRAY[NULL]::int[],
	   NULL, NULL, NULL
);

SELECT * FROM null_vals;

-- Test default values.
SELECT clickhouse_raw_query($$
	CREATE TABLE binary_inserts_test.default_vals (
		c1 UInt8,
		-- INT2OID
		c2 UInt8, c3 Int8, c4 Int16,
		-- INT4OID
		c5 Int32, c6 UInt16,
		-- INT8OID
		c7 Int64, c8 UInt32, c9 UInt64,
		-- FLOAT4OID, FLOAT8OID
		c10 Float32, c11 Float64,
		-- NUMERICOID
		c12 Decimal128(1), c13 Decimal64(1),
		c14 Decimal32(1), c15 Decimal(1, 0),
		-- TEXTOID
		c16 String, c17 FixedString(1),
		c18 Enum('x'=1), c19 Enum16('x'=1),
		c20 LowCardinality(String),
		-- DATEOID
		c21 Date, c22 Date32,
		-- TIMESTAMPOID, TIMESTAMPTZOID
		c23 DateTime, c24 DateTime64,
		-- ANYARRAYOID
		c25 Array(Int32), -- pg_clickhouse: nested Nullable is not supported
		-- UUIDOID
		c26 UUID,
		-- INETOID
		c27 IPv4, c28 IPv6
	) ENGINE = MergeTree ORDER BY (c1)
$$);

IMPORT FOREIGN SCHEMA binary_inserts_test LIMIT TO (default_vals)
FROM SERVER binary_inserts_loopback INTO binary_inserts_test;

-- Fails on c2, the first column we try to set to NULL. Will cease to fail if
-- the `if (isnull && !nullable)` block is removed from column_append(). See
-- its comment for details.
INSERT INTO default_vals VALUES(
	1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
	   NULL, NULL, NULL, NULL, ARRAY[NULL]::int[],
	   NULL, NULL, NULL
);

SELECT * FROM default_vals;

-- LowCardinality(Nullable(String)) round-trips NULL and non-NULL.
INSERT INTO not_nullable VALUES (1, 'x', 'x', NULL), (2, 'x', 'x', 'lc');
SELECT * FROM not_nullable ORDER BY c1;

DROP USER MAPPING FOR CURRENT_USER SERVER binary_inserts_loopback;
SELECT clickhouse_raw_query('DROP DATABASE binary_inserts_test');
DROP SERVER binary_inserts_loopback CASCADE;
