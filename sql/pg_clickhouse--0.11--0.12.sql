-- Forget cached ClickHouse vector index metadata, so the next vector search
-- reads it from ClickHouse.
CREATE FUNCTION clickhouse_refresh_vector_metadata()
RETURNS VOID
AS 'MODULE_PATHNAME'
LANGUAGE C;
