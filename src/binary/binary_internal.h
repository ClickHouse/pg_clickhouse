/*
 * binary_internal.h
 *
 * Cross-file shared state for the binary driver subdir. Not exposed via
 * src/include, anything outside src/binary should use binary.h.
 */

#ifndef PG_CLICKHOUSE_BINARY_INTERNAL_H
#define PG_CLICKHOUSE_BINARY_INTERNAL_H

#include "postgres.h"

#include <openssl/ssl.h>

#include "clickhouse-client.h"
#include "clickhouse-openssl.h"
#include "clickhouse-posix-io.h"
#include "clickhouse.h"

#include "binary.h"
#include "internal.h"

#if PG_VERSION_NUM < 180000
#define pg_noreturn pg_attribute_noreturn()
#endif

/*
 * Pump next Data block off wire (header block may carry zero rows). Caller
 * takes ownership, destroys via chc_block_destroy with pgch_alloc. NULL
 * when stream ends (eos, error, canceled), ch_binary_response_error reports
 * cause if any.
 */
extern const chc_block*
ch_binary_response_fetch_next_block(ch_binary_response_t* resp);

extern ch_binary_insert_handle*
ch_binary_begin_insert(ch_binary_connection_t* conn, const ch_query* query);

/*
 * Tear down handle. Never raises and never talks to server, safe to call
 * from a MemoryContext reset callback during transaction abort. Flags
 * connection broken if finalize did not run.
 */
extern void
ch_binary_release_insert(ch_binary_insert_handle* h);

/*
 * Per-connection state smuggled through ch_binary_connection_t.client.
 *
 * Lives in own context `cxt` (child of CacheMemoryContext) so connection
 * survives transaction boundaries. Result block buffers don't live here:
 * pgch_alloc routes through CurrentMemoryContext, so blocks land in
 * whichever per-query context the caller has switched to.
 */
struct ch_binary_state {
    /*
     * Connection-lifetime context; holds this struct, the chc_client, and
     * chc_client's initial read buffer. Deleted by ch_binary_close.
     */
    MemoryContext cxt;

    /*
     * Registered on cxt; closes fd / SSL on reset so half-built connections
     * release OS resources when PG_CATCH deletes cxt.
     */
    MemoryContextCallback reset_cb;

    chc_client* client;
    /* Transport vtable; backed by posix_state or openssl_state below. */
    chc_io io;

    /* Compression codec used by chc_client. */
    chc_codec codec;

    int fd; /* -1 once closed by reset_cb */
    SSL_CTX* ssl_ctx;
    SSL* ssl;
    bool tls;
    chc_posix_io posix_state;
    chc_openssl_io openssl_state;

    /* Per-query cancel callback (no userdata). */
    bool (*check_cancel_fn)(void);

    /*
     * Set when an unrecoverable protocol/IO error happened (server raised an
     * exception mid-INSERT and closed the socket, write hit EPIPE, etc).
     * Cache layer checks via ch_binary_is_broken & drops the entry.
     */
    bool broken;
};

static inline struct ch_binary_state*
conn_state(ch_binary_connection_t* conn) {
    return (struct ch_binary_state*)conn->client;
}

/* True if server advertises output_format_native_write_json_as_string. */
extern bool
server_supports_json_as_string(const chc_client* c);

/* Column buffers behind the handle; encode.c appends through this. */
extern pgch_writer*
ch_binary_insert_writer(ch_binary_insert_handle* h);

extern size_t
ch_binary_insert_ncols(const ch_binary_insert_handle* h);

/* Name the server gave column i of the INSERT, "" when unnamed. */
extern const char*
ch_binary_insert_column_name(const ch_binary_insert_handle* h, size_t i);

/* Send buffered rows and clear; ready for next batch. */
extern void
ch_binary_flush_block(ch_binary_insert_handle* h);

#endif /* PG_CLICKHOUSE_BINARY_INTERNAL_H */
