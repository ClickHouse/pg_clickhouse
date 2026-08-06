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

const char*
ch_binary_exception_message(const chc_exception* ex) {
    if (ex) {
        if (ex->display_text && ex->display_text[0]) {
            return ex->display_text;
        }
        if (ex->name && ex->name[0]) {
            return ex->name;
        }
    }
    return "server exception";
}

void
ch_binary_drain(ch_binary_connection_t* conn, char** out_msg) {
    if (out_msg) {
        *out_msg = NULL;
    }

    for (;;) {
        chc_packet pkt = {};
        chc_err err    = {};

        if (chc_client_recv_packet(conn->client, &pkt, &err) != CHC_OK) {
            conn->broken = true;
            if (out_msg) {
                *out_msg = pstrdup(err.msg[0] ? err.msg : "recv_packet failed");
            }
            chc_packet_clear(conn->client, &pkt);
            return;
        }

        switch (pkt.kind) {
        case CHC_PKT_EXCEPTION:
            /* Server usually closes socket after raising, next op would EPIPE */
            conn->broken = true;
            if (out_msg) {
                *out_msg = pstrdup(ch_binary_exception_message(pkt.exception));
            }
            break;
        case CHC_PKT_END_OF_STREAM:
            break;
        default:
            chc_packet_clear(conn->client, &pkt);
            continue;
        }

        chc_packet_clear(conn->client, &pkt);
        return;
    }
}
