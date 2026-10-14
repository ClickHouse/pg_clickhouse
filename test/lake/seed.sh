#!/bin/sh

set -e

src=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
if [ "$(clickhouse local --query "SELECT CAST(splitByChar('.', version()) AS Array(UInt32)) < [26, 3]")" = 1 ]; then
    printf 'Skipping Iceberg catalog: ClickHouse 26.3+ required\n'
    exit 77
fi

dir=${1:?Specify warehouse directory under ClickHouse user_files}
mkdir -p "$dir"
# clickhouse local confines file paths to working directory
cd "$dir"
clickhouse local --param_warehouse="$(pwd)" --queries-file "$src/seed.sql"
