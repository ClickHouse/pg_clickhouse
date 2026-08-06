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

/*
 * Pump next Data block off wire (header block may carry zero rows). Caller
 * takes ownership, destroys via chc_block_destroy with pgch_alloc. NULL
 * when stream ends (eos, error, canceled), ch_binary_response_error reports
 * cause if any.
 */
extern const chc_block*
ch_binary_response_fetch_next_block(ch_binary_response_t* resp);

/*
 * This structure stores state for the opaque ch_binary_connection_t.
 *
 * It lives in its own context under CacheMemoryContext, so the connection
 * survives transaction boundaries. Result block buffers use
 * CurrentMemoryContext and therefore live in the caller's query context.
 */
struct ch_binary_connection_t {
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

/*
 * Limits blocking reads in the active I/O backend. A zero deadline removes
 * the limit.
 */
extern void
ch_binary_set_deadline(ch_binary_connection_t* conn, int64_t deadline_us);

/* True if server advertises output_format_native_write_json_as_string. */
extern bool
server_supports_json_as_string(const chc_client* c);

/*
 * Collect settings for query, adding session settings before driver settings.
 * Store number of settings in *n_settings and return NULL when list is empty.
 */
extern chc_query_setting*
ch_binary_query_settings(
    const chc_client* c,
    const ch_query* query,
    size_t* n_settings
);

/* Returns memory owned by ex, which must be copied before clearing its packet. */
extern const char*
ch_binary_exception_message(const chc_exception* ex);

/*
 * Consumes packets until the server sends EOS or an exception. Stores a
 * palloc'd error message in out_msg when requested. Marks the connection as
 * broken after an exception or transport failure.
 */
extern void
ch_binary_drain(ch_binary_connection_t* conn, char** out_msg);

#endif /* PG_CLICKHOUSE_BINARY_INTERNAL_H */
