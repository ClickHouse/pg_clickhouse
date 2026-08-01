CREATE SERVER engine_args_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'engine_args_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER engine_args_svr;

CREATE SERVER engine_args_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER engine_args_admin;

CALL clickhouse_perform('engine_args_admin', 'DROP DATABASE IF EXISTS engine_args_test');
CALL clickhouse_perform('engine_args_admin', 'CREATE DATABASE engine_args_test');
CALL clickhouse_perform('engine_args_admin', $$
	CREATE TABLE engine_args_test.innocuous (
        id UInt32,
        value String
    )
	ENGINE = MergeTree()
	ORDER BY id
$$);

CALL clickhouse_perform('engine_args_admin', $$
    INSERT INTO engine_args_test.innocuous
    VALUES (1, 'public'), (2, 'data'), (3, 'here')
$$);

CALL clickhouse_perform('engine_args_admin', $$
    CREATE TABLE IF NOT EXISTS engine_args_test.sensitive (
        id UInt32,
        password String
    ) ENGINE=MergeTree() ORDER BY id;
$$);

CALL clickhouse_perform('engine_args_admin', $$
    INSERT INTO engine_args_test.sensitive
    VALUES (1, 'admin123'), (2, 'password!')
$$);

CREATE SCHEMA engine_args_test;

-- Should fail on invalid column name (also attempted SQL injection).
CREATE FOREIGN TABLE engine_args_test.dr_evil (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(1, (SELECT count() FROM sensitive) > 0) -- )'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.dr_evil;

-- Make sure ClickHouse returns the expected exception (formatting changed
-- between 24.3 and 24.8, so use LIKE to evaluate it.
DO $$
BEGIN
    PERFORM COUNT(id) FROM engine_args_test.dr_evil;
    RAISE WARNING 'Unexpected successful execution';
EXCEPTION
    WHEN OTHERS THEN
        IF SQLERRM NOT LIKE ' pg_clickhouse: DB::Exception: Unknown expression or function identifier %'
        AND SQLERRM NOT LIKE '%1, (SELECT count() FROM sensitive) > 0)%' THEN
            RAISE EXCEPTION 'UNEXPECTED EXCEPTION: %', SQLERRM;
        END IF;
        RAISE NOTICE 'Unknown expression error correctly raised from ClickHouse';
END;
$$;

-- Should fail on invalid double-quoted column name.
CREATE FOREIGN TABLE engine_args_test.dr_evil2 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree("id");create table asdf(id Int32);select "a")'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.dr_evil2;

-- Should fail on invalid backtick-quoted column name.
CREATE FOREIGN TABLE engine_args_test.dr_evil3 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(`id`);create table asdf(id Int32);select `a`)'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.dr_evil3;

-- Should work for normal column name.
CREATE FOREIGN TABLE engine_args_test.a_ok (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(id)'
);

EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.a_ok;
SELECT COUNT(id) FROM engine_args_test.a_ok;

-- Should work for "column name".
CREATE FOREIGN TABLE engine_args_test.name_var1 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree("id")'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var1;
SELECT COUNT(id) FROM engine_args_test.name_var1;

-- Should work for `column name`.
CREATE FOREIGN TABLE engine_args_test.name_var2 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(`id`)'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var2;
SELECT COUNT(id) FROM engine_args_test.name_var2;

-- Should not quote "column name""".
CREATE FOREIGN TABLE engine_args_test.name_var3 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree("id""")'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var3;

-- Should not quote "column name\"".
CREATE FOREIGN TABLE engine_args_test.name_var3a (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree("id\"")'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var3a;

-- Should  not quote quote `column name```.
CREATE FOREIGN TABLE engine_args_test.name_var4 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(`id```)'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var4;

-- Should  not quote quote `column name\``.
CREATE FOREIGN TABLE engine_args_test.name_var4a (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(`id\``)'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var4a;

-- Should fail with no parameter.
CREATE FOREIGN TABLE engine_args_test.name_var5 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree()'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var5;

-- Should fail with no parameter delimiters.
CREATE FOREIGN TABLE engine_args_test.name_bare (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_bare;

-- Should fail with no closing delimiter.
CREATE FOREIGN TABLE engine_args_test.name_open (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(id'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_open;

-- Should fail with parameter length > 63.
CREATE FOREIGN TABLE engine_args_test.name_var6 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx)'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var6;

-- Should succeed with max length parameter.
CREATE FOREIGN TABLE engine_args_test.name_var7 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree(xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx)'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var7;

-- Should fail with quoted parameter length > 127.
CREATE FOREIGN TABLE engine_args_test.name_var8 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")'
);
EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var8;

-- Should succeed with quoted parameter length == 130.
CREATE FOREIGN TABLE engine_args_test.name_var9 (id INT) SERVER engine_args_svr
OPTIONS (
  table_name 'innocuous',
  engine 'CollapsingMergeTree("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx")'
);

EXPLAIN (VERBOSE, COSTS OFF) SELECT COUNT(id) FROM engine_args_test.name_var9;

DROP USER MAPPING FOR CURRENT_USER SERVER engine_args_svr;
CALL clickhouse_perform('engine_args_admin', 'DROP DATABASE engine_args_test');

DROP SERVER engine_args_svr CASCADE;
