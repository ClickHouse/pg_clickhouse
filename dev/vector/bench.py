#!/usr/bin/env python3
"""Measure pg_clickhouse vector search recall and latency.

Loads Cohere Wikipedia embeddings into ClickHouse, computes exact top-10
neighbors in ClickHouse, then runs the same searches through pg_clickhouse
with and without vector similarity indexes. See README.md.
"""

import argparse
import json
import statistics
import time
from pathlib import Path

import psycopg

DATASET = "https://huggingface.co/datasets/Cohere/wikipedia-2023-11-embed-multilingual-v3/resolve/main/en"
RESULTS = Path(__file__).parent / "results"

# Each shard holds 100,000 rows; queries come from the shard after the corpus
SHAPES = {
    "unfiltered": "",
    "10pct": "WHERE bucket < 10",
    "1pct": "WHERE bucket = 0",
}

# CREATE INDEX options of each indexed variant
VARIANTS = {
    "exact": None,
    "hnsw_f32": "quantization = 'f32', m = 16, ef_construction = 128",
    "hnsw_bf16": "quantization = 'bf16', m = 16, ef_construction = 128",
    "hnsw_i8": "quantization = 'i8', m = 16, ef_construction = 128",
    "hnsw_b1": "quantization = 'b1', m = 16, ef_construction = 128",
}


def perform(cur, server, sql):
    cur.execute("CALL clickhouse_perform(%s, %s)", (server, sql))


def shard_url(shard):
    return f"url('{DATASET}/{shard:04d}.parquet', Parquet)"


def load(cur, args):
    shards = args.size // 100_000
    perform(cur, args.server, "CREATE DATABASE IF NOT EXISTS bench")
    perform(cur, args.server, "DROP TABLE IF EXISTS bench.docs")
    perform(cur, args.server, "DROP TABLE IF EXISTS bench.queries")
    perform(cur, args.server, """
        CREATE TABLE bench.docs (id Int64, bucket UInt8, emb Array(Float32))
        ENGINE = MergeTree ORDER BY (bucket, id)""")
    perform(cur, args.server, """
        CREATE TABLE bench.queries (qid UInt32, emb Array(Float32))
        ENGINE = MergeTree ORDER BY qid""")
    started = time.perf_counter()
    for shard in range(shards):
        perform(cur, args.server, f"""
            INSERT INTO bench.docs
            SELECT reinterpretAsInt64(cityHash64(_id)), cityHash64(_id) % 100,
                   CAST(emb AS Array(Float32))
            FROM {shard_url(shard)} SETTINGS max_http_get_redirects = 10""")
    perform(cur, args.server, f"""
        INSERT INTO bench.queries
        SELECT rowNumberInAllBlocks(), CAST(emb AS Array(Float32))
        FROM {shard_url(shards)} LIMIT {args.queries}
        SETTINGS max_http_get_redirects = 10, max_threads = 1""")
    print(f"loaded {args.size} rows in {time.perf_counter() - started:.0f} s")

    cur.execute("DROP FOREIGN TABLE IF EXISTS bench_docs, bench_queries")
    cur.execute(f"""
        CREATE FOREIGN TABLE bench_docs (id bigint, bucket smallint, emb vector(1024))
        SERVER {args.server} OPTIONS (database 'bench', table_name 'docs')""")
    cur.execute(f"""
        CREATE FOREIGN TABLE bench_queries (qid int, emb vector(1024))
        SERVER {args.server} OPTIONS (database 'bench', table_name 'queries')""")


def queries(cur):
    cur.execute("SELECT emb::text FROM bench_queries ORDER BY qid")
    return [row[0] for row in cur.fetchall()]


def ground_truth(cur, args, shape, vectors):
    """Exact top-10 per query, computed in ClickHouse without indexes."""
    path = RESULTS / f"truth_{shape}_{args.size}_{len(vectors)}.json"
    if path.exists():
        return json.loads(path.read_text())
    truth = []
    for vec in vectors:
        cur.execute(
            "SELECT id FROM clickhouse_query(%s, %s) AS t(id bigint)",
            (args.server, f"""
                SELECT id FROM bench.docs {SHAPES[shape]}
                ORDER BY cosineDistance(emb, {vec}), id LIMIT 10
                SETTINGS use_skip_indexes = 0"""),
        )
        truth.append([row[0] for row in cur.fetchall()])
    RESULTS.mkdir(exist_ok=True)
    path.write_text(json.dumps(truth))
    return truth


def search(cur, shape, vectors, truth, warmup):
    """Return the index EXPLAIN reports, recall@10, and latencies in ms."""
    cur.execute("DEALLOCATE ALL")
    cur.execute(f"""
        PREPARE q(vector) AS SELECT id FROM bench_docs {SHAPES[shape]}
        ORDER BY emb <=> $1 LIMIT 10""")
    cur.execute("EXPLAIN (COSTS OFF) EXECUTE q(%s::vector)", (vectors[0],))
    plan = "\n".join(row[0] for row in cur.fetchall())
    index = next((l.split(":", 1)[1].strip() for l in plan.splitlines()
                  if "Vector Index:" in l), "not pushed down")
    for vec in vectors[:warmup]:
        cur.execute("EXECUTE q(%s::vector)", (vec,))
        cur.fetchall()
    hits, times = 0, []
    for vec, expected in zip(vectors, truth):
        started = time.perf_counter()
        cur.execute("EXECUTE q(%s::vector)", (vec,))
        ids = [row[0] for row in cur.fetchall()]
        times.append((time.perf_counter() - started) * 1000)
        hits += len(set(ids) & set(expected))
    return index, hits / (10 * len(vectors)), times


def run(cur, args):
    vectors = queries(cur)
    truth = {shape: ground_truth(cur, args, shape, vectors) for shape in args.shapes}
    cur.execute("SHOW pg_clickhouse.session_settings")
    base = cur.fetchone()[0]
    print("| Variant | Shape | Candidates | Index | Recall@10 | p50 ms | p95 ms | p99 ms | Mean ms |")
    print("|---|---|--:|---|--:|--:|--:|--:|--:|")
    for variant in args.variants:
        options = VARIANTS[variant]
        if options:
            started = time.perf_counter()
            cur.execute(
                f"CREATE INDEX bench_docs_emb ON bench_docs USING clickhouse "
                f"(emb vector_cosine_ops) WITH ({options})"
            )
            print(f"<!-- {variant} built in {time.perf_counter() - started:.0f} s -->")
        for candidates in args.candidates if options else [None]:
            settings = base
            if candidates:
                settings += f", hnsw_candidate_list_size_for_search {candidates}"
            cur.execute("SELECT set_config('pg_clickhouse.session_settings', %s, false)", (settings,))
            for shape in args.shapes:
                index, recall, times = search(cur, shape, vectors, truth[shape], args.warmup)
                q = statistics.quantiles(times, n=100)
                print(f"| {variant} | {shape} | {candidates or '-'} | {index} | {recall:.3f} "
                      f"| {q[49]:.1f} | {q[94]:.1f} | {q[98]:.1f} | {statistics.mean(times):.1f} |",
                      flush=True)
        if options:
            cur.execute("DROP INDEX bench_docs_emb")
    cur.execute("SELECT set_config('pg_clickhouse.session_settings', %s, false)", (base,))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["load", "run"])
    parser.add_argument("--dsn", default="", help="libpq connection string")
    parser.add_argument("--server", default="bench_ch", help="pg_clickhouse foreign server")
    parser.add_argument("--size", type=int, default=100_000, help="corpus rows, a multiple of 100000")
    parser.add_argument("--queries", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--variants", type=lambda s: s.split(","), default=list(VARIANTS))
    parser.add_argument("--shapes", type=lambda s: s.split(","), default=list(SHAPES))
    parser.add_argument("--candidates", type=lambda s: [int(c) for c in s.split(",")],
                        default=[16, 64, 256])
    args = parser.parse_args()
    # EXPLAIN EXECUTE takes no bind parameters, so bind on the client
    with psycopg.connect(args.dsn, autocommit=True, cursor_factory=psycopg.ClientCursor) as conn, \
            conn.cursor() as cur:
        cur.execute("LOAD 'pg_clickhouse'")
        (load if args.command == "load" else run)(cur, args)


if __name__ == "__main__":
    main()
