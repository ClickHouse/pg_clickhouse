\unset ECHO
CREATE SERVER regex_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'regex_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER regex_loopback;

CREATE SERVER regex_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER regex_admin;

CALL clickhouse_perform('regex_admin', 'DROP DATABASE IF EXISTS regex_test');
CALL clickhouse_perform('regex_admin', 'CREATE DATABASE regex_test');

CALL clickhouse_perform('regex_admin', $$
	CREATE TABLE regex_test.strings(id Int32, val String) engine=TinyLog()
$$);

CALL clickhouse_perform('regex_admin', $$
	INSERT INTO regex_test.strings
	VALUES (1, 'val1'),
		   (2, 'val2'),
		   (3, 'a,b,c'),
		   (4, 'foo,bar,baz'),
		   (5, 'sleep   no   more'),
		   (6, 'aa-T-bb-t-cc'),
           (7, 'test@example.com, user@hi.example'),
           (8, 'line\ntarget')
$$);

CREATE FOREIGN TABLE strings (id int, val text) SERVER regex_loopback;

\unset ECHO
-- Use DO to test functions available in Postgres 15+
-- PG15+: trim_array → match()
DO $_$
DECLARE
    rec record;
    tc jsonb;
	output JSONB;
    tests jsonb[] := ARRAY[
        $${
          "func": "regexp_like",
          "where": "regexp_like(val, '^val\\d')",
          "push": "(match(val, '^val\\\\d'))"
        }$$,
        $${
          "func": "case-insensitive regexp_like",
          "where": "regexp_like(val, '^VAL\\d', 'i')",
          "push": "(match(val, concat('(?i)', '^VAL\\\\d')))"
        }$$,
        $${
          "func": "regexp_like with si",
          "where": "regexp_like(val, '^VAL\\d', 'si')",
          "push": "(match(val, concat('(?is)', '^VAL\\\\d')))"
        }$$,
        $${
          "func": "regexp_like with unsupported flag",
          "where": "regexp_like(val, '^VAL\\d', 'ix')"
        }$$,
        $${
          "func": "regexp_like with t flag",
          "where": "regexp_like(val, '^VAL\\d', 'it')",
          "push": "(match(val, concat('(?i)', '^VAL\\\\d')))"
        }$$
    ];
BEGIN
    IF current_setting('server_version_num')::int >= 150000 THEN
        FOREACH tc IN ARRAY tests LOOP
            EXECUTE format('EXPLAIN (VERBOSE, FORMAT JSON) SELECT val FROM strings WHERE %s', tc->>'where') INTO output;
            If tc ? 'push' THEN
                RAISE NOTICE '% PUSHED DOWN: %', tc->>'func', jsonb_path_query(
                    output, '$[0].Plan'
                )->>'Remote SQL' = format('SELECT val FROM regex_test.strings WHERE %s', tc->>'push');
            ELSE
                RAISE NOTICE '% NOT PUSHED DOWN: %', tc->>'func', jsonb_path_query(
                    output, '$[0].Plan'
                )->>'Remote SQL' = 'SELECT val FROM regex_test.strings';
            END IF;
            FOR rec IN EXECUTE format('SELECT val FROM strings WHERE %s', tc->>'where') LOOP
                RAISE NOTICE '%', rec;
            END LOOP;
        END LOOP;
    ELSE
        -- Fake it on earlier versions.
        RAISE NOTICE 'regexp_like PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'case-insensitive regexp_like PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'regexp_like with si PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'regexp_like with unsupported flag NOT PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
        RAISE NOTICE 'regexp_like with t flag PUSHED DOWN: t';
        RAISE NOTICE '(val1)';
        RAISE NOTICE '(val2)';
    END IF;
END;
$_$;

\set ECHO all

-- Check regexp_split_to_array.
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, ',') = '{a,b,c}'::text[];
SELECT val FROM strings WHERE regexp_split_to_array(val, ',') = '{a,b,c}'::text[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, '\s+') = '{sleep,no,more}'::text[];
SELECT val FROM strings WHERE regexp_split_to_array(val, '\s+') = '{sleep,no,more}'::text[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, '-t-', 'i') = '{aa,bb,cc}'::text[];
SELECT val FROM strings WHERE regexp_split_to_array(val, '-t-', 'i') = '{aa,bb,cc}'::text[];
-- No pushdown when the regex is not a constant.
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, val) = '{}'::text[];

-- Check regexp_replace().
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '[0,1]$', '_xyz') = 'val_xyz';
SELECT * FROM strings WHERE regexp_replace(val, '[0,1]$', '_xyz') = 'val_xyz';
-- No replace returns unmodified string.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '^x', 'y') = 'val2';
SELECT * FROM strings WHERE regexp_replace(val, '^x', 'y') = 'val2';
-- Case-insensitive, refer to capture.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, 'VAL([0,1])$', 'x-\1', 'i') = 'x-1';
SELECT * FROM strings WHERE regexp_replace(val, 'VAL([0,1])$', 'x-\1', 'i') = 'x-1';
-- Case-insensitive.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'i') = 'xal1';
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'i') = 'xal1';
-- Replace all case-insensitive.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'gig') = 'xax1';
SELECT * FROM strings WHERE regexp_replace(val, '[VL]', 'x', 'gig') = 'xax1';
-- Refer to full match.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM strings WHERE regexp_replace(val, '^val', 'x-\0', 'i') = 'x-val1';
SELECT * FROM strings WHERE regexp_replace(val, '^val', 'x-\0', 'i') = 'x-val1';
-- No pushdown when the regex is not a constant.
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM strings WHERE regexp_replace(val, val, '') = '';

-- Check regexp_match().
EXPLAIN (VERBOSE, COSTS OFF)
SELECT regexp_match(val, '[01]$') FROM strings WHERE regexp_match(val, '[01]$') = '{1}'::text[];
SELECT regexp_match(val, '[01]$') FROM strings WHERE regexp_match(val, '[01]$') = '{1}'::text[];
-- No match.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, regexp_match(val, '[34]$') FROM strings WHERE regexp_match(val, '[34]$') = '{}'::text[];
SELECT id, regexp_match(val, '[34]$') FROM strings WHERE regexp_match(val, '[34]$') = '{}'::text[];
-- Multiple matches.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT regexp_match(val, 'ba') FROM strings WHERE regexp_match(val, 'ba') = '{ba}'::text[];
SELECT regexp_match(val, 'ba') FROM strings WHERE regexp_match(val, 'ba') = '{ba}'::text[];
-- Capturing groups.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT regexp_match(val, '([a-zA-Z0-9]+)@') FROM strings WHERE regexp_match(val, '([a-zA-Z0-9]+)@') = '{test}'::text[];
SELECT regexp_match(val, '([a-zA-Z0-9]+)@') FROM strings WHERE regexp_match(val, '([a-zA-Z0-9]+)@') = '{test}'::text[];
-- Case-insensitive.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT regexp_match(val, 'VAL[01]$', 'i') FROM strings WHERE regexp_match(val, 'VAL[01]$', 'i') = '{val1}'::text[];
SELECT regexp_match(val, 'VAL[01]$', 'i') FROM strings WHERE regexp_match(val, 'VAL[01]$', 'i') = '{val1}'::text[];
-- No pushdown when the regex is not a constant.
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM strings WHERE regexp_match(val, val) = '{1}'::text[];

-- Compare consistency of `.` matching a newline.
SELECT regexp_match(val, 'line.') FROM strings
 WHERE regexp_match(val, 'line.') = E'{"line\n"}'::text[];
SELECT regexp_match(val, 'line.', 's') FROM strings
 WHERE regexp_match(val, 'line.', 's') = E'{"line\n"}'::text[];
SELECT regexp_match(val, 'line.', 'm'), id FROM strings
 WHERE regexp_match(val, 'line.', 'm') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, 'line.', 'n'), id FROM strings
 WHERE regexp_match(val, 'line.', 'n') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, 'line.', 'p'), id FROM strings
 WHERE regexp_match(val, 'line.', 'p') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, 'line.', 'w') FROM strings
 WHERE regexp_match(val, 'line.', 'w') = E'{"line\n"}'::text[];
-- Last flag overrides.
SELECT regexp_match(val, 'line.', 'ms') FROM strings
 WHERE regexp_match(val, 'line.', 'ms') = E'{"line\n"}'::text[];
SELECT regexp_match(val, 'line.', 'sm'), id FROM strings
 WHERE regexp_match(val, 'line.', 'sm') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, 'line.', 'mspw') FROM strings
 WHERE regexp_match(val, 'line.', 'mspw') = E'{"line\n"}'::text[];

 -- Compare consistency of `[^x]` matching a newline.
SELECT regexp_match(val, 'line[^x]') FROM strings
 WHERE regexp_match(val, 'line[^x]') = E'{"line\n"}'::text[];
SELECT regexp_match(val, 'line[^x]', 's') FROM strings
 WHERE regexp_match(val, 'line[^x]', 's') = E'{"line\n"}'::text[];
-- m, n match in CH but not PG
SELECT regexp_match(val, 'line[^x]', 'm') FROM strings
 WHERE regexp_match(val, 'line[^x]', 'm') = E'{"line\n"}'::text[];
SELECT regexp_match(val, 'line[^x]', 'n') FROM strings
 WHERE regexp_match(val, 'line[^x]', 'n') = E'{"line\n"}'::text[];
SELECT regexp_match(val, 'line[^x]', 'p') FROM strings
 WHERE regexp_match(val, 'line[^x]', 'p') = E'{"line\n"}'::text[];
-- w matches in both.
SELECT regexp_match(val, 'line[^x]', 'w') FROM strings
 WHERE regexp_match(val, 'line[^x]', 'w') = E'{"line\n"}'::text[];
-- Last flag overrides.
SELECT regexp_match(val, 'line[^x]', 'msw') FROM strings
 WHERE regexp_match(val, 'line[^x]', 'msw') = E'{"line\n"}'::text[];

-- Compare consistency of `$` end of line.
SELECT regexp_match(val, 'line$'), id FROM strings
 WHERE regexp_match(val, 'line$') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, 'line$', 'm') FROM strings
 WHERE regexp_match(val, 'line$', 'm') = E'{line}'::text[];
SELECT regexp_match(val, 'line$', 'n') FROM strings
 WHERE regexp_match(val, 'line$', 'n') = E'{line}'::text[];
SELECT regexp_match(val, 'line$', 's'), id FROM strings
 WHERE regexp_match(val, 'line$', 's') = '{}'::text[] ORDER BY id;
-- p matches in CH but not PG.
SELECT regexp_match(val, 'line$', 'p'), id FROM strings
 WHERE regexp_match(val, 'line$', 'p') = '{}'::text[] ORDER BY id;
-- w matches in both.
SELECT regexp_match(val, 'line$', 'w') FROM strings
 WHERE regexp_match(val, 'line$', 'w') = E'{line}'::text[];
-- Last flag overrides.
SELECT regexp_match(val, 'line$', 'ms'), id FROM strings
 WHERE regexp_match(val, 'line$', 'ms') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, 'line$', 'sm') FROM strings
 WHERE regexp_match(val, 'line$', 'sm') = E'{line}'::text[];
SELECT regexp_match(val, 'line$', 'smw') FROM strings
 WHERE regexp_match(val, 'line$', 'smw') = E'{line}'::text[];

-- Compare consistency of `^` start of line.
SELECT regexp_match(val, '^target'), id FROM strings
 WHERE regexp_match(val, '^target') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, '^target', 'm') FROM strings
 WHERE regexp_match(val, '^target', 'm') = E'{target}'::text[];
SELECT regexp_match(val, '^target', 'n') FROM strings
 WHERE regexp_match(val, '^target', 'n') = E'{target}'::text[];
SELECT regexp_match(val, '^target', 's'), id FROM strings
 WHERE regexp_match(val, '^target', 's') = '{}'::text[] ORDER BY id;
-- p matches in CH but not PG.
SELECT regexp_match(val, '^target', 'p'), id FROM strings
 WHERE regexp_match(val, '^target', 'p') = '{}'::text[] ORDER BY id;
-- w matches in both.
SELECT regexp_match(val, '^target', 'w') FROM strings
 WHERE regexp_match(val, '^target', 'w') = E'{target}'::text[];
-- Last flag overrides.
SELECT regexp_match(val, '^target', 'ms'), id FROM strings
 WHERE regexp_match(val, '^target', 'ms') = '{}'::text[] ORDER BY id;
SELECT regexp_match(val, '^target', 'sm') FROM strings
 WHERE regexp_match(val, '^target', 'sm') = E'{target}'::text[];
SELECT regexp_match(val, '^target', 'msw') FROM strings
 WHERE regexp_match(val, '^target', 'msw') = E'{target}'::text[];

-- Ensure no pushdown when we disable it.
SET pg_clickhouse.pushdown_regex = 'false';
EXPLAIN (VERBOSE, COSTS OFF) SELECT val FROM strings WHERE regexp_split_to_array(val, ',') = '{a,b,c}'::text[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM strings WHERE regexp_replace(val, '^x', 'y') = 'val2';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM strings WHERE regexp_match(val, '.') = '{1}'::text[];

\unset ECHO
-- Use DO to test functions available in Postgres 15+
-- PG15+: trim_array → match()
DO $_$
DECLARE
	output JSONB;
BEGIN
    IF current_setting('server_version_num')::int >= 150000 THEN
        EXECUTE format($$EXPLAIN (VERBOSE, FORMAT JSON) SELECT val FROM strings WHERE regexp_like(val, '^val\d')$$) INTO output;
        RAISE NOTICE 'regexp_like: %', jsonb_path_query(output, '$[0].Plan')->>'Remote SQL';
    ELSE
        -- Fake it on earlier versions.
        RAISE NOTICE 'regexp_like: SELECT val FROM regex_test.strings';
    END IF;
END;
$_$;
\set ECHO all

DROP USER MAPPING FOR CURRENT_USER SERVER regex_loopback;
CALL clickhouse_perform('regex_admin', 'DROP DATABASE regex_test');
DROP SERVER regex_loopback CASCADE;
