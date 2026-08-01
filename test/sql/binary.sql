SET datestyle = 'ISO';
CREATE SERVER binary_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'binary_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_loopback;

CREATE SERVER binary_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_admin;

CALL clickhouse_perform('binary_admin', 'DROP DATABASE IF EXISTS binary_test');
CALL clickhouse_perform('binary_admin', 'CREATE DATABASE binary_test');

-- integer types
CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.ints (
    c1 Int8, c2 Int16, c3 Int32, c4 Int64,
    c5 UInt8, c6 UInt16, c7 UInt32, c8 UInt64,
    c9 Float32, c10 Float64, c11 Bool
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.ints SELECT
    number, number + 1, number + 2, number + 3, number + 4, number + 5,
    number + 6, number + 7, number + 8.1, number + 9.2, cast(number % 2 as Bool)
    FROM numbers(10);');

-- date and string types
CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.types (
    c1 Date, c2 DateTime, c3 String, c4 FixedString(5), c5 UUID,
    c6 Enum8(''one'' = 1, ''two'' = 2),
    c7 Enum16(''one'' = 1, ''two'' = 2, ''three'' = 3)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.types SELECT
    addDays(toDate(''1990-01-01''), number),
    addMinutes(addSeconds(addDays(toDateTime(''1990-01-01 10:00:00'', ''UTC''), number), number), number),
    format(''number {0}'', toString(number)),
    format(''num {0}'', toString(number)),
    format(''f4bf890f-f9dc-4332-ad5c-0c18e73f28e{0}'', toString(number)),
    ''two'',
    ''three''
    FROM numbers(10);');

-- array types
CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.arrays (
    c1 Array(Int), c2 Array(String)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.arrays SELECT
    [number, number + 1],
    [format(''num{0}'', toString(number)), format(''num{0}'', toString(number + 1))]
    FROM numbers(10);');

-- nested arrays
CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.nested_arrays (
    c1 Int8, c2 Array(Array(Int32)), c3 Array(Array(String))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.nested_arrays VALUES
    (1, [[1,2],[3,4]], [[''a'',''b''],[''c'',''d'']]),
    (2, [[5,6],[7,8]], [[''e'',''f''],[''g'',''h'']]);
');

-- ragged nested arrays must error: postgres requires hyper-rectangles
CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.ragged_arrays (
    c1 Int8, c2 Array(Array(Int32))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.ragged_arrays VALUES (1, [[1,2,3],[4]]);');

CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.tuples (
    c1 Int8,
    c2 Tuple(Int, String, Float32),
    c3 UInt8
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.tuples SELECT
    number,
    (number, toString(number), number + 1.0),
    number % 2
    FROM numbers(10);');

CALL clickhouse_perform('binary_admin', 'CREATE TABLE binary_test.bytes (
    c1 Int8,
    c2 String,
    c3 FixedString(16)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
CALL clickhouse_perform('binary_admin', 'INSERT INTO binary_test.bytes SELECT
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
EXPLAIN (VERBOSE, COSTS OFF)
SELECT c1 FROM fbytes
WHERE c3 IN (
    decode('8de92ce2033cf3ca03fa8cc63e7a703f', 'hex'),
    decode('9163c8c66d03c512404cca8549a250e7', 'hex')
)
ORDER BY c1;
SELECT c1 FROM fbytes
WHERE c3 IN (
    decode('8de92ce2033cf3ca03fa8cc63e7a703f', 'hex'),
    decode('9163c8c66d03c512404cca8549a250e7', 'hex')
)
ORDER BY c1;

-- unknown driver is rejected
CREATE SERVER binary_bogus FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(driver 'bogus');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_bogus;
SELECT * FROM clickhouse_query('binary_bogus', 'SELECT 1') AS t(x int);

-- clickhouse_query: server-based typed rowset over the binary driver
SELECT * FROM clickhouse_query(
    'binary_loopback', 'SELECT c1, c3 FROM ints ORDER BY c1 LIMIT 3'
) AS t(c1 int2, c3 int);
SELECT * FROM clickhouse_query(
    'binary_loopback', 'SELECT toInt32(number) AS n, toString(number) AS s FROM numbers(3) ORDER BY n'
) AS t(n int, s text);
-- empty result yields no rows
SELECT count(*) FROM clickhouse_query('binary_loopback', 'SELECT 1 WHERE 0') AS t(x int);
-- missing column definition list is rejected
SELECT * FROM clickhouse_query('binary_loopback', 'SELECT 1');
-- fewer columns declared than returned is rejected
SELECT * FROM clickhouse_query('binary_loopback', 'SELECT 1, 2') AS t(x int);
-- more columns declared than returned is rejected
SELECT * FROM clickhouse_query('binary_loopback', 'SELECT 1') AS t(x int, y text);
-- mismatch is rejected even when query returns no rows
SELECT * FROM clickhouse_query('binary_loopback', 'SELECT 1 WHERE 0')
    AS t(x int, y text);
-- DDL returns zero columns, so it runs through clickhouse_perform
CALL clickhouse_perform(
    'binary_loopback', 'CREATE TABLE ddl (c1 Int32) ENGINE = Memory'
);
-- zero columns returned yields no rows; the statement still ran remotely
SELECT * FROM clickhouse_query('binary_loopback', 'DROP TABLE ddl') AS t(x int);
-- value not coercible to the declared type is rejected
SELECT * FROM clickhouse_query('binary_loopback', 'SELECT ''abc''') AS t(x int);
-- unknown server is rejected
SELECT * FROM clickhouse_query('no_such_server', 'SELECT 1') AS t(x int);
-- Nullable(Tuple(...)) is Beta, off by default, and only exists from CH 26
-- onward; older servers reject the type outright regardless of settings.
SELECT split_part(clickhouse_server_version('binary_loopback'), '.', 1)::int >= 26
    AS ch_nullable_tuple \gset
\if :ch_nullable_tuple
-- null nested tuple in first row, conversion state initializes lazily
SELECT * FROM clickhouse_query(
    'binary_loopback',
    $$
    SELECT tuple(
        if(number = 0,
           CAST(NULL, 'Nullable(Tuple(Int32))'),
           CAST(tuple(toInt32(number)), 'Nullable(Tuple(Int32))'))
    )
    FROM numbers(2)
    ORDER BY number
    SETTINGS allow_experimental_nullable_tuple_type = 1
    $$
) AS t(v text);
-- non-null nested tuple first, later null row skips cached converter
SELECT * FROM clickhouse_query(
    'binary_loopback',
    $$
    SELECT tuple(
        if(number = 0,
           CAST(NULL, 'Nullable(Tuple(Int32))'),
           CAST(tuple(toInt32(number)), 'Nullable(Tuple(Int32))'))
    )
    FROM numbers(2)
    ORDER BY number DESC
    SETTINGS allow_experimental_nullable_tuple_type = 1
    $$
) AS t(v text);
-- same shape declared as a composite type; null nested tuple yields NULL field
CREATE TYPE nested_tup_inner AS (a int);
CREATE TYPE nested_tup AS (t nested_tup_inner);
SELECT * FROM clickhouse_query(
    'binary_loopback',
    $$
    SELECT tuple(
        if(number = 0,
           CAST(NULL, 'Nullable(Tuple(Int32))'),
           CAST(tuple(toInt32(number)), 'Nullable(Tuple(Int32))'))
    )
    FROM numbers(2)
    ORDER BY number
    SETTINGS allow_experimental_nullable_tuple_type = 1
    $$
) AS t(v nested_tup);
DROP TYPE nested_tup, nested_tup_inner;
\endif

DROP USER MAPPING FOR CURRENT_USER SERVER binary_loopback;
CALL clickhouse_perform('binary_admin', 'DROP DATABASE binary_test');

DROP SERVER binary_loopback CASCADE;
