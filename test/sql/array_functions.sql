CREATE SERVER arr_svr FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'arr_test');
CREATE USER MAPPING FOR CURRENT_USER SERVER arr_svr;

CREATE SERVER arr_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER arr_admin;

CALL clickhouse_perform('arr_admin', 'DROP DATABASE IF EXISTS arr_test');
CALL clickhouse_perform('arr_admin', 'CREATE DATABASE arr_test');
CALL clickhouse_perform('arr_admin', $$
    CREATE TABLE arr_test.t1 (
        id   Int32,
        vals Array(Int32),
        tags Array(String),
        list String
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('arr_admin', $$
    INSERT INTO arr_test.t1 VALUES
        (1, [10,20,30], ['a','b','c'], 'aa-bb-cc'),
        (2, [40,50],    ['d','e'], 'x//z'),
        (3, [60],       ['f'], 'Edit -> Insert -> Line Break')
$$);
CALL clickhouse_perform('arr_admin', $$
    CREATE TABLE arr_test.empty_arrays (
        id   Int32,
        vals Array(Int32)
    ) ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('arr_admin', 'INSERT INTO arr_test.empty_arrays VALUES (4, [])');

CREATE SCHEMA arr_test;
IMPORT FOREIGN SCHEMA arr_test FROM SERVER arr_svr INTO arr_test;
SET search_path = arr_test, public;

-- array_cat → arrayConcat
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_cat(vals, ARRAY[99]) = ARRAY[10,20,30,99];
SELECT * FROM t1 WHERE array_cat(vals, ARRAY[99]) = ARRAY[10,20,30,99];

-- array_append → arrayPushBack
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_append(vals, 99) = ARRAY[10,20,30,99];
SELECT * FROM t1 WHERE array_append(vals, 99) = ARRAY[10,20,30,99];

-- array_remove → arrayRemove (CH 26+, EXPLAIN only)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_remove(vals, 20) = ARRAY[10,30];

\unset ECHO
-- Use a DO block to test arrayRemove on 26+ only.
DO $$
DECLARE
	chv int[] := regexp_matches(clickhouse_server_version('arr_svr'), '^(\d+)\.(\d+)')::int[];
    result record;
BEGIN
    IF chv[1] >= 26 THEN
		FOR result IN SELECT * FROM t1 WHERE array_remove(vals, 20) = ARRAY[10,30] LOOP
			RAISE NOTICE '%', result;
		END LOOP;
    ELSE
		-- Fake it on earlier versions.
		RAISE NOTICE '(1,"{10,20,30}","{a,b,c}",aa-bb-cc)';
    END IF;
END;
$$;
\set ECHO all

-- array_to_string → arrayStringConcat
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_to_string(tags, ',') = 'a,b,c';
SELECT * FROM t1 WHERE array_to_string(tags, ',') = 'a,b,c';

-- cardinality → length
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE cardinality(vals) = 3;
SELECT * FROM t1 WHERE cardinality(vals) = 3;

-- array_position → nullIf(indexOf, 0)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_position(vals, 20) = 2;
SELECT * FROM t1 WHERE array_position(vals, 20) = 2;

-- array_position returns NULL rather than zero when the value is absent.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE array_position(vals, 999) IS NULL ORDER BY id;
SELECT id FROM t1 WHERE array_position(vals, 999) IS NULL ORDER BY id;
-- Returns no records because result is `NULL` not `0`.
SELECT id FROM t1 WHERE array_position(vals, 999) = 0 ORDER BY id;

-- Third argument constant > 0 should push down arraySlice().
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_position(vals, 20, 2) = 2;
SELECT * FROM t1 WHERE array_position(vals, 20, 2) = 2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1
WHERE array_position(vals, 20, 2) * 10 = 20
ORDER BY id;
SELECT id FROM t1
WHERE array_position(vals, 20, 2) * 10 = 20
ORDER BY id;

-- Third argument should push down arraySlice(), not found should return NULL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1
WHERE array_position(vals, 999, 2) IS NULL
ORDER BY id;
SELECT id FROM t1
WHERE array_position(vals, 999, 2) IS NULL
ORDER BY id;

-- Should be safe to start search outside array bounds.
SELECT id FROM t1
WHERE array_position(vals, 20, 100) IS NULL
ORDER BY id;

-- Should not find 10 in index 1 because we're starting from 2.
SELECT id FROM t1
WHERE array_position(vals, 10, 2) IS NULL
ORDER BY id;

-- Third argument < 1 should prevent pushdown.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1
WHERE array_position(vals, 20, 0) = 2
ORDER BY id;

-- Non-constant third argument should not push down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1
WHERE array_position(vals, 20, id) IS NOT NULL
ORDER BY id;

-- array_length → nullIf(length(), 0) for the first dimension
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_length(vals, 1) = 2;
SELECT * FROM t1 WHERE array_length(vals, 1) = 2;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM empty_arrays WHERE array_length(vals, 1) IS NULL ORDER BY id;
SELECT id FROM empty_arrays WHERE array_length(vals, 1) IS NULL ORDER BY id;

-- Other dimensions and dynamic dimensions must evaluate locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE array_length(vals, 2) IS NULL ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE array_length(vals, id) IS NULL ORDER BY id;
SELECT id FROM t1 WHERE array_length(vals, id) IS NULL ORDER BY id;

-- array_prepend → arrayPushFront (args reversed)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE array_prepend(99, vals) = ARRAY[99,10,20,30];
SELECT * FROM t1 WHERE array_prepend(99, vals) = ARRAY[99,10,20,30];

-- string_to_array → splitByString
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE string_to_array(list, '-') = ARRAY['aa','bb','cc'];
SELECT * FROM t1 WHERE string_to_array(list, '-') = ARRAY['aa','bb','cc'];
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE string_to_array(list, ' -> ') = ARRAY['aa','bb','cc'];
SELECT * FROM t1 WHERE string_to_array(list, ' -> ') = ARRAY['Edit','Insert', 'Line Break'];

-- split_part → splitByString
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE split_part(list, '-', 2) = 'bb';
SELECT * FROM t1 WHERE split_part(list, '-', 2) = 'bb';
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE split_part(list, '-', -1) = 'cc';
SELECT * FROM t1 WHERE split_part(list, '-', -1) = 'cc';

\unset ECHO
-- Use DO to test functions available in Postgres 14+
-- trim_array → arrayResize(arr, length(arr) - n)
DO $$
DECLARE
    rec record;
	output JSONB;
BEGIN
    IF current_setting('server_version_num')::int >= 140000 THEN
        EXECUTE 'EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t1 WHERE trim_array(vals, 1) = ARRAY[10,20]' INTO output;
        RAISE NOTICE 'trim_array PUSHED DOWN: %', jsonb_path_query(
            output, '$[0].Plan'
        )->>'Remote SQL' = 'SELECT id, vals, tags, list FROM arr_test.t1 WHERE ((arrayResize(vals, length(vals) - 1) = [10,20]))';
        FOR rec IN EXECUTE 'SELECT * FROM t1 WHERE trim_array(vals, 1) = ARRAY[10,20]' LOOP
            RAISE NOTICE '%', rec;
        END LOOP;
    ELSE
        -- Fake it on earlier versions.
        RAISE NOTICE 'trim_array PUSHED DOWN: t';
        RAISE NOTICE '(1,"{10,20,30}","{a,b,c}",aa-bb-cc)';
    END IF;
END;
$$;

-- Use DO to test functions available in Postgres 18+
DO $$
DECLARE
    rec record;
    tc jsonb;
    tests jsonb[] := ARRAY[
        -- PG18+: array_reverse → arrayReverse
        $_${
          "func": "array_reverse",
          "where": "array_reverse(vals) = ARRAY[30,20,10]",
          "push": "((arrayReverse(vals) = [30,20,10]))"
        }$_$,
        -- PG18+: array_sort → arraySort
        $_${
          "func": "array_sort",
          "where": "array_sort(vals, true) = vals",
          "push": "((arraySort(vals) = vals))"
        }$_$,
        -- PG18+ unshippable: 'array_sort(vals, dynamic) = ARRAY[30,20,10]'
        -- Keep before array_sort(vals, const) to ensure function names not
        -- replaced.
        $_${
          "func": "array_sort(x, dynamic)",
          "where": "array_sort(vals, (select true)) = ARRAY[30,20,10]"
        }$_$,
        -- PG18+ 'array_sort(vals, const) = ARRAY[30,20,10]'
        $_${
          "func": "array_sort(x, true)",
          "where": "array_sort(vals, true) = ARRAY[30,20,10]",
          "push": "((arrayReverseSort(vals) = [30,20,10]))"
        }$_$,
        -- PG18+ unshippable: 'array_sort(vals, true, true) = ARRAY[30,20,10]'
        $_${
          "func": "array_sort(x, true, true)",
          "where": "array_sort(vals, true, true) = ARRAY[30,20,10]"
        }$_$
    ];
	output JSONB;
BEGIN
    IF current_setting('server_version_num')::int >= 180000 THEN
        FOREACH tc IN ARRAY tests LOOP
            EXECUTE format('EXPLAIN (VERBOSE, FORMAT JSON) SELECT * FROM t1 WHERE %s', tc->>'where') INTO output;
            If tc ? 'push' THEN
                RAISE NOTICE '% PUSHED DOWN: %', tc->>'func', jsonb_path_query(
                    output, '$[0].Plan'
                )->>'Remote SQL' = format('SELECT id, vals, tags, list FROM arr_test.t1 WHERE %s', tc->>'push');
            ELSE
                RAISE NOTICE '% NOT PUSHED DOWN: %', tc->>'func', jsonb_path_query(
                    output, '$[0].Plan'
                )->>'Remote SQL' = 'SELECT id, vals, tags, list FROM arr_test.t1';
            END IF;
            FOR rec IN EXECUTE format('SELECT * FROM t1 WHERE %s', tc->>'where') LOOP
                RAISE NOTICE '%', rec;
            END LOOP;
        END LOOP;
    ELSE
        -- Fake it for earlier versions.
        RAISE NOTICE 'array_reverse PUSHED DOWN: t';
        RAISE NOTICE '(1,"{10,20,30}","{a,b,c}",aa-bb-cc)';
        RAISE NOTICE 'array_sort PUSHED DOWN: f';
        RAISE NOTICE '(3,{60},{f},"Edit -> Insert -> Line Break")';
        RAISE NOTICE 'array_sort(x, dynamic) NOT PUSHED DOWN: t';
        RAISE NOTICE '(1,"{10,20,30}","{a,b,c}",aa-bb-cc)';
        RAISE NOTICE 'array_sort(x, true) PUSHED DOWN: t';
        RAISE NOTICE '(1,"{10,20,30}","{a,b,c}",aa-bb-cc)';
        RAISE NOTICE 'array_sort(x, true, true) NOT PUSHED DOWN: t';
        RAISE NOTICE '(1,"{10,20,30}","{a,b,c}",aa-bb-cc)';
    END IF;
END;
$$;
\set ECHO all

-- array_shuffle / array_sample added in PG16; output is non-deterministic
-- so put them in WHERE with cardinality predicates that always hold.
SELECT current_setting('server_version_num')::int >= 160000 AS pg16 \gset
\if :pg16
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE cardinality(array_shuffle(vals)) = cardinality(vals) ORDER BY id;
SELECT id FROM t1 WHERE cardinality(array_shuffle(vals)) = cardinality(vals) ORDER BY id;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE cardinality(array_sample(vals, 1)) = 1 ORDER BY id;
SELECT id FROM t1 WHERE cardinality(array_sample(vals, 1)) = 1 ORDER BY id;
\endif

-- Operators: @> → hasAll
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals @> ARRAY[10];
SELECT * FROM t1 WHERE vals @> ARRAY[10] ORDER BY id;

-- Operators: <@ → hasAll (reversed)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals <@ ARRAY[10,20,30];
SELECT * FROM t1 WHERE vals <@ ARRAY[10,20,30] ORDER BY id;

-- Operators: && → hasAny
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals && ARRAY[20,40];
SELECT * FROM t1 WHERE vals && ARRAY[20,40] ORDER BY id;

-- Subscript pushdown
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals[1] = 10;
SELECT * FROM t1 WHERE vals[1] = 10;

-- Slicing pushdown
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals[1:2] = ARRAY[10,20];
SELECT * FROM t1 WHERE vals[1:2] = ARRAY[10,20];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals[:2] = ARRAY[10,20];
SELECT * FROM t1 WHERE vals[:2] = ARRAY[10,20];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals[2:] = ARRAY[20,30];
SELECT * FROM t1 WHERE vals[2:] = ARRAY[20,30];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE vals[:] = ARRAY[10,20,30];
SELECT * FROM t1 WHERE vals[:] = ARRAY[10,20,30];

-- Unshippable (function NOT in Remote SQL)
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_dims(vals) = '[1:3]';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_ndims(vals) = 1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_lower(vals, 1) = 1;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_upper(vals, 1) = 3;
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_replace(vals, 20, 99) = ARRAY[10,99,30];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_positions(vals, 20) = ARRAY[2];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_to_string(vals, ',', '*') = '10,20,30';
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE array_fill(7, ARRAY[2], vals) = '[60:61]={7,7}'::int[];
EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t1 WHERE string_to_array(list, '/', 'nil') = ARRAY['x','','z'];

DROP USER MAPPING FOR CURRENT_USER SERVER arr_svr;
CALL clickhouse_perform('arr_admin', 'DROP DATABASE arr_test');
DROP SERVER arr_svr CASCADE;
