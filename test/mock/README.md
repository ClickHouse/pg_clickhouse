# Misbehaving Mock ClickHouse

Tests that exercise pg_clickhouse against intentionally malformed ClickHouse
HTTP responses. Companion to issue [#194].

## What it covers

The real ClickHouse server is well-behaved, so the parser (`src/parser.c`)
and HTTP transport (`src/http.c`, `src/http_streaming.c`) rarely see invalid
input in CI. PR [#188] showed there are real bugs lurking when the bytes on
the wire are not the bytes we expect. This mock returns those bytes on
demand so we can lock the behavior down.

Behaviors covered:

- `http_500` — ClickHouse-style server error with `X-ClickHouse-Exception-Code`.
- `http_503` — Service unavailable with no body.
- `invalid_array` — TSV row whose final field opens an array literal but
  never closes it (the bug class fixed in #188).
- `truncated_chunked` — chunked transfer that promises 32 bytes and ships 5,
  then closes without the terminating chunk.
- `slow_headers` — delays the first byte to exercise client-side timeouts.
- `invalid_json` — `application/json` content type with a truncated JSON body.
- `drop_during_body` — sends a `Content-Length` then drops the socket
  partway through the body.
- `trailing_garbage` — valid TSV followed by 32 bytes of binary smear and no
  trailing newline.
- `unterminated_string` — array field with an open quote and no closing quote.
- `backslash_at_eof` — field that ends with a lone backslash (escape with no
  escapee).
- `wrong_content_length` — `Content-Length` larger than the body actually
  delivered.

## Running the tests

The mock and its tests are pure-Python and have no PostgreSQL or ClickHouse
dependency.

```sh
# from the repo root
pip install pytest
python3 -m pytest test/mock -v
```

To run the mock standalone (e.g. when wiring it into a future regression
test):

```sh
python3 test/mock/mock_clickhouse.py --port 18123
```

Then point pg_clickhouse at it:

```sql
SELECT clickhouse_raw_query(
    '-- BEHAVIOR=http_500
SELECT 1',
    'host=127.0.0.1 port=18123'
);
```

The pg-side regression test that drives the mock through `clickhouse_raw_query()`
lives at `test/mock/mock_misbehaving.sql`. It is *not* part of the default
`make installcheck` schedule (the mock server has to be running first); see
the comment at the top of that file for how to opt in.

[#188]: https://github.com/ClickHouse/pg_clickhouse/pull/188
[#194]: https://github.com/ClickHouse/pg_clickhouse/issues/194
