-- Test ILIKE and POSIX regex operator pushdown
SET datestyle = 'ISO';

CREATE SERVER ilike_regex_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'ilike_regex_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER ilike_regex_bin_svr;

CREATE SERVER ilike_regex_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'ilike_regex_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER ilike_regex_http_svr;

CREATE SERVER ilike_regex_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER ilike_regex_admin;

CALL clickhouse_perform('ilike_regex_admin', 'DROP DATABASE IF EXISTS ilike_regex_test');
CALL clickhouse_perform('ilike_regex_admin', 'CREATE DATABASE ilike_regex_test');

CALL clickhouse_perform('ilike_regex_admin', $$
    CREATE TABLE ilike_regex_test.events (
        id     Int32,
        name   String,
        path   String
    ) ENGINE = MergeTree ORDER BY id
$$);

CALL clickhouse_perform('ilike_regex_admin', $$
    INSERT INTO ilike_regex_test.events VALUES
        (1, 'page_view',   '/users/profile'),
        (2, 'Page_View',   '/users/settings'),
        (3, 'PAGE_VIEW',   '/admin/dashboard'),
        (4, 'add_to_cart', '/products/shoes'),
        (5, 'Add_To_Cart', '/products/hats'),
        (6, 'purchase',    '/checkout'),
        (7, 'PURCHASE',    '/checkout/confirm'),
        (8, 'share',       '/social/twitter'),
        (9, 'logout',      '/auth/logout'),
        (10, 'signup',     '/auth/signup')
$$);

CREATE SCHEMA ilike_regex_bin;
CREATE SCHEMA ilike_regex_http;
IMPORT FOREIGN SCHEMA "ilike_regex_test" FROM SERVER ilike_regex_bin_svr INTO ilike_regex_bin;
IMPORT FOREIGN SCHEMA "ilike_regex_test" FROM SERVER ilike_regex_http_svr INTO ilike_regex_http;

-- =======================================================
-- ILIKE: case-insensitive LIKE pushdown (binary driver)
-- =======================================================

-- ILIKE should match regardless of case
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name ILIKE '%view%' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name ILIKE '%view%' ORDER BY id;

-- NOT ILIKE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name NOT ILIKE '%view%' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name NOT ILIKE '%view%' ORDER BY id;

-- ILIKE in aggregate context
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM ilike_regex_bin.events WHERE name ILIKE 'page%';
SELECT count(*) FROM ilike_regex_bin.events WHERE name ILIKE 'page%';

-- ILIKE with underscore wildcard (matches page_view, Page_View, PAGE_VIEW)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name ILIKE 'page_____' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name ILIKE 'page_____' ORDER BY id;

-- LIKE (case-sensitive) should still work as before
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name LIKE '%view%' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name LIKE '%view%' ORDER BY id;

-- NOT LIKE (case-sensitive)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM ilike_regex_bin.events WHERE name NOT LIKE '%view%';
SELECT count(*) FROM ilike_regex_bin.events WHERE name NOT LIKE '%view%';

-- =======================================================
-- ILIKE: case-insensitive LIKE pushdown (http driver)
-- =======================================================

-- ILIKE should match regardless of case
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_http.events WHERE name ILIKE '%view%' ORDER BY id;
SELECT id, name FROM ilike_regex_http.events WHERE name ILIKE '%view%' ORDER BY id;

-- NOT ILIKE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_http.events WHERE name NOT ILIKE '%view%' ORDER BY id;
SELECT id, name FROM ilike_regex_http.events WHERE name NOT ILIKE '%view%' ORDER BY id;

-- ILIKE with underscore wildcard (matches page_view, Page_View, PAGE_VIEW)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_http.events WHERE name ILIKE 'page_____' ORDER BY id;
SELECT id, name FROM ilike_regex_http.events WHERE name ILIKE 'page_____' ORDER BY id;

-- =======================================================
-- POSIX regex: ~ mapped to match(), !~ to NOT match()
-- =======================================================

-- ~ (case-sensitive regex match)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name ~ '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name ~ '^page' ORDER BY id;

-- !~ (case-sensitive regex no match)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name !~ '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name !~ '^page' ORDER BY id;

-- ~ with alternation pattern
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name ~ '^(purchase|share)$' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name ~ '^(purchase|share)$' ORDER BY id;

-- ~ in aggregate context
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FROM ilike_regex_bin.events WHERE name ~ '_view$';
SELECT count(*) FROM ilike_regex_bin.events WHERE name ~ '_view$';

-- ~ with path column (test regex with slashes)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, path FROM ilike_regex_bin.events WHERE path ~ '^/auth/' ORDER BY id;
SELECT id, path FROM ilike_regex_bin.events WHERE path ~ '^/auth/' ORDER BY id;

-- POSIX regex with http driver
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_http.events WHERE name ~ '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_http.events WHERE name ~ '^page' ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_http.events WHERE name !~ '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_http.events WHERE name !~ '^page' ORDER BY id;

-- =======================================================
-- ~* and !~* pushed down via match(col, concat('(?i)', pattern))
-- =======================================================

-- ~* (case-insensitive regex) should be pushed down
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name ~* '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name ~* '^page' ORDER BY id;

-- !~* (case-insensitive regex negation) should be pushed down
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_bin.events WHERE name !~* '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_bin.events WHERE name !~* '^page' ORDER BY id;

-- ~* with http driver
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, name FROM ilike_regex_http.events WHERE name ~* '^page' ORDER BY id;
SELECT id, name FROM ilike_regex_http.events WHERE name ~* '^page' ORDER BY id;

-- Ensure no regular expression pushdown when we disable it.
SET pg_clickhouse.pushdown_regex = 'false';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ilike_regex_bin.events WHERE name ~ '^page' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ilike_regex_bin.events WHERE name !~ '^page' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ilike_regex_bin.events WHERE name ~* '^page' ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM ilike_regex_bin.events WHERE name !~* '^page' ORDER BY id;

-- Cleanup
CALL clickhouse_perform('ilike_regex_admin', 'DROP DATABASE ilike_regex_test');
DROP USER MAPPING FOR CURRENT_USER SERVER ilike_regex_bin_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER ilike_regex_http_svr;
DROP SERVER ilike_regex_bin_svr CASCADE;
DROP SERVER ilike_regex_http_svr CASCADE;
