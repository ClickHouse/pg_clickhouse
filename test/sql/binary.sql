SET datestyle = 'ISO';
CREATE SERVER binary_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'binary_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS binary_test');
SELECT clickhouse_raw_query('CREATE DATABASE binary_test');

-- integer types
SELECT clickhouse_raw_query('CREATE TABLE binary_test.ints (
    c1 Int8, c2 Int16, c3 Int32, c4 Int64,
    c5 UInt8, c6 UInt16, c7 UInt32, c8 UInt64,
    c9 Float32, c10 Float64, c11 Bool
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.ints SELECT
    number, number + 1, number + 2, number + 3, number + 4, number + 5,
    number + 6, number + 7, number + 8.1, number + 9.2, cast(number % 2 as Bool)
    FROM numbers(10);');

-- date and string types
SELECT clickhouse_raw_query('CREATE TABLE binary_test.types (
    c1 Date, c2 DateTime, c3 String, c4 FixedString(5), c5 UUID,
    c6 Enum8(''one'' = 1, ''two'' = 2),
    c7 Enum16(''one'' = 1, ''two'' = 2, ''three'' = 3)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.types SELECT
    addDays(toDate(''1990-01-01''), number),
    addMinutes(addSeconds(addDays(toDateTime(''1990-01-01 10:00:00'', ''UTC''), number), number), number),
    format(''number {0}'', toString(number)),
    format(''num {0}'', toString(number)),
    format(''f4bf890f-f9dc-4332-ad5c-0c18e73f28e{0}'', toString(number)),
    ''two'',
    ''three''
    FROM numbers(10);');

-- array types
SELECT clickhouse_raw_query('CREATE TABLE binary_test.arrays (
    c1 Array(Int), c2 Array(String)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.arrays SELECT
    [number, number + 1],
    [format(''num{0}'', toString(number)), format(''num{0}'', toString(number + 1))]
    FROM numbers(10);');

-- nested arrays
SELECT clickhouse_raw_query('CREATE TABLE binary_test.nested_arrays (
    c1 Int8, c2 Array(Array(Int32)), c3 Array(Array(String))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.nested_arrays VALUES
    (1, [[1,2],[3,4]], [[''a'',''b''],[''c'',''d'']]),
    (2, [[5,6],[7,8]], [[''e'',''f''],[''g'',''h'']]);
');

-- ragged nested arrays must error: postgres requires hyper-rectangles
SELECT clickhouse_raw_query('CREATE TABLE binary_test.ragged_arrays (
    c1 Int8, c2 Array(Array(Int32))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.ragged_arrays VALUES (1, [[1,2,3],[4]]);');

SELECT clickhouse_raw_query('CREATE TABLE binary_test.tuples (
    c1 Int8,
    c2 Tuple(Int, String, Float32),
    c3 UInt8
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.tuples SELECT
    number,
    (number, toString(number), number + 1.0),
    number % 2
    FROM numbers(10);');

SELECT clickhouse_raw_query('CREATE TABLE binary_test.bytes (
    c1 Int8,
    c2 String,
    c3 FixedString(16)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO binary_test.bytes SELECT
    number,
    SHA1(''val'' || toString(number)),
    MD5(''val'' || toString(number))
    FROM numbers(10);');

CREATE FOREIGN TABLE fints (
	c1 int2,
	c2 int2,
	c3 int,
	c4 int8,
	c5 int2,
	c6 int,
	c7 int8,
	c8 int8,
    c9 float4,
    c10 float8,
    c11 bool
) SERVER binary_loopback OPTIONS (table_name 'ints');

CREATE FOREIGN TABLE ftypes (
	c1 date,
	c2 timestamp with time zone,
    c3 text,
    c4 text,
    c5 uuid,
    c6 text, -- Enum8
    c7 text  -- Enum16
) SERVER binary_loopback OPTIONS (table_name 'types');

CREATE FOREIGN TABLE farrays (
	c1 int[],
    c2 text[]
) SERVER binary_loopback OPTIONS (table_name 'arrays');

CREATE FOREIGN TABLE farrays2 (
	c1 int8[],
    c2 text[]
) SERVER binary_loopback OPTIONS (table_name 'arrays');

CREATE FOREIGN TABLE fnested_arrays (
    c1 int2,
    c2 int[],
    c3 text[]
) SERVER binary_loopback OPTIONS (table_name 'nested_arrays');

CREATE FOREIGN TABLE fragged_arrays (
    c1 int2,
    c2 int[]
) SERVER binary_loopback OPTIONS (table_name 'ragged_arrays');

CREATE TYPE tupformat AS (a int, b text, c float4);
CREATE FOREIGN TABLE ftuples (
    c1 int,
    c2 tupformat,
    c3 bool
) SERVER binary_loopback OPTIONS (table_name 'tuples');

CREATE FOREIGN TABLE fbytes(
    c1 int,
    c2 BYTEA,
    c3 BYTEA
) SERVER binary_loopback OPTIONS (table_name 'bytes');

COPY fints FROM stdin;
\.

-- integers
SELECT * FROM fints ORDER BY c1;
SELECT c2, c1, c8, c3, c4, c7, c6, c5 FROM fints ORDER BY c1;
SELECT a, b FROM (SELECT c1 * 10 as a, c8 * 11 as b FROM fints ORDER BY a LIMIT 2) t1;
SELECT NULL FROM fints LIMIT 2;
SELECT c2, NULL, c1, NULL FROM fints ORDER BY c2 LIMIT 2;

-- types
SELECT * FROM ftypes ORDER BY c1;
SELECT c2, c1, c4, c3, c5, c7, c6 FROM ftypes ORDER BY c1;

-- arrays
SELECT * FROM farrays ORDER BY c1;
SELECT * FROM farrays2 ORDER BY c1;

-- nested arrays
SELECT * FROM fnested_arrays ORDER BY c1;
SELECT * FROM fragged_arrays ORDER BY c1;

-- tuples
SELECT * FROM ftuples ORDER BY c1;

-- Bytes.
SELECT * FROM fbytes ORDER BY c1;

DROP USER MAPPING FOR CURRENT_USER SERVER binary_loopback;
SELECT clickhouse_raw_query('DROP DATABASE binary_test');

DROP SERVER binary_loopback CASCADE;
