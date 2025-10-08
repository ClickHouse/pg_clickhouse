SET datestyle = 'ISO';
CREATE SERVER import_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'import_test', driver 'http');
CREATE SERVER import_loopback_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'import_test', driver 'binary');
CREATE SCHEMA clickhouse;
CREATE SCHEMA clickhouse_bin;
CREATE SCHEMA clickhouse_limit;
CREATE SCHEMA clickhouse_except;
CREATE USER MAPPING FOR CURRENT_USER SERVER import_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER import_loopback_bin;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS import_test');
SELECT clickhouse_raw_query('CREATE DATABASE import_test');
SELECT clickhouse_raw_query('CREATE DATABASE import_test_2');

-- integer types
SELECT clickhouse_raw_query('CREATE TABLE import_test.ints (
    c1 Int8, c2 Int16, c3 Int32, c4 Int64,
    c5 UInt8, c6 UInt16, c7 UInt32, c8 UInt64,
    c9 Float32, c10 Nullable(Float64)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO import_test.ints SELECT
    number, number + 1, number + 2, number + 3, number + 4, number + 5,
    number + 6, number + 7, number + 8.1, number + 9.2 FROM numbers(10);');
SELECT clickhouse_raw_query('INSERT INTO import_test.ints SELECT
    number, number + 1, number + 2, number + 3, number + 4, number + 5,
    number + 6, number + 7, number + 8.1, NULL FROM numbers(10, 2);');

SELECT clickhouse_raw_query('CREATE TABLE import_test.types (
    c1 Date, c2 DateTime, c3 String, c4 FixedString(5), c5 UUID,
    c6 Enum8(''one'' = 1, ''two'' = 2),
    c7 Enum16(''one'' = 1, ''two'' = 2, ''three'' = 3),
    c9 Nullable(FixedString(50)),
    c8 LowCardinality(String)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO import_test.types SELECT
    addDays(toDate(''1990-01-01''), number),
    addMinutes(addSeconds(addDays(toDateTime(''1990-01-01 10:00:00''), number), number), number),
    format(''number {0}'', toString(number)),
    format(''num {0}'', toString(number)),
    format(''f4bf890f-f9dc-4332-ad5c-0c18e73f28e{0}'', toString(number)),
    ''two'',
    ''three'',
    toString(number),
    format(''cardinal {0}'', toString(number))
    FROM numbers(10);');

SELECT clickhouse_raw_query('CREATE TABLE import_test.types2 (
    c1 LowCardinality(Nullable(String))
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1) SETTINGS allow_nullable_key = 1;
');
SELECT clickhouse_raw_query('INSERT INTO import_test.types2 SELECT
    format(''cardinal {0}'', toString(number + 1))
    FROM numbers(10);');

SELECT clickhouse_raw_query('CREATE TABLE import_test.ip (
    c1 IPv4,
    c2 IPv6
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query($$
    INSERT INTO import_test.ip VALUES
    ('116.106.34.242', '2001:44c8:129:2632:33:0:252:2'),
    ('116.106.34.243', '2a02:e980:1e::1'),
    ('116.106.34.244', '::1');
$$);

-- array types
SELECT clickhouse_raw_query('CREATE TABLE import_test.arrays (
    c1 Array(Int), c2 Array(String)
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO import_test.arrays SELECT
    [number, number + 1],
    [format(''num{0}'', toString(number)), format(''num{0}'', toString(number + 1))]
    FROM numbers(10);');

-- tuple
SELECT clickhouse_raw_query('CREATE TABLE import_test.tuples (
    c1 Int8,
    c2 Tuple(Int, String, Float32),
	c3 Nested(a Int, b Int),
	c4 Int16
) ENGINE = MergeTree PARTITION BY c1 ORDER BY (c1);
');
SELECT clickhouse_raw_query('INSERT INTO import_test.tuples SELECT
    number,
    (number, toString(number), number + 1.0),
	[toInt32(number),1,1],
	[toInt32(number),2,2],
	toInt16(number)
    FROM numbers(10);');

-- datetime with timezones
SELECT clickhouse_raw_query('CREATE TABLE import_test.timezones (
	t1 DateTime64(6,''UTC''),
	t2 DateTime64(6,''Europe/Berlin''),
	t4 DateTime(''Europe/Berlin''),
	t5 DateTime64(6))
	ENGINE = MergeTree ORDER BY (t1) SETTINGS index_granularity=8192;');

SELECT clickhouse_raw_query('INSERT INTO import_test.timezones VALUES (
	''2020-01-01 11:00:00'',
	''2020-01-01 11:00:00'',
	''2020-01-01 11:00:00'',
	''2020-01-01 11:00:00'')');

SELECT clickhouse_raw_query('INSERT INTO import_test.timezones VALUES (
	''2020-01-01 12:00:00'',
	''2020-01-01 12:00:00'',
	''2020-01-01 12:00:00'',
	''2020-01-01 12:00:00'')');

IMPORT FOREIGN SCHEMA "import_test" FROM SERVER import_loopback INTO clickhouse;

\d+ clickhouse.ints;
\d+ clickhouse.types;
\d+ clickhouse.types2;
\d+ clickhouse.arrays;
\d+ clickhouse.tuples;
\d+ clickhouse.timezones;
\d+ clickhouse.ip;

SELECT * FROM clickhouse.ints ORDER BY c1 DESC LIMIT 4;
SELECT * FROM clickhouse.types ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse.types2 ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse.arrays ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse.tuples ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse.timezones ORDER BY t1 LIMIT 2;
SELECT * FROM clickhouse.ip ORDER BY c1;

IMPORT FOREIGN SCHEMA "import_test" FROM SERVER import_loopback_bin INTO clickhouse_bin;

\d+ clickhouse_bin.ints;
\d+ clickhouse_bin.types;
\d+ clickhouse_bin.types2;
\d+ clickhouse_bin.arrays;
\d+ clickhouse_bin.tuples;
\d+ clickhouse_bin.timezones;
\d+ clickhouse_bin.ip;

SELECT * FROM clickhouse_bin.ints ORDER BY c1 DESC LIMIT 4;
SELECT * FROM clickhouse_bin.types ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse_bin.types2 ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse_bin.arrays ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse_bin.tuples ORDER BY c1 LIMIT 2;
SELECT * FROM clickhouse_bin.timezones ORDER BY t1 LIMIT 2;
SELECT * FROM clickhouse.ip ORDER BY c1;

IMPORT FOREIGN SCHEMA "import_test" LIMIT TO (ints, types) FROM SERVER import_loopback INTO clickhouse_limit;

\d+ clickhouse_limit.ints;
\d+ clickhouse_limit.types;
\d+ clickhouse_limit.arrays;
\d+ clickhouse_limit.tuples;

IMPORT FOREIGN SCHEMA "import_test" EXCEPT (ints, types) FROM SERVER import_loopback INTO clickhouse_except;

\d+ clickhouse_except.ints;
\d+ clickhouse_except.types;
\d+ clickhouse_except.arrays;
\d+ clickhouse_except.tuples;

-- check custom database
SELECT clickhouse_raw_query('CREATE TABLE import_test_2.custom_option (a Int64) ENGINE = MergeTree ORDER BY (a)');
IMPORT FOREIGN SCHEMA "import_test_2" FROM SERVER import_loopback INTO clickhouse;

EXPLAIN VERBOSE SELECT * FROM clickhouse.custom_option;
ALTER FOREIGN TABLE clickhouse.custom_option OPTIONS (DROP database);
EXPLAIN VERBOSE SELECT * FROM clickhouse.custom_option;

DROP USER MAPPING FOR CURRENT_USER SERVER import_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER import_loopback_bin;

SELECT clickhouse_raw_query('DROP DATABASE import_test');
SELECT clickhouse_raw_query('DROP DATABASE import_test_2');
DROP SERVER import_loopback_bin CASCADE;
DROP SERVER import_loopback CASCADE;
