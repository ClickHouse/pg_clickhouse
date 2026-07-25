/*
 * binary.c
 *
 * Core glue for the binary driver. Holds the CHC_IMPLEMENTATION and
 * PGCH_IMPLEMENTATION defines for the header-only clickhouse-c and
 * pg-clickhouse-c libraries; the other .c files in src/binary include the
 * same headers without those defines and link against the bodies emitted
 * here. pgch_in_alloc needs both in the same TU, so they stay together.
 *
 * API lives in src/include/binary.h. Driver internals shared between
 * connection.c / select.c / insert.c live in binary_internal.h
 * alongside this file.
 */

#include "postgres.h"

#include "utils/palloc.h"

#define CHC_IMPLEMENTATION
#define CHC_NO_ASYNC
#include "clickhouse-client.h"
#include "clickhouse-compression.h"
#include "clickhouse-openssl.h"
#include "clickhouse-posix-io.h"
#include "clickhouse.h"

#define PGCH_IMPLEMENTATION
#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#include "binary_internal.h"

/*
 * output_format_native_write_json_as_string exists on the server from
 * 24.10 onwards. Sending it as `important` against an older server would
 * fail the query, so gate.
 */
bool
server_supports_json_as_string(const chc_client* c) {
    const chc_server_info* info = chc_client_server_info(c);

    if (!info) {
        return false;
    }
    if (info->version_major > 24) {
        return true;
    }
    if (info->version_major == 24 && info->version_minor >= 10) {
        return true;
    }
    return false;
}
