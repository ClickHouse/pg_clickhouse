-- Report the ClickHouse server version ("major.minor.patch") for a foreign
-- server, connecting if necessary.
CREATE FUNCTION clickhouse_server_version(server_name TEXT)
RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Execute query against foreign server, mapping ClickHouse result to Postgres
-- types named in caller's column definition list, e.g.
-- SELECT FROM clickhouse_query('srv', 'SELECT a, b FROM t') AS (a int, b text);
CREATE FUNCTION clickhouse_query(TEXT, TEXT)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

-- Make sure PUBLIC can't run arbitrary remote queries; roles must be granted
-- explicit access.
REVOKE EXECUTE ON FUNCTION clickhouse_query(text, text) FROM PUBLIC;

-- Execute statement against foreign server, discarding any result, e.g.
-- CALL clickhouse_perform('srv', 'CREATE TABLE t (a Int32) ENGINE = Memory');
CREATE PROCEDURE clickhouse_perform(TEXT, TEXT)
AS 'MODULE_PATHNAME'
LANGUAGE C;

-- Make sure PUBLIC can't run arbitrary remote queries; roles must be granted
-- explicit access.
REVOKE EXECUTE ON PROCEDURE clickhouse_perform(text, text) FROM PUBLIC;
