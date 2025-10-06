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

-- Create error-raising argMax aggregate that should be pushed down to
-- ClickHouse.
CREATE FUNCTION ch_argmax(anyelement, anyelement, anyelement) RETURNS anyelement
AS $$ BEGIN
	RAISE EXCEPTION 'argMax should be pushed down to ClickHouse';
END $$ LANGUAGE 'plpgsql' IMMUTABLE;

CREATE AGGREGATE argMax(anyelement, anyelement)
(
    sfunc = ch_argmax,
    stype = anyelement
);

-- Create error-raising argMin aggregates that should be pushed down to
-- ClickHouse.
CREATE FUNCTION ch_argmin(anyelement, anyelement, anyelement) RETURNS anyelement
AS $$ BEGIN
	RAISE EXCEPTION 'argMin should be pushed down to ClickHouse';
END $$ LANGUAGE 'plpgsql' IMMUTABLE;

CREATE AGGREGATE argMin(anyelement, anyelement)
(
    sfunc = ch_argmin,
    stype = anyelement
);

-- Create error-raising dictGet function that should be pushed down to
-- ClickHouse.
CREATE FUNCTION dictGet(text, text, anyelement)
RETURNS text
AS $$ BEGIN
	RAISE EXCEPTION 'dictGet should be pushed down to ClickHouse';
END $$ LANGUAGE 'plpgsql' IMMUTABLE;
