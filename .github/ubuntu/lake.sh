#!/bin/sh

# Seed an Iceberg warehouse under ClickHouse user_files, then serve it as an
# Iceberg REST catalog on port 8181 for test/sql/import_lake.sql. Requires
# ClickHouse 26.3 or later to create Iceberg tables; skips older versions.

set -e

src="$(cd "$(dirname "$0")/../.." && pwd)/test/lake"
dir=/var/lib/clickhouse/user_files/pg_clickhouse_lake

# seed.sh exits 77 when ClickHouse is too old
sh "$src/seed.sh" "$dir" || exit $(($? != 77))

printf "~~~~ Starting Iceberg catalog ~~~~\n"
python3 -I "$src/catalog.py" 8181 "$dir" > /tmp/pg_clickhouse_lake.log 2>&1 < /dev/null &
if ! curl -sf --retry 10 --retry-connrefused --retry-delay 1 http://127.0.0.1:8181/v1/config > /dev/null; then
    cat /tmp/pg_clickhouse_lake.log
    exit 1
fi
