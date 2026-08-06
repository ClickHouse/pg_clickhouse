/*
 * select.c
 *
 * Submit a SELECT (chc_client_send_query_ex) and pump Data packets one block
 * at a time. pgch_reader pulls the next block via
 * ch_binary_response_fetch_next_block when it exhausts the current one, so
 * peak memory is bounded by one block plus what postgres holds for the
 * current row. Queries include session and driver settings.
 * Cancel polling drives chc_io's per-read callback; server-side exceptions
 * flag the connection as broken so the cache drops it. Premature
 * close (eg LIMIT, error in decode, transaction abort) sends Cancel
 * + drains in ch_binary_response_free so the connection is reusable.
 */

#include "postgres.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary_internal.h"

/* Wall-clock cap on drain_until_eos. A well-behaved server acknowledges
 * Cancel by flushing the in-flight block + EndOfStream within a second. */
#define DRAIN_DEADLINE_US (5 * 1000 * 1000)

struct ch_binary_response_t {
    MemoryContext cxt;
    ch_binary_connection_t* conn;

    char* error; /* NULL on success */
    bool eos;    /* True after END_OF_STREAM or an exception. */

    chc_block* staged; /* next block to hand out; ownership passes to caller */
};

static void
resp_set_error(ch_binary_response_t* resp, const char* msg) {
    if (resp->error) {
        return;
    }
    resp->error = pstrdup(msg && *msg ? msg : "?");
}

/*
 * Read one wire packet under resp->cxt and fold it into the response. Sets
 * resp->staged (for Data with columns; header block may carry zero rows),
 * resp->error (cancel/exception/transport), or resp->eos (END_OF_STREAM /
 * exception / transport failure). Other packet kinds (progress, log,
 * profile, ...) are silently consumed.
 */
static void
pump_one(ch_binary_response_t* resp) {
    MemoryContext old = MemoryContextSwitchTo(resp->cxt);
    chc_packet pkt    = {};
    chc_err err       = {};
    int rc            = chc_client_recv_packet(resp->conn->client, &pkt, &err);

    if (rc != CHC_OK) {
        resp_set_error(resp, err.msg);
        resp->conn->broken = true;
        resp->eos          = true;
        MemoryContextSwitchTo(old);
        return;
    }

    if (resp->conn->check_cancel_fn && resp->conn->check_cancel_fn()) {
        resp_set_error(resp, "query was canceled");
    }

    switch (pkt.kind) {
    case CHC_PKT_DATA:
        if (pkt.block && chc_block_n_columns(pkt.block) > 0 && resp->staged == NULL) {
            size_t ncols = chc_block_n_columns(pkt.block);
            chc_err verr = {};
            int vrc      = CHC_OK;

            for (size_t i = 0; i < ncols; i++) {
                vrc = chc_column_validate(chc_block_column(pkt.block, i), &verr);
                if (vrc != CHC_OK) {
                    break;
                }
            }
            if (vrc != CHC_OK) {
                resp_set_error(resp, verr.msg);
                resp->conn->broken = true;
                resp->eos          = true;
            } else {
                resp->staged = pkt.block;
                pkt.block    = NULL;
            }
        }
        chc_packet_clear(resp->conn->client, &pkt);
        break;

    case CHC_PKT_EXCEPTION:
        resp_set_error(resp, ch_binary_exception_message(pkt.exception));
        chc_packet_clear(resp->conn->client, &pkt);

        /*
         * Older servers (and some protocol states) close the socket after
         * raising an exception, so reusing this connection for a
         * follow-up query risks EPIPE. Match the legacy C++ driver (which
         * always called Client::ResetConnection) and treat the connection
         * as broken.
         */
        resp->conn->broken = true;
        resp->eos          = true;
        break;

    case CHC_PKT_END_OF_STREAM:
        chc_packet_clear(resp->conn->client, &pkt);
        resp->eos = true;
        break;

    default:
        chc_packet_clear(resp->conn->client, &pkt);
        break;
    }

    MemoryContextSwitchTo(old);
}

static int64_t
drain_now_us(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/*
 * Best-effort drain of any unconsumed stream so the connection is left clean
 * for the next query. Disables cancel polling while draining so
 * QueryCancelPending doesn't short-circuit every refill, and caps total wait
 * with an IO deadline so a peer that ignores Cancel doesn't cause hang. Sets
 * conn->broken on transport failure / timeout / send_cancel failure so the
 * cache drops the connection.
 */
static void
drain_until_eos(ch_binary_response_t* resp) {
    if (resp->eos) {
        return;
    }

    MemoryContext old = MemoryContextSwitchTo(resp->cxt);
    chc_err ce        = {};

    resp->eos = true;
    if (chc_client_send_cancel(resp->conn->client, &ce) != CHC_OK) {
        resp->conn->broken = true;
        MemoryContextSwitchTo(old);
        return;
    }
    resp->conn->check_cancel_fn = NULL;

    ch_binary_set_deadline(resp->conn, drain_now_us() + DRAIN_DEADLINE_US);
    ch_binary_drain(resp->conn, NULL);
    ch_binary_set_deadline(resp->conn, 0);
    MemoryContextSwitchTo(old);
}

ch_binary_response_t*
ch_binary_simple_query(
    ch_binary_connection_t* conn,
    const ch_query* query,
    bool (*check_cancel)(void)
) {
    MemoryContext cxt = AllocSetContextCreate(
        CurrentMemoryContext, "pg_clickhouse binary response", ALLOCSET_DEFAULT_SIZES
    );
    MemoryContext old          = MemoryContextSwitchTo(cxt);
    ch_binary_response_t* resp = palloc0(sizeof(*resp));

    resp->cxt             = cxt;
    resp->conn            = conn;
    conn->check_cancel_fn = check_cancel;

    size_t n_params         = (size_t)(query->num_params > 0 ? query->num_params : 0);
    chc_query_param* params = NULL;
    size_t n_settings;
    chc_query_setting* settings =
        ch_binary_query_settings(conn->client, query, &n_settings);

    if (n_params) {
        params = palloc0(n_params * sizeof(*params));
        for (size_t i = 0; i < n_params; i++) {
            char nm[32];

            snprintf(nm, sizeof(nm), "p%zu", i + 1);
            params[i].name = pstrdup(nm);

            /*
             * Quote & escape the value the way clickhouse-cpp's
             * WriteQuotedString did: wrap in single quotes, replace inner
             * specials with backslash-escapes the server's
             * Field::restoreFromDump understands. Without escaping inner
             * quotes the server stops parsing at the first `'` inside the
             * value, which breaks Array(String) parameters whose CH literal
             * already contains quoted elements.
             */
            const char* raw = query->param_values[i];

            if (raw) {
                size_t rlen = strlen(raw);
                size_t cap  = rlen * 4 + 3;
                char* dst   = palloc(cap);
                size_t o    = 0;

                dst[o++] = '\'';
                for (size_t j = 0; j < rlen; j++) {
                    unsigned char ch = (unsigned char)raw[j];

                    switch (ch) {
                    case '\0':
                        dst[o++] = '\\';
                        dst[o++] = 'x';
                        dst[o++] = '0';
                        dst[o++] = '0';
                        break;
                    case '\b':
                        dst[o++] = '\\';
                        dst[o++] = 'x';
                        dst[o++] = '0';
                        dst[o++] = '8';
                        break;
                    case '\t':
                        dst[o++] = '\\';
                        dst[o++] = 't';
                        break;
                    case '\n':
                        dst[o++] = '\\';
                        dst[o++] = 'n';
                        break;
                    case '\'':
                        dst[o++] = '\\';
                        dst[o++] = 'x';
                        dst[o++] = '2';
                        dst[o++] = '7';
                        break;
                    case '\\':
                        dst[o++] = '\\';
                        dst[o++] = '\\';
                        break;
                    default:
                        dst[o++] = (char)ch;
                    }
                }
                dst[o++]        = '\'';
                dst[o]          = '\0';
                params[i].value = dst;
            } else {
                params[i].value = "'\\N'";
            }
        }
    }

    chc_query_opts opts = {
        .settings   = settings,
        .n_settings = n_settings,
        .params     = params,
        .n_params   = n_params,
    };
    chc_err err = {};
    int rc      = chc_client_send_query_ex(
        conn->client, query->sql, strlen(query->sql), &opts, &err
    );

    if (rc != CHC_OK) {
        resp_set_error(resp, err.msg);
        conn->broken = true;
        resp->eos    = true;
        MemoryContextSwitchTo(old);
        return resp;
    }

    /*
     * Pump until first Data block or eos so query-time exceptions surface
     * before cursor construction. Header block carries schema even for
     * empty result sets.
     */
    while (resp->staged == NULL && !resp->eos && !resp->error) {
        pump_one(resp);
    }

    MemoryContextSwitchTo(old);
    return resp;
}

void
ch_binary_response_free(ch_binary_response_t* resp) {
    if (!resp) {
        return;
    }

    /* Avoid raising from a MemoryContextResetCallback. */
    PG_TRY();
    { drain_until_eos(resp); }
    PG_CATCH();
    {
        FlushErrorState();
        resp->conn->broken = true;
    }
    PG_END_TRY();

    resp->conn->check_cancel_fn = NULL;
    MemoryContextDelete(resp->cxt);
}

const char*
ch_binary_response_error(const ch_binary_response_t* resp) {
    return resp ? resp->error : NULL;
}

/* Adapt TCP response to shared block source. */
static const chc_block*
resp_src_next_block(void* ud) {
    return ch_binary_response_fetch_next_block((ch_binary_response_t*)ud);
}

static const char*
resp_src_error(void* ud) {
    return ch_binary_response_error((ch_binary_response_t*)ud);
}

pgch_block_source
ch_binary_response_block_source(ch_binary_response_t* resp) {
    return (pgch_block_source){
        .ud         = resp,
        .next_block = resp_src_next_block,
        .error      = resp_src_error,
    };
}

const chc_block*
ch_binary_response_fetch_next_block(ch_binary_response_t* resp) {
    chc_block* blk;

    if (!resp) {
        return NULL;
    }

    while (resp->staged == NULL && !resp->eos && !resp->error) {
        pump_one(resp);
    }

    blk          = resp->staged;
    resp->staged = NULL;
    return blk;
}
