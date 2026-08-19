-- Fold imported ClickHouse names to lowercase, so PostgreSQL queries need no
-- double quotes. Read columns from the catalog rather than \d+, whose footer
-- differs between PostgreSQL versions
CREATE SERVER import_case_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'import_case_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER import_case_loopback;

CREATE SERVER import_case_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER import_case_admin;

CALL clickhouse_perform('import_case_admin', 'DROP DATABASE IF EXISTS import_case_test');
CALL clickhouse_perform('import_case_admin', 'CREATE DATABASE import_case_test');

CALL clickhouse_perform('import_case_admin', 'CREATE TABLE import_case_test.Hits (
    WatchID Int64, JavaEnable Int8, Title String, plain Int32
) ENGINE = MergeTree ORDER BY (WatchID);');
CALL clickhouse_perform('import_case_admin', 'INSERT INTO import_case_test.Hits
    VALUES (1, 2, ''page'', 3)');

-- Columns differing only by case cannot both fold
CALL clickhouse_perform('import_case_admin', 'CREATE TABLE import_case_test.clash (
    id Int32, Val String, val String
) ENGINE = MergeTree ORDER BY (id);');
CALL clickhouse_perform('import_case_admin', 'INSERT INTO import_case_test.clash
    VALUES (1, ''upper'', ''lower'')');

-- Nor can tables differing only by case
CALL clickhouse_perform('import_case_admin', 'CREATE TABLE import_case_test.Twin (
    id Int32
) ENGINE = MergeTree ORDER BY (id);');
CALL clickhouse_perform('import_case_admin', 'CREATE TABLE import_case_test.twin (
    id Int32
) ENGINE = MergeTree ORDER BY (id);');

-- A backslash in a ClickHouse name must survive as one backslash in the
-- column_name option of the generated CREATE FOREIGN TABLE
CALL clickhouse_perform('import_case_admin', 'CREATE TABLE import_case_test.weird (
    id Int32, `Back\\slash` Int32
) ENGINE = MergeTree ORDER BY (id);');

CREATE SCHEMA import_case;
IMPORT FOREIGN SCHEMA import_case_test FROM SERVER import_case_loopback
    INTO import_case OPTIONS (table_case 'lower', column_case 'lower');

-- Folded columns carry the ClickHouse spelling in a column_name option
SELECT c.relname, a.attname, a.attfdwoptions
  FROM pg_class c
  JOIN pg_namespace n ON n.oid = c.relnamespace
  JOIN pg_attribute a ON a.attrelid = c.oid
 WHERE n.nspname = 'import_case' AND a.attnum > 0
 ORDER BY c.relname COLLATE "C", a.attnum;

SELECT octet_length((attfdwoptions)[1]) AS option_bytes
  FROM pg_attribute
 WHERE attrelid = 'import_case.weird'::regclass AND attname = 'back\slash';

-- Unquoted lowercase names reach the case-sensitive ClickHouse columns
SELECT watchid, javaenable, title, plain FROM import_case.hits WHERE title = 'page';
SELECT * FROM import_case."clash" ORDER BY id;
INSERT INTO import_case.hits (watchid, javaenable, title, plain) VALUES (2, 3, 'next', 4);
SELECT watchid, title FROM import_case.hits ORDER BY watchid;

-- column_case alone leaves table names as ClickHouse spells them
CREATE SCHEMA import_case_cols;
IMPORT FOREIGN SCHEMA import_case_test LIMIT TO ("Hits")
    FROM SERVER import_case_loopback INTO import_case_cols OPTIONS (column_case 'lower');

SELECT a.attname, a.attfdwoptions
  FROM pg_attribute a
 WHERE a.attrelid = 'import_case_cols."Hits"'::regclass AND a.attnum > 0
 ORDER BY a.attnum;

-- table_case alone leaves column names as ClickHouse spells them. PostgreSQL
-- matches LIMIT TO against created names, so it lists the folded name
CREATE SCHEMA import_case_tables;
IMPORT FOREIGN SCHEMA import_case_test LIMIT TO (hits)
    FROM SERVER import_case_loopback INTO import_case_tables OPTIONS (table_case 'lower');

SELECT a.attname, a.attfdwoptions
  FROM pg_attribute a
 WHERE a.attrelid = 'import_case_tables.hits'::regclass AND a.attnum > 0
 ORDER BY a.attnum;

-- keep, the default, imports the ClickHouse spelling
CREATE SCHEMA import_case_keep;
IMPORT FOREIGN SCHEMA import_case_test LIMIT TO ("Hits")
    FROM SERVER import_case_loopback INTO import_case_keep
    OPTIONS (table_case 'keep', column_case 'keep');

SELECT a.attname, a.attfdwoptions
  FROM pg_attribute a
 WHERE a.attrelid = 'import_case_keep."Hits"'::regclass AND a.attnum > 0
 ORDER BY a.attnum;

-- Reject an unknown option or value
IMPORT FOREIGN SCHEMA import_case_test FROM SERVER import_case_loopback
    INTO import_case OPTIONS (table_case 'title');
IMPORT FOREIGN SCHEMA import_case_test FROM SERVER import_case_loopback
    INTO import_case OPTIONS (nonesuch 'lower');

DROP USER MAPPING FOR CURRENT_USER SERVER import_case_loopback;
CALL clickhouse_perform('import_case_admin', 'DROP DATABASE import_case_test');
DROP SERVER import_case_loopback CASCADE;
