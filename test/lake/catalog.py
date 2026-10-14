"""
Read-only Iceberg REST catalog serving each directory under root holding a
metadata directory as a table, its parent directories as namespaces.

Usage: catalog.py PORT ROOT

ROOT is a warehouse directory seeded by seed.sh. ClickHouse must see its
contents at identical absolute paths, including when running in Docker.

Implements the subset of the REST spec ClickHouse DataLakeCatalog reads:
https://github.com/apache/iceberg/blob/main/open-api/rest-catalog-open-api.yaml
"""

import http.server
import json
import pathlib
import sys
import urllib.parse

# Separates namespace levels in paths and the parent parameter
SEP = "\x1f"

root = pathlib.Path(sys.argv[2]).resolve()


def tables():
    for meta in sorted(root.glob("**/metadata")):
        *ns, name = meta.parent.relative_to(root).parts
        yield tuple(ns), name, meta


def version(path):
    return int(path.name.removeprefix("v").split(".")[0])


def load(meta):
    current = max(meta.glob("v*.metadata.json"), key=version)
    body = json.loads(current.read_text())
    # ClickHouse picks storage by location scheme
    body["location"] = meta.parent.as_uri()
    return {"metadata-location": current.as_uri(), "metadata": body}


class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, body, code=200):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        path = [urllib.parse.unquote(p) for p in url.path.split("/") if p]
        query = urllib.parse.parse_qs(url.query)

        match path:
            case ["v1", "config"]:
                return self.reply({"defaults": {}, "overrides": {}})
            case ["v1", "namespaces"]:
                parent = tuple(query["parent"][0].split(SEP)) if "parent" in query else ()
                children = {
                    ns[: len(parent) + 1]
                    for ns, _, _ in tables()
                    if len(ns) > len(parent) and ns[: len(parent)] == parent
                }
                return self.reply({"namespaces": [list(ns) for ns in sorted(children)]})
            case ["v1", "namespaces", ns, "tables"]:
                ns = tuple(ns.split(SEP))
                return self.reply(
                    {"identifiers": [{"namespace": list(n), "name": t} for n, t, _ in tables() if n == ns]}
                )
            case ["v1", "namespaces", ns, "tables", name]:
                ns = tuple(ns.split(SEP))
                for n, t, meta in tables():
                    if (n, t) == (ns, name):
                        return self.reply(load(meta))
        self.reply({"error": {"message": self.path, "type": "NoSuchTableException", "code": 404}}, 404)


http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), Handler).serve_forever()
