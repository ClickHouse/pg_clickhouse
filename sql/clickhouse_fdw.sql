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

-- Function used for by functions and aggregates to fall back on when pushdown
--fails. The first argument should describe the operation that should have
--been pushed down.
CREATE FUNCTION clickhouse_pushdown(TEXT, VARIADIC "any") RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- No-op function used for aggregate final functions that return BIGINT.
-- Allows their state to be text. Returns NULL.
CREATE FUNCTION ch_noop_bigint(TEXT) RETURNS BIGINT
AS 'MODULE_PATHNAME', 'clickhouse_noop'
LANGUAGE C STRICT;

-- Create error-raising argMax aggregate that should be pushed down to
-- ClickHouse.
CREATE FUNCTION ch_argmax(anyelement, anyelement, anycompatible)
RETURNS anyelement AS $$ SELECT clickhouse_pushdown('aggregate argMax()') $$
LANGUAGE 'plpgsql' IMMUTABLE;

CREATE AGGREGATE argMax(anyelement, anycompatible)
(
    sfunc = ch_argmax,
    stype = anyelement
);

-- Create error-raising argMin aggregate that should be pushed down to
-- ClickHouse.
CREATE FUNCTION ch_argmin(anyelement, anyelement, anycompatible)
RETURNS anyelement AS $$ SELECT clickhouse_pushdown('aggregate argMin()') $$
LANGUAGE 'plpgsql' IMMUTABLE;

CREATE AGGREGATE argMin(anyelement, anycompatible)
(
    sfunc = ch_argmin,
    stype = anyelement
);

-- Variadic aggregate that takes any number of arguments of any type.
CREATE AGGREGATE uniqExact(VARIADIC "any")
(
    SFUNC     = clickhouse_pushdown,     -- raises error
	INITCOND  = 'aggregate uniqExact()', -- what to push down
	STYPE     = TEXT,                    -- state type
	FINALFUNC = ch_noop_bigint           -- returns NULL
);

/*
 * XXX Other variadic aggregates to add:
 *
 * ❯ rg -Fl. 'variable number of parameters'
 * docs/en/sql-reference/aggregate-functions/reference/uniqhll12.md
 * docs/en/sql-reference/aggregate-functions/reference/uniqcombined64.md
 * docs/en/sql-reference/aggregate-functions/reference/uniqcombined.md
 * docs/en/sql-reference/aggregate-functions/reference/corrmatrix.md
 * docs/en/sql-reference/aggregate-functions/reference/covarsampmatrix.md
 * docs/en/sql-reference/aggregate-functions/reference/covarpopmatrix.md
 * docs/en/sql-reference/aggregate-functions/reference/uniqexact.md
 * docs/en/sql-reference/aggregate-functions/reference/uniq.md
 * docs/en/sql-reference/aggregate-functions/reference/uniqthetasketch.md
 *
 * Plus variadic hashing functions:
 * https://clickhouse.com/docs/sql-reference/functions/hash-functions
*/

-- Create error-raising dictGet function that should be pushed down to
-- ClickHouse.
CREATE FUNCTION dictGet(text, text, anyelement)
RETURNS TEXT AS $$ SELECT clickhouse_pushdown('dictGet()') $$
LANGUAGE 'plpgsql' IMMUTABLE;
