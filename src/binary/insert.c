/*
 * insert.c
 *
 * Server-facing half of the binary INSERT: enter insert mode, take the
 * schema from the server's initial empty Data block, ship blocks, finish.
 * Row buffering is pg-clickhouse-c's pgch_writer, built over the same
 * chc_type tree the server sent.
 */

#include "postgres.h"

#include <string.h>

#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary_internal.h"

struct ch_binary_insert_handle {
    MemoryContext cxt;
    chc_client* client;
    struct ch_binary_state* state; /* parent connection; used to flag broken
                                    * state on error */
    chc_block* initial_block;      /* schema source (server's empty Data) */
    size_t ncols;
    char** names;   /* column names, borrowed by writer's pgch_col */
    pgch_writer* w; /* row buffers, one per column */
    bool started;
    bool finalized; /* finalize_insert has run (success or raised) */
};

static void
recv_initial_block(struct ch_binary_state* s, ch_binary_insert_handle* h) {
    for (;;) {
        chc_packet pkt = {};
        chc_err err    = {};
        int rc         = chc_client_recv_packet(s->client, &pkt, &err);

        if (rc != CHC_OK) {
            s->broken = true;
            pgch_raise(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ");
        }
        if (pkt.kind == CHC_PKT_EXCEPTION) {
            const char* msg = "server exception";

            if (pkt.exception && pkt.exception->display_text) {
                msg = pkt.exception->display_text;
            } else if (pkt.exception && pkt.exception->name) {
                msg = pkt.exception->name;
            }
            char* msg_copy = pstrdup(msg);

            s->broken = true;
            chc_packet_clear(s->client, &pkt);
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_ERROR),
                errmsg("pg_clickhouse: could not prepare insert - %s", msg_copy)
            );
        }
        if (pkt.kind == CHC_PKT_DATA && pkt.block &&
            chc_block_n_columns(pkt.block) > 0) {
            h->initial_block = pkt.block;
            pkt.block        = NULL;
            chc_packet_clear(s->client, &pkt);
            return;
        }
        chc_packet_clear(s->client, &pkt);
    }
}

/*
 * After pgch_writer_new ereports the server is mid-INSERT awaiting our
 * Data; send empty Data + drain so the connection stays usable.
 */
static void
drain_aborted_insert(struct ch_binary_state* s) {
    chc_err ce = {};

    (void)chc_client_send_data(s->client, NULL, &ce);
    for (;;) {
        chc_packet drain = {};

        ce      = (chc_err){ 0 };
        int drc = chc_client_recv_packet(s->client, &drain, &ce);
        bool eos =
            (drc == CHC_OK &&
             (drain.kind == CHC_PKT_END_OF_STREAM || drain.kind == CHC_PKT_EXCEPTION));

        chc_packet_clear(s->client, &drain);
        if (drc != CHC_OK || eos) {
            break;
        }
    }
}

ch_binary_insert_handle*
ch_binary_begin_insert(ch_binary_connection_t* conn, const ch_query* query) {
    struct ch_binary_state* s = conn_state(conn);

    /*
     * Parent h's cxt to the connection's cxt, not CurrentMemoryContext. The
     * caller registers a reset callback on a sibling context that drains the
     * insert via end_insert(h); if h lived under that sibling, MemoryContext
     * tree teardown would free h before the callback fired.
     */
    MemoryContext cxt = AllocSetContextCreate(
        s->cxt, "pg_clickhouse binary insert", ALLOCSET_DEFAULT_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ch_binary_insert_handle* h;
    volatile bool need_drain = false;

    PG_TRY();
    {
        h         = palloc0(sizeof(*h));
        h->cxt    = cxt;
        h->client = s->client;
        h->state  = s;

        /* Append " VALUES" so server enters insert mode. */
        size_t sql_len = strlen(query->sql);
        char* sql      = palloc(sql_len + 8);

        memcpy(sql, query->sql, sql_len);
        memcpy(sql + sql_len, " VALUES", 7);
        sql[sql_len + 7] = '\0';

        /*
         * On servers that support it (24.10+), tell server to serialize any
         * JSON columns using STRING wire format. INSERT path doesn't need
         * this, server reads the per-column version prefix the builder
         * writes, but we set it on the same packet for symmetry with the
         * SELECT path and so any RETURNING-style projection on top still
         * decodes.
         */
        chc_query_setting json_setting = {
            .name      = "output_format_native_write_json_as_string",
            .value     = "1",
            .important = true,
        };
        chc_query_opts insert_opts     = {};
        const chc_query_opts* opts_ptr = NULL;

        if (server_supports_json_as_string(s->client)) {
            insert_opts.settings   = &json_setting;
            insert_opts.n_settings = 1;
            opts_ptr               = &insert_opts;
        }

        chc_err err = {};
        int rc = chc_client_send_query_ex(s->client, sql, sql_len + 7, opts_ptr, &err);

        if (rc != CHC_OK) {
            s->broken = true;
            pgch_raise(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ");
        }

        recv_initial_block(s, h);

        /*
         * Server is now waiting our Data; failures past this point need an
         * empty-Data + drain so the connection stays usable.
         */
        need_drain = true;
        h->started = true;

        size_t nc      = chc_block_n_columns(h->initial_block);
        pgch_col* cols = nc ? palloc0(nc * sizeof(pgch_col)) : NULL;

        h->ncols = nc;
        h->names = nc ? palloc0(nc * sizeof(char*)) : NULL;
        for (size_t i = 0; i < nc; i++) {
            size_t nlen;
            const char* nm = chc_block_column_name(h->initial_block, i, &nlen);

            h->names[i]      = pnstrdup(nm ? nm : "", nlen);
            cols[i].name     = h->names[i];
            cols[i].name_len = nlen;
            cols[i].type     = chc_block_column_type(h->initial_block, i);
        }
        h->w = pgch_writer_new(cxt, cols, nc);
    }
    PG_CATCH();
    {
        if (need_drain) {
            drain_aborted_insert(s);
        }
        MemoryContextSwitchTo(old);
        MemoryContextDelete(cxt);
        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old);
    return h;
}

pgch_writer*
ch_binary_insert_writer(ch_binary_insert_handle* h) {
    return h->w;
}

size_t
ch_binary_insert_ncols(const ch_binary_insert_handle* h) {
    return h->ncols;
}

const char*
ch_binary_insert_column_name(const ch_binary_insert_handle* h, size_t i) {
    return i < h->ncols ? h->names[i] : "";
}

/*
 * Flush once insert buffered reaches 64MiB, so large COPY or INSERT SELECT
 * streams blocks instead of accumulating all rows in memory. Server coalesces
 * small blocks within one INSERT via min_insert_block_size_rows/bytes.
 */
void
ch_binary_insert_autoflush(ch_binary_insert_state* state) {
    ch_binary_insert_handle* h = state->insert_block;

    if (h && pgch_writer_bytes(h->w) >= 64 * 1024 * 1024) {
        ch_binary_flush_block(h);
    }
}

void
ch_binary_flush_block(ch_binary_insert_handle* h) {
    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    chc_err err       = {};
    int rc            = chc_client_send_data(h->client, pgch_writer_build(h->w), &err);

    if (rc != CHC_OK) {
        if (h->state) {
            h->state->broken = true;
        }
        pgch_raise(&err, ERRCODE_FDW_ERROR, "could not insert columns - ");
    }

    pgch_writer_reset(h->w);
    MemoryContextSwitchTo(old);
}

/*
 * Send final empty Data + drain. May ereport on server exception or
 * transport failure. Idempotent via h->finalized. Leaves h->cxt alive;
 * ch_binary_release_insert deletes it.
 */
void
ch_binary_finalize_insert(ch_binary_insert_handle* h) {
    if (!h || h->finalized) {
        return;
    }

    /*
     * Set early so an ereport(ERROR) below still leaves h in the "do not
     * touch the wire" state for the release callback.
     */
    h->finalized = true;

    if (!(h->started && h->client)) {
        return;
    }

    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    char* exc_msg     = NULL;
    bool broke        = false;
    chc_err err       = {};
    int rc            = chc_client_send_data(h->client, NULL, &err);

    if (rc != CHC_OK) {
        broke   = true;
        exc_msg = pstrdup(err.msg[0] ? err.msg : "send_data failed");
    } else {
        /* Drain until EOS or exception. */
        for (;;) {
            chc_packet pkt = {};

            err = (chc_err){ 0 };
            rc  = chc_client_recv_packet(h->client, &pkt, &err);
            if (rc != CHC_OK) {
                broke   = true;
                exc_msg = pstrdup(err.msg[0] ? err.msg : "recv_packet failed");
                chc_packet_clear(h->client, &pkt);
                break;
            }
            if (pkt.kind == CHC_PKT_EXCEPTION) {
                const char* msg = "server exception";

                if (pkt.exception && pkt.exception->display_text) {
                    msg = pkt.exception->display_text;
                } else if (pkt.exception && pkt.exception->name) {
                    msg = pkt.exception->name;
                }
                exc_msg = pstrdup(msg);
                broke   = true;
                chc_packet_clear(h->client, &pkt);
                break;
            }
            chc_packet_clear(h->client, &pkt);
            if (pkt.kind == CHC_PKT_END_OF_STREAM) {
                break;
            }
        }
    }

    /*
     * Server raised mid-INSERT and typically closes the socket; the next op
     * would EPIPE. Mark broken so the cache rebuilds.
     */
    if (broke && h->state) {
        h->state->broken = true;
    }

    MemoryContextSwitchTo(old);

    if (exc_msg) {
        /* exc_msg lives in h->cxt; copy into parent before raising. */
        char* parent_msg = pstrdup(exc_msg);

        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("pg_clickhouse: could not finish INSERT - %s", parent_msg)
        );
    }
}

/*
 * Teardown counterpart to finalize. Safe from a MemoryContext reset
 * callback: never raises, never talks to the server. If finalize did not
 * run (mid-query abort), flags the connection broken so it rebuilds on
 * next use.
 */
void
ch_binary_release_insert(ch_binary_insert_handle* h) {
    if (!h) {
        return;
    }

    if (!h->finalized && h->started && h->state) {
        h->state->broken = true;
    }

    MemoryContextDelete(h->cxt);
}
