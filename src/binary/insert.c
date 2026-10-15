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
    ch_binary_connection_t* conn;
    chc_block* initial_block; /* Holds the schema from the server's empty block. */
    size_t ncols;
    char** names;   /* Holds names referenced by the writer's columns. */
    pgch_writer* w; /* Buffers rows by column. */
    bool finalized; /* Records whether finalization ran or raised an error. */
    ch_binary_insert_handle* next; /* Next open INSERT of the connection. */
};

static void
recv_initial_block(ch_binary_connection_t* conn, ch_binary_insert_handle* h) {
    for (;;) {
        chc_packet pkt = {};
        chc_err err    = {};
        int rc         = chc_client_recv_packet(conn->client, &pkt, &err);

        if (rc != CHC_OK) {
            conn->broken = true;
            pgch_raise(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ", NULL);
        }
        if (pkt.kind == CHC_PKT_EXCEPTION) {
            char* msg = pstrdup(ch_binary_exception_message(pkt.exception));

            conn->broken = true;
            chc_packet_clear(conn->client, &pkt);
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_ERROR),
                errmsg("pg_clickhouse: could not prepare insert - %s", msg)
            );
        }
        if (pkt.kind == CHC_PKT_DATA && pkt.block &&
            chc_block_n_columns(pkt.block) > 0) {
            h->initial_block = pkt.block;
            pkt.block        = NULL;
            chc_packet_clear(conn->client, &pkt);
            return;
        }
        chc_packet_clear(conn->client, &pkt);
    }
}

void
ch_binary_detach_inserts(ch_binary_connection_t* conn) {
    for (ch_binary_insert_handle* h = conn->inserts; h != NULL; h = h->next) {
        h->conn = NULL;
    }
    conn->inserts = NULL;
}

/* Fail if the connection closed during the INSERT, which cannot continue */
static void
check_open(const ch_binary_insert_handle* h) {
    if (!h->conn) {
        ereport(
            ERROR,
            errcode(ERRCODE_CONNECTION_FAILURE),
            errmsg("pg_clickhouse: connection closed during INSERT")
        );
    }
}

/*
 * After pgch_writer_new ereports the server is mid-INSERT awaiting our
 * Data; send empty Data + drain so the connection stays usable.
 */
static void
drain_aborted_insert(ch_binary_connection_t* conn) {
    chc_err ce = {};

    if (chc_client_send_data(conn->client, NULL, &ce) != CHC_OK) {
        conn->broken = true;
        return;
    }
    ch_binary_drain(conn, NULL);
}

ch_binary_insert_handle*
ch_binary_begin_insert(ch_binary_connection_t* conn, const ch_query* query) {
    /*
     * Allocate the handle's buffers under the connection context. Allocate the
     * handle itself in the caller's context, with the reset callback that
     * releases it, which can run after the connection is closed and its context
     * deleted. Allocating the buffers under that context would free them before
     * the callback ran.
     */
    MemoryContext cxt = AllocSetContextCreate(
        conn->cxt, "pg_clickhouse binary insert", ALLOCSET_DEFAULT_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ch_binary_insert_handle* h;
    volatile bool need_drain = false;

    PG_TRY();
    {
        h       = MemoryContextAllocZero(old, sizeof(*h));
        h->cxt  = cxt;
        h->conn = conn;

        /* Append " VALUES" so server enters insert mode. */
        size_t sql_len = strlen(query->sql);
        char* sql      = palloc(sql_len + 8);

        memcpy(sql, query->sql, sql_len);
        memcpy(sql + sql_len, " VALUES", 7);
        sql[sql_len + 7] = '\0';

        size_t n_settings;
        chc_query_setting* settings =
            ch_binary_query_settings(conn, query, &n_settings);
        chc_query_opts insert_opts = {
            .settings   = settings,
            .n_settings = n_settings,
        };

        chc_err err = {};
        int rc      = chc_client_send_query_ex(
            conn->client, sql, sql_len + 7, &insert_opts, &err
        );

        if (rc != CHC_OK) {
            conn->broken = true;
            pgch_raise(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ", NULL);
        }

        recv_initial_block(conn, h);

        /*
         * Server is now waiting our Data; failures past this point need an
         * empty-Data + drain so the connection stays usable.
         */
        need_drain = true;

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
            drain_aborted_insert(conn);
        }
        MemoryContextSwitchTo(old);
        MemoryContextDelete(cxt);
        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old);
    h->next       = conn->inserts;
    conn->inserts = h;
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

void
ch_binary_flush_block(ch_binary_insert_handle* h) {
    check_open(h);

    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    chc_err err       = {};
    int rc = chc_client_send_data(h->conn->client, pgch_writer_build(h->w), &err);

    if (rc != CHC_OK) {
        h->conn->broken = true;
        pgch_raise(&err, ERRCODE_FDW_ERROR, "could not insert columns - ", NULL);
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
    check_open(h);

    /*
     * Set early so an ereport(ERROR) below still leaves h in the "do not
     * touch the wire" state for the release callback.
     */
    h->finalized = true;

    MemoryContext old = MemoryContextSwitchTo(h->cxt);
    char* exc_msg     = NULL;
    chc_err err       = {};

    if (chc_client_send_data(h->conn->client, NULL, &err) != CHC_OK) {
        h->conn->broken = true;
        exc_msg         = pstrdup(err.msg[0] ? err.msg : "send_data failed");
    } else {
        /* Marks broken itself on exception or transport failure */
        ch_binary_drain(h->conn, &exc_msg);
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
    if (!h || !h->conn) {
        return;
    }

    if (!h->finalized) {
        h->conn->broken = true;
    }
    for (ch_binary_insert_handle** open = &h->conn->inserts; *open != NULL;
         open                           = &(*open)->next) {
        if (*open == h) {
            *open = h->next;
            break;
        }
    }
    MemoryContextDelete(h->cxt);
}
