-- Forget cached ClickHouse vector index metadata, so the next vector search
-- reads it from ClickHouse.
CREATE FUNCTION clickhouse_refresh_vector_metadata()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Report the ClickHouse vector similarity indexes of a foreign table, with
-- their size and the build mutations not yet done.
CREATE FUNCTION clickhouse_vector_indexes(
    foreign_table REGCLASS,
    OUT name TEXT,
    OUT column_name TEXT,
    OUT definition TEXT,
    OUT compressed_bytes BIGINT,
    OUT pending_mutations INT
) RETURNS SETOF RECORD
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Create error-raising QBit distance functions to push down to ClickHouse.
CREATE FUNCTION L2DistanceTransposed("any", "any", INT) RETURNS FLOAT8
AS 'MODULE_PATHNAME', 'clickhouse_push_fail'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION cosineDistanceTransposed("any", "any", INT) RETURNS FLOAT8
AS 'MODULE_PATHNAME', 'clickhouse_push_fail'
LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION dotProductTransposed("any", "any", INT) RETURNS FLOAT8
AS 'MODULE_PATHNAME', 'clickhouse_push_fail'
LANGUAGE C IMMUTABLE STRICT;
