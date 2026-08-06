/*
 * binary.h
 *
 * This API provides native-protocol transport over TCP. pg-clickhouse-c
 * encodes and decodes rows, while this layer moves blocks. Each public entry
 * point allocates in the caller's CurrentMemoryContext and reports errors with
 * ereport. Returned objects own a dedicated MemoryContext that their cleanup
 * functions delete.
 */

#ifndef CLICKHOUSE_BINARY_H
#define CLICKHOUSE_BINARY_H

#include "postgres.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clickhouse.h"
#include "engine.h"
#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

typedef struct ch_binary_connection_t ch_binary_connection_t;
typedef struct ch_binary_response_t ch_binary_response_t;
typedef struct ch_binary_insert_handle ch_binary_insert_handle;

/* Connection. */
extern ch_binary_connection_t*
ch_binary_connect(ch_connection_details* details);
extern void
ch_binary_close(ch_binary_connection_t* conn);

/*
 * Returns true if connection encountered an unrecoverable error
 * (server exception, IO failure, mid-protocol break). Callers should
 * drop cached connection instead of reusing it.
 */
extern bool
ch_binary_is_broken(const ch_binary_connection_t* conn);

/*
 * Read the server version captured during the native-protocol handshake.
 * Writes 0 to all out-params when the version is unavailable.
 */
extern void
ch_binary_server_version(
    const ch_binary_connection_t* conn,
    int* major,
    int* minor,
    int* patch
);

/* SELECT. */
extern ch_binary_response_t*
ch_binary_simple_query(
    ch_binary_connection_t* conn,
    const ch_query* query,
    bool (*check_cancel)(void)
);
extern void
ch_binary_response_free(ch_binary_response_t* resp);
extern const char*
ch_binary_response_error(const ch_binary_response_t* resp);

/* The response must outlive the returned block source. */
extern pgch_block_source
ch_binary_response_block_source(ch_binary_response_t* resp);

/*
 * Starts an INSERT and reads its column schema from the server's initial empty
 * block. The caller appends rows through the handle's writer and calls
 * ch_binary_flush_block to send each block.
 */
extern ch_binary_insert_handle*
ch_binary_begin_insert(ch_binary_connection_t* conn, const ch_query* query);

extern pgch_writer*
ch_binary_insert_writer(ch_binary_insert_handle* h);

extern size_t
ch_binary_insert_ncols(const ch_binary_insert_handle* h);

/* Returns the server's name for column i, or an empty string when unnamed. */
extern const char*
ch_binary_insert_column_name(const ch_binary_insert_handle* h, size_t i);

/* Sends buffered rows as one block and clears the writer. */
extern void
ch_binary_flush_block(ch_binary_insert_handle* h);

/*
 * Finishes the INSERT by sending an empty Data packet and draining the
 * response. Reports server errors with ereport. This function is idempotent.
 */
extern void
ch_binary_finalize_insert(ch_binary_insert_handle* h);

/*
 * Releases the handle without contacting the server or reporting an error, so
 * a MemoryContext reset callback can call it during transaction abort. Marks
 * the connection as broken if finalization did not run.
 */
extern void
ch_binary_release_insert(ch_binary_insert_handle* h);

#endif /* CLICKHOUSE_BINARY_H */
