"""Mock ClickHouse HTTP server that misbehaves on demand.

The real ClickHouse HTTP interface always returns well-formed responses, so
the pg_clickhouse parser and HTTP transport rarely see the kinds of invalid
input that bug #194 (and the parser fix in #188) imply they should handle
gracefully. This mock is a small standalone HTTP server that intentionally
returns malformed, partial, slow, or dropped responses so the extension can be
exercised against them.

The mock dispatches on a marker that callers embed in the SQL body of the
request:

    -- BEHAVIOR=<name>
    SELECT 1

Supported behaviors are defined in :data:`BEHAVIORS`. A request without a
marker is answered with a single tab-separated row so simple round-trip tests
keep working.

Run standalone:

    python3 test/mock/mock_clickhouse.py --port 18123

Or import :class:`MockClickHouseServer` from a test and start it in a thread.
"""

from __future__ import annotations

import argparse
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Callable
from urllib.parse import parse_qs, urlparse

# A small TSV body that looks like a real ClickHouse text response.
HAPPY_BODY = b"1\thello\n"

# Pattern used to extract the BEHAVIOR marker from a request body or query
# string. Matches '-- BEHAVIOR=<name>' on its own line, or 'behavior=<name>'
# in the query string.
_BEHAVIOR_RE = re.compile(rb"--\s*BEHAVIOR\s*=\s*([A-Za-z0-9_-]+)")


class _RawHandler(BaseHTTPRequestHandler):
    """HTTP handler that emits a configured misbehavior per request."""

    server_version = "MockClickHouse/0.1"

    # Silence the default per-request stderr logging; tests will inspect the
    # captured behavior log instead.
    def log_message(self, format, *args):  # noqa: A002 - signature from BaseHTTPRequestHandler
        return

    def _read_body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length <= 0:
            return b""
        return self.rfile.read(length)

    def _pick_behavior(self, body: bytes) -> str:
        # Body marker wins; fall back to query string for GET-style probes.
        match = _BEHAVIOR_RE.search(body or b"")
        if match:
            return match.group(1).decode("ascii")
        qs = parse_qs(urlparse(self.path).query)
        if "behavior" in qs and qs["behavior"]:
            return qs["behavior"][0]
        return "happy"

    def do_GET(self):  # noqa: N802 - BaseHTTPRequestHandler API
        self._dispatch(self._read_body())

    def do_POST(self):  # noqa: N802 - BaseHTTPRequestHandler API
        self._dispatch(self._read_body())

    def _dispatch(self, body: bytes):
        behavior = self._pick_behavior(body)
        handler = BEHAVIORS.get(behavior, _happy)
        # Record what we did so tests can assert dispatch happened.
        self.server.behaviors_seen.append(behavior)  # type: ignore[attr-defined]
        try:
            handler(self)
        except (BrokenPipeError, ConnectionResetError):
            # The client may have closed the socket on us (the dropped-body
            # behaviors are designed to provoke this); swallow so the server
            # thread keeps serving.
            pass


# --- Behavior implementations -------------------------------------------------
#
# Each behavior takes the request handler and is responsible for producing the
# full response (status, headers, body). They deliberately bypass the helper
# `send_response` flow when they need to emit malformed bytes the API would
# otherwise reject.


def _happy(h: _RawHandler) -> None:
    """Well-formed 200 response with a single TSV row."""
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(HAPPY_BODY)))
    h.end_headers()
    h.wfile.write(HAPPY_BODY)


def _http_500(h: _RawHandler) -> None:
    """ClickHouse-style 500 with the error text in the body."""
    body = b"Code: 60. DB::Exception: Table mock.t does not exist.\n"
    h.send_response(500)
    h.send_header("Content-Type", "text/plain; charset=UTF-8")
    h.send_header("X-ClickHouse-Exception-Code", "60")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _http_503(h: _RawHandler) -> None:
    """Service unavailable, no body."""
    h.send_response(503)
    h.send_header("Content-Length", "0")
    h.end_headers()


def _invalid_array(h: _RawHandler) -> None:
    """200 OK with an unterminated array literal that the parser must reject.

    Mirrors the kind of corruption fixed in PR #188 -- the parser previously
    walked off the end of the buffer when the final field omitted its closing
    bracket.
    """
    body = b"1\t['unterminated\n"
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _truncated_chunked(h: _RawHandler) -> None:
    """Chunked transfer that announces more bytes than it delivers.

    Sends one chunk header promising 32 bytes, then writes only 5 and closes
    the connection without the terminating 0-chunk.
    """
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Transfer-Encoding", "chunked")
    h.end_headers()
    h.wfile.write(b"20\r\n")  # 0x20 = 32 bytes promised
    h.wfile.write(b"1\the")  # only 5 bytes actually written
    h.wfile.flush()
    # Close the underlying socket abruptly. libcurl should surface a transport
    # error rather than emitting a partial row to the parser.
    try:
        h.connection.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    h.connection.close()


def _slow_headers(h: _RawHandler) -> None:
    """Delay before the first byte. Used to exercise client-side timeouts."""
    delay = float(getattr(h.server, "slow_seconds", 0.5))  # type: ignore[attr-defined]
    time.sleep(delay)
    body = b"1\tlate\n"
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _invalid_json(h: _RawHandler) -> None:
    """JSON-flavored response with truncated body + JSON content-type."""
    body = b'{"meta":[{"name":"x"}],"data":[{"x":1'
    h.send_response(200)
    h.send_header("Content-Type", "application/json")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _drop_during_body(h: _RawHandler) -> None:
    """Send headers + a few bytes then drop the connection mid-body."""
    body_prefix = b"1\thel"
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    # Lie about length so the client knows there is more coming.
    h.send_header("Content-Length", "1024")
    h.end_headers()
    h.wfile.write(body_prefix)
    h.wfile.flush()
    try:
        h.connection.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    h.connection.close()


def _trailing_garbage(h: _RawHandler) -> None:
    """Otherwise valid TSV body followed by binary garbage and no newline."""
    body = b"1\tok\n2\tnext" + bytes(range(0, 32))
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _unterminated_string(h: _RawHandler) -> None:
    """Array literal with an open quote and no closing quote."""
    body = b"1\t['hello world\n"
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _backslash_at_eof(h: _RawHandler) -> None:
    """Field that ends with a lone backslash (escape with no escapee)."""
    body = b"1\tfoo\\"
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(body)))
    h.end_headers()
    h.wfile.write(body)


def _wrong_content_length(h: _RawHandler) -> None:
    """Content-Length larger than the body we actually write."""
    body = b"1\tshort\n"
    h.send_response(200)
    h.send_header("Content-Type", "text/tab-separated-values; charset=UTF-8")
    h.send_header("Content-Length", str(len(body) + 64))
    h.end_headers()
    h.wfile.write(body)
    # Close before satisfying the announced length.
    try:
        h.connection.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    h.connection.close()


BEHAVIORS: dict[str, Callable[[_RawHandler], None]] = {
    "happy": _happy,
    "http_500": _http_500,
    "http_503": _http_503,
    "invalid_array": _invalid_array,
    "truncated_chunked": _truncated_chunked,
    "slow_headers": _slow_headers,
    "invalid_json": _invalid_json,
    "drop_during_body": _drop_during_body,
    "trailing_garbage": _trailing_garbage,
    "unterminated_string": _unterminated_string,
    "backslash_at_eof": _backslash_at_eof,
    "wrong_content_length": _wrong_content_length,
}


class MockClickHouseServer(ThreadingHTTPServer):
    """Threaded HTTP server with a record of the behaviors it has served."""

    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], slow_seconds: float = 0.5):
        super().__init__(address, _RawHandler)
        self.behaviors_seen: list[str] = []
        self.slow_seconds = slow_seconds

    @classmethod
    def start(
        cls, host: str = "127.0.0.1", port: int = 0, slow_seconds: float = 0.5
    ) -> "MockClickHouseServer":
        """Start a server in a background thread and return it.

        Use ``server.server_address`` to discover the assigned port when
        ``port=0``.
        """
        server = cls((host, port), slow_seconds=slow_seconds)
        thread = threading.Thread(
            target=server.serve_forever, name="mock-clickhouse", daemon=True
        )
        thread.start()
        server._thread = thread  # type: ignore[attr-defined]
        return server

    def stop(self) -> None:
        self.shutdown()
        self.server_close()


def _main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=18123)
    parser.add_argument(
        "--slow-seconds",
        type=float,
        default=0.5,
        help="delay used by the slow_headers behavior",
    )
    args = parser.parse_args()
    server = MockClickHouseServer(
        (args.host, args.port), slow_seconds=args.slow_seconds
    )
    print(
        f"mock_clickhouse listening on http://{args.host}:{server.server_address[1]}/"
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(_main())
