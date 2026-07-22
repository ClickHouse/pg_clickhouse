-- NULL semantics of the IN family (ScalarArrayOpExpr pushdown).
--
-- ClickHouse evaluates IN under two-valued logic: a probe that finds no
-- match yields 0 even when the searched set contains a NULL, where Postgres
-- computes NULL, which changes negation semantics (NOT(NULL) -> NULL).
-- The has()/countEqual() functions also match a NULL probe against a NULL
-- element as if it were an ordinary value. Every shape below ships regardless:
-- deparse.c uses the cheap native form when it can prove neither side can be
-- NULL (or when the only possible divergence is FALSE-for-NULL and the result
-- merely qualifies rows), and a guarded CASE otherwise that checks at runtime
-- instead, computing Postgres's exact three-valued answer (NULL, not FALSE,
-- or worse, TRUE) in every context. These tests ensure both forms deparse
-- correctly and that the rows returned match local (Postgres) semantics
-- either way.
CREATE SERVER in_null_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'in_null_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER in_null_svr;

CREATE SERVER in_null_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER in_null_admin;

CALL clickhouse_perform('in_null_admin', 'DROP DATABASE IF EXISTS in_null_test');
CALL clickhouse_perform('in_null_admin', 'CREATE DATABASE in_null_test');

-- xn imports as a plain (nullable) int column, xp as int NOT NULL, and arr
-- as int[]; its elements' nullability is never provable at plan time
CALL clickhouse_perform('in_null_admin', 'CREATE TABLE in_null_test.tnull
    (id Int32, xn Nullable(Int32), xp Int32, arr Array(Int32))
    ENGINE = MergeTree ORDER BY id');
CALL clickhouse_perform('in_null_admin', $$
    INSERT INTO in_null_test.tnull
    VALUES (1, 1, 1, [1, 500]), (2, 2, 2, [500]), (3, NULL, 3, [])
$$);

CREATE SCHEMA in_null_test;
IMPORT FOREIGN SCHEMA in_null_test FROM SERVER in_null_svr INTO in_null_test;
SET SESSION search_path = in_null_test,public;

-- The import must carry ClickHouse nullability into the declarations the
-- never-null proofs rely on
\d tnull

-- ============================================================
-- 1. IN over a constant list: always pushable in a WHERE condition
-- ============================================================
-- NULL-free list is exactly equivalent to the native IN
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn IN (1, 500) ORDER BY id;
SELECT id FROM tnull WHERE xn IN (1, 500) ORDER BY id;

-- A NULL in the list only pulls results toward FALSE, which a WHERE treats
-- like the NULL Postgres computes: still the native IN
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn IN (1, NULL) ORDER BY id;
SELECT id FROM tnull WHERE xn IN (1, NULL) ORDER BY id;

-- OR preserves the truth context, so the native IN still ships
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp > 500 OR xn IN (1, NULL) ORDER BY id;
SELECT id FROM tnull WHERE xp > 500 OR xn IN (1, NULL) ORDER BY id;

-- An IS NULL test observes the exact value even inside a WHERE clause: the
-- guarded CASE ships instead, keeping id 2's NULL distinct from a FALSE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE (xn IN (1, NULL)) IS NULL ORDER BY id;
SELECT id FROM tnull WHERE (xn IN (1, NULL)) IS NULL ORDER BY id;

-- ============================================================
-- 2. NOT IN over a constant list: always pushable, native when NULL-free
-- ============================================================
-- ClickHouse's native NOT IN yields NULL for a NULL probe, so a NULL-free
-- list is exact: id 3 (xn IS NULL) is dropped on both systems
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn NOT IN (1, 500) ORDER BY id;
SELECT id FROM tnull WHERE xn NOT IN (1, 500) ORDER BY id;

-- With a NULL in the list, ClickHouse's native NOT IN would return id 2
-- where Postgres returns no rows (every result is NULL or FALSE): the
-- guarded CASE computes Postgres's answer instead of using the native form
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn NOT IN (1, NULL) ORDER BY id;
SELECT id FROM tnull WHERE xn NOT IN (1, NULL) ORDER BY id;

-- NOT over IN is the same thing arriving unrewritten
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE NOT (xn IN (1, NULL)) ORDER BY id;
SELECT id FROM tnull WHERE NOT (xn IN (1, NULL)) ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE NOT (xn IN (1, 500)) ORDER BY id;
SELECT id FROM tnull WHERE NOT (xn IN (1, 500)) ORDER BY id;

-- ============================================================
-- 3. Non-constant arrays: the has()/countEqual() deparse forms
-- ============================================================
-- has() matches a NULL probe against a NULL element where Postgres computes
-- NULL, so = ANY uses the cheap has() form only with a provably non-NULL
-- probe; xp is declared NOT NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xn, 5]) ORDER BY id;
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xn, 5]) ORDER BY id;

-- A nullable probe against an array that may hold a NULL would turn
-- Postgres's NULL (id 3: NULL = ANY({NULL,5})) into TRUE via has(): the
-- guarded CASE ships instead, computing the real NULL at runtime
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn = ANY(ARRAY[xn, 5]) ORDER BY id;
SELECT id FROM tnull WHERE xn = ANY(ARRAY[xn, 5]) ORDER BY id;

-- Basic arithmetic over provably non-NULL inputs is itself provably
-- non-NULL (it returns a value or raises; see never_null_opfuncs), so an
-- expression element doesn't cost the cheap form...
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xp + 0, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xp + 0, 500]) ORDER BY id;

-- ...while the same arithmetic over a nullable input proves nothing:
-- guarded (id 3: 3 = ANY({NULL,500}) is NULL, dropped either way)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xn + 0, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp = ANY(ARRAY[xn + 0, 500]) ORDER BY id;

-- countEqual() = 0 turns any NULL involved into TRUE (id 3: 3 <> ALL of
-- {NULL,5} is NULL on Postgres): <> ALL uses the cheap countEqual() form
-- only with both sides provably non-NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xn, 5]) ORDER BY id;
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xn, 5]) ORDER BY id;

-- ...and with both sides proven, the cheap form ships
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xp, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp <> ALL(ARRAY[xp, 500]) ORDER BY id;

-- `!=` is just `<>` by the time the parser hands it to us (transformed at
-- parse analysis, before any FDW code sees the tree); confirm it deparses
-- identically rather than falling through chfdw_is_equal_op's string match
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp != ALL(ARRAY[xp, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp != ALL(ARRAY[xp, 500]) ORDER BY id;

-- <> ANY deparses by counting elements rather than has() (which would
-- compute <> ALL instead): the cheap form ships with both sides proven
-- non-NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp <> ANY(ARRAY[xp, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xp <> ANY(ARRAY[xp, 500]) ORDER BY id;

-- Neither side is proven non-NULL here, but this still ships: a guarded
-- CASE checks at runtime whether the probe or an array element is NULL and
-- returns a genuine NULL there instead of requiring a plan-time proof
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn <> ANY(ARRAY[xn, 500]) ORDER BY id;
SELECT id FROM tnull WHERE xn <> ANY(ARRAY[xn, 500]) ORDER BY id;

-- A provably empty array is exact via the cheap form regardless of the
-- probe's nullability (has()/countEqual() degenerate correctly on an
-- empty array by arithmetic): id 3's NULL probe joins ids 1/2 under the
-- same FALSE flag, since <> ANY(empty) is FALSE for every probe on both
-- systems, NULL probe included
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn <> ANY(ARRAY[]::int[]) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn <> ANY(ARRAY[]::int[]) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- A non-constant array can be empty at *runtime*, and Postgres resolves an
-- empty array before even looking at the probe: id 3's NULL = ANY('{}') is
-- FALSE, not NULL, so it must group with id 2. The guarded CASE answers
-- NULL for a NULL probe only against a non-empty array and falls through
-- to the empty-array answer otherwise
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn = ANY(arr) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn = ANY(arr) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- ...and the ALL side of the same coin: id 3's NULL <> ALL('{}') is TRUE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn <> ALL(arr) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn <> ALL(arr) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- = ALL ships via the cheap form with both sides proven non-NULL...
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xp = ALL(ARRAY[xp]) ORDER BY id;
SELECT id FROM tnull WHERE xp = ALL(ARRAY[xp]) ORDER BY id;

-- ...but countEqual() counts NULL as equal to NULL (id 3 would come back
-- TRUE where Postgres computes NULL = ALL({NULL}) as NULL): the guarded
-- CASE ships instead
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn = ALL(ARRAY[xn]) ORDER BY id;
SELECT id FROM tnull WHERE xn = ALL(ARRAY[xn]) ORDER BY id;

-- ============================================================
-- 4. Value contexts no longer observe any divergence
-- ============================================================
-- A NULL-free list needs no guard: the native IN is already exact
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn IN (1, 500) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn IN (1, 500) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- A NULL in the list previously forced this local, since the value
-- position demands exactness the native IN can't give; the guarded CASE
-- now ships instead, returning a genuine NULL so id 3 lands in the same
-- group Postgres would put it in
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn IN (1, NULL) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn IN (1, NULL) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- <> ANY ships the same way, aggregation included, despite neither side
-- being proven non-NULL
EXPLAIN (VERBOSE, COSTS OFF)
SELECT xn <> ANY(ARRAY[xn, 500]) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;
SELECT xn <> ANY(ARRAY[xn, 500]) AS flag, count(*) FROM tnull GROUP BY flag ORDER BY flag;

-- An aggregate FILTER only qualifies rows, so it keeps truth-context rules
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*) FILTER (WHERE xn IN (1, NULL)) FROM tnull;
SELECT count(*) FILTER (WHERE xn IN (1, NULL)) FROM tnull;

-- ============================================================
-- 5. Shippability-walker regressions fixed alongside the rules
-- ============================================================
-- A NULL array constant survives const-folding when the probe is a column;
-- inspecting it for the IN-list deparse would detoast a null datum. Local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE xn = ANY(NULL::int[]) ORDER BY id;
SELECT id FROM tnull WHERE xn = ANY(NULL::int[]) ORDER BY id;

-- A CASE arg WHEN .. with an unshippable tested expression shipped without
-- its branches ever being checked. Local, with correct rows.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM tnull WHERE CASE pg_backend_pid() WHEN 0 THEN xp = 1 ELSE xp > 0 END
ORDER BY id;
SELECT id FROM tnull WHERE CASE pg_backend_pid() WHEN 0 THEN xp = 1 ELSE xp > 0 END
ORDER BY id;

-- Cleanup
SET SESSION search_path = public;
DROP SCHEMA in_null_test CASCADE;
CALL clickhouse_perform('in_null_admin', 'DROP DATABASE in_null_test');
DROP USER MAPPING FOR CURRENT_USER SERVER in_null_svr;
DROP SERVER in_null_svr;
