-- Drive pg_clickhouse against the misbehaving mock server defined in
-- test/mock/mock_clickhouse.py. This file lives outside test/sql/ on purpose
-- so it does *not* run as part of `make installcheck`: it requires the mock
-- server to be running on port 18123. To run by hand:
--
--     python3 test/mock/mock_clickhouse.py --port 18123 &
--     psql -d <db> -f test/mock/mock_misbehaving.sql
--
-- Each query below is expected to surface a Postgres ERROR (the mock is
-- intentionally returning bad bytes); the goal is that none of them crash
-- the backend or hang it. The companion pytest suite at
-- test/mock/test_mock_clickhouse.py asserts the wire-level behavior of the
-- mock itself.

\set mock_conn 'host=127.0.0.1 port=18123'

-- Sanity: the mock returns a single happy row for unmarked queries.
SELECT clickhouse_raw_query('SELECT 1', :'mock_conn');

-- A 500 from ClickHouse must surface as a Postgres ERROR with the upstream
-- exception text intact (not an empty / generic transport error).
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=http_500\nSELECT 1', :'mock_conn'
);

-- 503 with no body should still produce an ERROR, not a misleading empty row.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=http_503\nSELECT 1', :'mock_conn'
);

-- Unterminated array literal: parser must reject, not crash. Regression test
-- for the class of bugs fixed in PR #188.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=invalid_array\nSELECT 1', :'mock_conn'
);

-- Open quote inside an array with no close.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=unterminated_string\nSELECT 1', :'mock_conn'
);

-- Field ends with a lone escape byte.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=backslash_at_eof\nSELECT 1', :'mock_conn'
);

-- Valid prefix, binary garbage tail, no trailing newline.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=trailing_garbage\nSELECT 1', :'mock_conn'
);

-- Chunked transfer cut short mid-body.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=truncated_chunked\nSELECT 1', :'mock_conn'
);

-- Connection dropped after partial body.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=drop_during_body\nSELECT 1', :'mock_conn'
);

-- Server lies about Content-Length and closes early.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=wrong_content_length\nSELECT 1', :'mock_conn'
);

-- application/json content-type with a truncated JSON body. The text driver
-- doesn't expect JSON, but it must not crash on it either.
SELECT clickhouse_raw_query(
    E'-- BEHAVIOR=invalid_json\nSELECT 1', :'mock_conn'
);

-- After every misbehavior, the connection cache must remain usable. Issuing
-- a happy-path query last verifies we did not leave a broken socket pinned.
SELECT clickhouse_raw_query('SELECT 1', :'mock_conn');
