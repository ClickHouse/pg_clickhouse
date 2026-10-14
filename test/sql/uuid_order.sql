SET client_min_messages = notice;

CREATE SERVER uuid_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER uuid_admin;
CREATE SERVER uuid_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'uuid_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER uuid_svr;

CALL clickhouse_perform('uuid_admin', 'DROP DATABASE IF EXISTS uuid_test');
CALL clickhouse_perform('uuid_admin', 'CREATE DATABASE uuid_test');
-- ClickHouse compares UUID by second half first; these rows sort differently
CALL clickhouse_perform('uuid_admin', $$
    CREATE TABLE uuid_test.ids (id Int32, u Nullable(UUID))
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('uuid_admin', $$
    INSERT INTO uuid_test.ids VALUES
    (1, '00000000-0000-0000-0000-000000000002'),
    (2, '00000000-0000-0001-0000-000000000001'),
    (3, '00000001-0000-0000-0000-000000000000'),
    (4, '00000000-0000-0000-0100-000000000000'),
    (5, '018f05c9-4ab8-7b86-b64e-c9f03fbd45d1'),
    (6, '018f05c9-4ab9-7000-8000-000000000000'),
    (7, NULL)
$$);

CREATE SCHEMA uuid_test;
IMPORT FOREIGN SCHEMA "uuid_test" FROM SERVER uuid_svr INTO uuid_test;

EXPLAIN (VERBOSE, COSTS OFF) SELECT id, u FROM uuid_test.ids ORDER BY u;
SELECT id, u FROM uuid_test.ids ORDER BY u;
SELECT id, u FROM uuid_test.ids ORDER BY u DESC NULLS LAST LIMIT 3;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM uuid_test.ids WHERE u < '00000000-0000-0000-0100-000000000000' ORDER BY id;
SELECT id FROM uuid_test.ids WHERE u < '00000000-0000-0000-0100-000000000000' ORDER BY id;
SELECT id FROM uuid_test.ids WHERE u >= '018f05c9-4ab8-7b86-b64e-c9f03fbd45d1' ORDER BY id;
SELECT id FROM uuid_test.ids
WHERE u BETWEEN '00000000-0000-0001-0000-000000000000' AND '00000001-0000-0000-0000-000000000000'
ORDER BY id;

-- Equality keeps native comparison
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM uuid_test.ids WHERE u = '00000000-0000-0001-0000-000000000001';

PREPARE uuid_lt(uuid) AS SELECT id FROM uuid_test.ids WHERE u < $1 ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE uuid_lt('00000001-0000-0000-0000-000000000000');
EXECUTE uuid_lt('00000001-0000-0000-0000-000000000000');
DEALLOCATE uuid_lt;

-- Join on ids so comparison spans two columns
EXPLAIN (VERBOSE, COSTS OFF)
SELECT a.id, b.id FROM uuid_test.ids a JOIN uuid_test.ids b ON a.u < b.u
WHERE a.id = 2 ORDER BY b.id;
SELECT a.id, b.id FROM uuid_test.ids a JOIN uuid_test.ids b ON a.u < b.u
WHERE a.id = 2 ORDER BY b.id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, row_number() OVER (ORDER BY u) FROM uuid_test.ids ORDER BY id;
SELECT id, row_number() OVER (ORDER BY u) FROM uuid_test.ids ORDER BY id;

-- greatest/least stay local
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, greatest(u, '00000000-0000-0000-0000-000000000002') FROM uuid_test.ids;
SELECT id, greatest(u, '00000000-0000-0000-0000-000000000002') FROM uuid_test.ids ORDER BY id;

DROP SCHEMA uuid_test CASCADE;
CALL clickhouse_perform('uuid_admin', 'DROP DATABASE uuid_test');
DROP USER MAPPING FOR CURRENT_USER SERVER uuid_svr;
DROP SERVER uuid_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER uuid_admin;
DROP SERVER uuid_admin;
