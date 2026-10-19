Vector Search Benchmark
=======================

[bench.py](bench.py) measures the recall and latency of pgvector nearest
neighbor searches through pg_clickhouse, with and without ClickHouse vector
similarity indexes. It uses the 1024-dimension English [Cohere Wikipedia
embeddings], which ClickHouse loads straight from Hugging Face, 100,000 rows
per file.

Setup
-----

The script requires:

*   Python 3 with [psycopg] 3
*   A Postgres database with the pgvector and pg_clickhouse extensions
*   A pg_clickhouse foreign server for ClickHouse 25.8 or later, with access
    to the internet

For example:

```sh
createdb bench
psql -d bench -c "CREATE EXTENSION vector; CREATE EXTENSION pg_clickhouse"
psql -d bench -c "CREATE SERVER bench_ch FOREIGN DATA WRAPPER clickhouse_fdw"
psql -d bench -c "CREATE USER MAPPING FOR CURRENT_USER SERVER bench_ch"
```

Usage
-----

```sh
./bench.py load --dsn dbname=bench
./bench.py run --dsn dbname=bench
```

The `load` command creates two ClickHouse tables and maps them as foreign
tables:

*   `bench.docs`, mapped as `bench_docs`, holds `--size` rows, 100,000 by
    default, in multiples of 100,000.
*   `bench.queries`, mapped as `bench_queries`, holds `--queries` query
    vectors, 1,000 by default, from the next file of the data set.

The `run` command first computes the exact top 10 rows for each query in
ClickHouse and caches them in `results/`. Then it runs each query through a
prepared statement:

```sql
PREPARE q(vector) AS SELECT id FROM bench_docs <filter> ORDER BY emb <=> $1 LIMIT 10;
```

It runs every combination of these options:

*   `--variants`: `exact` searches without an index. `hnsw_f32`,
    `hnsw_bf16`, `hnsw_i8`, and `hnsw_b1` each create a cosine index with
    that quantization, then drop it.
*   `--shapes`: `unfiltered`, `10pct` (`WHERE bucket < 10`), and `1pct`
    (`WHERE bucket = 0`).
*   `--candidates`: values for the ClickHouse setting
    `hnsw_candidate_list_size_for_search`, appended to
    `pg_clickhouse.session_settings`. Defaults to 16, 64, and 256.

After `--warmup` untimed queries, 100 by default, it prints a Markdown table
with the index that `EXPLAIN` reports, recall@10, and client-side latency
percentiles for each combination.

Expect filtered searches to show perfect recall at about the speed of an
exact search. `bucket` leads the primary key, so ClickHouse filters on it
first and compares the remaining rows exactly, whatever index `EXPLAIN`
names. Expect the `i8` and `b1` quantizations to lose recall that more
candidates do not recover.

  [Cohere Wikipedia embeddings]: https://huggingface.co/datasets/Cohere/wikipedia-2023-11-embed-multilingual-v3
  [psycopg]: https://www.psycopg.org/psycopg3/
