-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION clickhouse_fdw" to load this file. \quit

-- Set up the FDW.
CREATE FUNCTION clickhouse_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION clickhouse_raw_query(TEXT, TEXT DEFAULT 'host=localhost port=8123')
RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION clickhouse_fdw_validator(text[], oid)
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER clickhouse_fdw
	HANDLER clickhouse_fdw_handler
	VALIDATOR clickhouse_fdw_validator;

-- Function used by variadic aggregate functions when pushdown fails. The
-- first argument should describe the operation that should have been pushed
-- down.
CREATE FUNCTION ch_push_agg_text(TEXT, VARIADIC "any") RETURNS TEXT
AS 'MODULE_PATHNAME', 'clickhouse_func_push_fail'
LANGUAGE C STRICT;

-- Function used by TEXT-returning functions when pushdown fails. The
-- argument should describe the operation that should have been pushed down.
CREATE FUNCTION ch_push_func_text(TEXT) RETURNS TEXT
AS 'MODULE_PATHNAME', 'clickhouse_func_push_fail'
LANGUAGE C STRICT;

-- No-op functions used for aggregate final functions that specific types.
-- Allows their states to be text. Return NULL.
CREATE FUNCTION ch_noop_bigint(TEXT) RETURNS BIGINT
AS 'MODULE_PATHNAME', 'clickhouse_noop'
LANGUAGE C STRICT;

-- Create error-raising argMax aggregate that should be pushed down to
-- ClickHouse.
CREATE FUNCTION ch_argmax(anyelement, anyelement, anycompatible)
RETURNS anyelement AS $$
BEGIN
    RAISE 'clickhouse_fdw: failed to push down aggregate argMax()'
    USING ERRCODE = 'fdw_error';
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

CREATE AGGREGATE argMax(anyelement, anycompatible)
(
    sfunc = ch_argmax,
    stype = anyelement
);

-- Create error-raising argMin aggregate that should be pushed down to
-- ClickHouse.
CREATE FUNCTION ch_argmin(anyelement, anyelement, anycompatible)
RETURNS anyelement AS $$
BEGIN
    RAISE 'clickhouse_fdw: failed to push down aggregate argMin()'
    USING ERRCODE = 'fdw_error';
END;
$$
LANGUAGE 'plpgsql' IMMUTABLE;

CREATE AGGREGATE argMin(anyelement, anycompatible)
(
    sfunc = ch_argmin,
    stype = anyelement
);

-- Variadic aggregates that take any number of arguments of any type and
-- return a UINT64 (we settle for BIGINT).
CREATE AGGREGATE uniq(VARIADIC "any")
(
    SFUNC     = ch_push_agg_text,   -- raises error
	INITCOND  = 'aggregate uniq()', -- what to push down
	STYPE     = TEXT,               -- state type
	FINALFUNC = ch_noop_bigint      -- returns NULL
);

CREATE AGGREGATE uniqExact(VARIADIC "any")
(
    SFUNC     = ch_push_agg_text,
	INITCOND  = 'aggregate uniqExact()',
	STYPE     = TEXT,
	FINALFUNC = ch_noop_bigint
);

CREATE AGGREGATE uniqCombined(VARIADIC "any")
(
    SFUNC     = ch_push_agg_text,
	INITCOND  = 'aggregate uniqCombined()',
	STYPE     = TEXT,
	FINALFUNC = ch_noop_bigint
);

CREATE AGGREGATE uniqCombined64(VARIADIC "any")
(
    SFUNC     = ch_push_agg_text,
	INITCOND  = 'aggregate uniqCombined64()',
	STYPE     = TEXT,
	FINALFUNC = ch_noop_bigint
);

CREATE AGGREGATE uniqHLL12(VARIADIC "any")
(
    SFUNC     = ch_push_agg_text,
	INITCOND  = 'aggregate uniqHLL12()',
	STYPE     = TEXT,
	FINALFUNC = ch_noop_bigint
);

CREATE AGGREGATE uniqTheta(VARIADIC "any")
(
    SFUNC     = ch_push_agg_text,
	INITCOND  = 'aggregate uniqTheta()',
	STYPE     = TEXT,
	FINALFUNC = ch_noop_bigint
);

/*
 * XXX Other variadic aggregates to add:
 *
 * ❯ rg -Fl. 'variable number of parameters'
 * docs/en/sql-reference/aggregate-functions/reference/corrmatrix.md
 * docs/en/sql-reference/aggregate-functions/reference/covarsampmatrix.md
 * docs/en/sql-reference/aggregate-functions/reference/covarpopmatrix.md
 *
 * Plus variadic hashing functions:
 * https://clickhouse.com/docs/sql-reference/functions/hash-functions
*/

-- Create error-raising functions that should be pushed down to ClickHouse.
CREATE FUNCTION dictGet(TEXT, TEXT, ANYELEMENT)
RETURNS TEXT AS $$ SELECT ch_push_func_text('dictGet()') $$
LANGUAGE 'sql' IMMUTABLE;
