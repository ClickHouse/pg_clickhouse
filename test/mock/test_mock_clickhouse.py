"""Tests for the misbehaving mock ClickHouse server.

These tests do not require PostgreSQL or ClickHouse. They start the in-process
:class:`MockClickHouseServer`, fire HTTP requests with the marker the
extension-side regression test will use, and assert the wire-level shape of
each misbehavior.

The point is to lock down the contract that the SQL regression test
``test/sql/mock_misbehaving.sql`` relies on: when pg_clickhouse hits the mock
with a particular ``-- BEHAVIOR=<name>`` marker, the bytes coming back are
the bytes the parser/transport must cope with. If a future change to the mock
breaks one of those shapes, these tests fail loudly before the regression
suite has a chance to mask it.

Run with:

    python3 -m pytest test/mock/test_mock_clickhouse.py -v
"""

from __future__ import annotations

import http.client
import socket
import threading
import time
from contextlib import contextmanager
from typing import Iterator

import pytest

from mock_clickhouse import BEHAVIORS, MockClickHouseServer


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def server() -> Iterator[MockClickHouseServer]:
    srv = MockClickHouseServer(("127.0.0.1", 0), slow_seconds=0.25)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield srv
    finally:
        srv.shutdown()
        srv.server_close()


def _post(
    server: MockClickHouseServer, body: bytes, *, timeout: float = 2.0
) -> tuple[int, dict[str, str], bytes]:
    """POST ``body`` to the mock and return (status, headers, body).

    Uses :mod:`http.client` directly so chunked transfer / partial reads stay
    visible -- ``requests`` would re-buffer them away.
    """
    host, port = server.server_address
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("POST", "/", body=body)
        resp = conn.getresponse()
        return resp.status, dict(resp.getheaders()), resp.read()
    finally:
        conn.close()


@contextmanager
def _raw_socket(
    server: MockClickHouseServer, body: bytes, *, timeout: float = 2.0
) -> Iterator[socket.socket]:
    """Issue a POST over a raw socket so callers can inspect the wire bytes."""
    host, port = server.server_address
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        request = (
            b"POST / HTTP/1.1\r\n"
            b"Host: " + host.encode() + b"\r\n"
            b"Content-Length: " + str(len(body)).encode() + b"\r\n"
            b"Connection: close\r\n"
            b"\r\n" + body
        )
        sock.sendall(request)
        yield sock
    finally:
        sock.close()


# ---------------------------------------------------------------------------
# Behavior coverage
# ---------------------------------------------------------------------------


def test_behaviors_registry_is_complete() -> None:
    """Sanity: each behavior referenced by the SQL test exists in the mock."""
    expected = {
        "happy",
        "http_500",
        "http_503",
        "invalid_array",
        "truncated_chunked",
        "slow_headers",
        "invalid_json",
        "drop_during_body",
        "trailing_garbage",
        "unterminated_string",
        "backslash_at_eof",
        "wrong_content_length",
    }
    assert expected.issubset(set(BEHAVIORS)), "missing behaviors: " + ", ".join(
        sorted(expected - set(BEHAVIORS))
    )


def test_happy_path(server: MockClickHouseServer) -> None:
    status, _headers, body = _post(server, b"-- BEHAVIOR=happy\nSELECT 1")
    assert status == 200
    assert body == b"1\thello\n"
    assert server.behaviors_seen == ["happy"]


def test_default_when_no_marker(server: MockClickHouseServer) -> None:
    """Requests without a BEHAVIOR marker fall through to the happy path."""
    status, _headers, body = _post(server, b"SELECT 1")
    assert status == 200
    assert body == b"1\thello\n"
    assert server.behaviors_seen == ["happy"]


def test_http_500_carries_clickhouse_error_text(server: MockClickHouseServer) -> None:
    status, headers, body = _post(server, b"-- BEHAVIOR=http_500\nSELECT 1")
    assert status == 500
    assert headers.get("X-ClickHouse-Exception-Code") == "60"
    assert b"DB::Exception" in body


def test_http_503_no_body(server: MockClickHouseServer) -> None:
    status, _headers, body = _post(server, b"-- BEHAVIOR=http_503\nSELECT 1")
    assert status == 503
    assert body == b""


def test_invalid_array_body_is_unterminated(server: MockClickHouseServer) -> None:
    """The parser bug fixed in #188: a final '[' field with no ']'."""
    status, _headers, body = _post(server, b"-- BEHAVIOR=invalid_array\nSELECT 1")
    assert status == 200
    assert body.endswith(b"['unterminated\n")
    assert b"]" not in body


def test_invalid_json_is_truncated(server: MockClickHouseServer) -> None:
    status, headers, body = _post(server, b"-- BEHAVIOR=invalid_json\nSELECT 1")
    assert status == 200
    assert headers.get("Content-Type") == "application/json"
    assert b'"data":[{"x":1' in body
    # No closing brace anywhere -- the body really is truncated.
    assert body.count(b"}") < body.count(b"{")


def test_unterminated_string_in_array(server: MockClickHouseServer) -> None:
    status, _headers, body = _post(server, b"-- BEHAVIOR=unterminated_string\nSELECT 1")
    assert status == 200
    # Open quote, no close.
    assert b"['hello world" in body
    assert body.count(b"'") == 1


def test_backslash_at_eof(server: MockClickHouseServer) -> None:
    status, _headers, body = _post(server, b"-- BEHAVIOR=backslash_at_eof\nSELECT 1")
    assert status == 200
    assert body.endswith(b"\\")


def test_trailing_garbage(server: MockClickHouseServer) -> None:
    status, _headers, body = _post(server, b"-- BEHAVIOR=trailing_garbage\nSELECT 1")
    assert status == 200
    # Starts with valid TSV, ends with binary smear, no terminating newline.
    assert body.startswith(b"1\tok\n2\tnext")
    assert not body.endswith(b"\n")
    assert any(b < 32 for b in body[-32:] if b not in (ord("\t"), ord("\n")))


def test_truncated_chunked_drops_connection(server: MockClickHouseServer) -> None:
    """Chunk header promises 32 bytes but only 5 arrive; socket then closes."""
    body = b"-- BEHAVIOR=truncated_chunked\nSELECT 1"
    with _raw_socket(server, body, timeout=2.0) as sock:
        received = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            received += chunk
            # Bound the read so we never block forever in case of regressions.
            if len(received) > 4096:
                break
    assert b"Transfer-Encoding: chunked" in received
    assert b"\r\n20\r\n" in received  # the 32-byte promise
    payload = received.split(b"\r\n20\r\n", 1)[1]
    # Far less than the promised 32 bytes, no terminating "0\r\n\r\n".
    assert len(payload) < 32
    assert b"\r\n0\r\n\r\n" not in received


def test_drop_during_body_short_circuits(server: MockClickHouseServer) -> None:
    body = b"-- BEHAVIOR=drop_during_body\nSELECT 1"
    with _raw_socket(server, body, timeout=2.0) as sock:
        received = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            received += chunk
            if len(received) > 4096:
                break
    # The mock advertises a 1024-byte body but closes after a few bytes.
    assert b"Content-Length: 1024" in received
    headers, _, payload = received.partition(b"\r\n\r\n")
    assert payload == b"1\thel"


def test_wrong_content_length_closes_early(server: MockClickHouseServer) -> None:
    """Server promises more bytes than it delivers, then drops."""
    body = b"-- BEHAVIOR=wrong_content_length\nSELECT 1"
    with _raw_socket(server, body, timeout=2.0) as sock:
        received = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            received += chunk
            if len(received) > 4096:
                break
    headers, _, payload = received.partition(b"\r\n\r\n")
    # Content-Length should overstate the body.
    cl_line = next(
        line
        for line in headers.split(b"\r\n")
        if line.lower().startswith(b"content-length:")
    )
    declared = int(cl_line.split(b":", 1)[1].strip())
    assert declared > len(payload)


def test_slow_headers_delays_first_byte(server: MockClickHouseServer) -> None:
    server.slow_seconds = 0.25
    start = time.monotonic()
    status, _headers, body = _post(
        server, b"-- BEHAVIOR=slow_headers\nSELECT 1", timeout=2.0
    )
    elapsed = time.monotonic() - start
    assert status == 200
    assert body == b"1\tlate\n"
    # Generous lower bound -- CI clocks can be jittery.
    assert elapsed >= 0.2, f"slow_headers returned in {elapsed:.3f}s"


def test_query_string_marker_also_works(server: MockClickHouseServer) -> None:
    """Some callers cannot edit the body; ?behavior=<name> works too."""
    host, port = server.server_address
    conn = http.client.HTTPConnection(host, port, timeout=2.0)
    try:
        conn.request("POST", "/?behavior=http_500", body=b"SELECT 1")
        resp = conn.getresponse()
        assert resp.status == 500
    finally:
        conn.close()


def test_unknown_behavior_falls_back_to_happy(server: MockClickHouseServer) -> None:
    """Unknown markers should not crash the mock; default to happy."""
    status, _headers, body = _post(server, b"-- BEHAVIOR=does_not_exist\nSELECT 1")
    assert status == 200
    assert body == b"1\thello\n"


def test_server_records_each_request(server: MockClickHouseServer) -> None:
    _post(server, b"-- BEHAVIOR=happy\nSELECT 1")
    _post(server, b"-- BEHAVIOR=http_500\nSELECT 1")
    _post(server, b"-- BEHAVIOR=happy\nSELECT 2")
    assert server.behaviors_seen == ["happy", "http_500", "happy"]
