/*
 * binary.h
 *
 * API exposed by the binary driver. Allocates against caller's
 * CurrentMemoryContext at every public entry point and ereports on
 * error. Returned objects own a dedicated MemoryContext; explicit
 * *_free / *_close calls do MemoryContextDelete.
 */

#ifndef CLICKHOUSE_BINARY_H
#define CLICKHOUSE_BINARY_H

#include "postgres.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "utils/palloc.h"

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
extern bool
ch_binary_response_success(const ch_binary_response_t* resp);

/* INSERT. */

/*
 * Finish the insert: send final empty Data, drain the response, ereport
 * if the server raised. Idempotent; call exactly once from the FDW happy
 * path before tearing down the handle.
 */
extern void
ch_binary_finalize_insert(ch_binary_insert_handle* h);

/* PG-typed surface follows. Rows decode through pgch_reader from
 * pg-clickhouse-c; this layer only supplies the block stream. */

/* resp must outlive returned source. */
extern pgch_block_source
ch_binary_response_block_source(ch_binary_response_t* resp);

/* Slot attribute feeding one ClickHouse column, resolved on first tuple. */
typedef struct {
    AttrNumber attnum;
    Oid atttypid;
} ch_binary_insert_colmap;

typedef struct {
    MemoryContext memcxt; /* used for cleanup */
    MemoryContextCallback callback;

    ch_binary_insert_handle* insert_block;
    size_t len;                      /* ClickHouse column count */
    ch_binary_insert_colmap* colmap; /* len entries, NULL until first tuple */
    Oid relid;                       /* foreign table relid, for column_name lookups */
} ch_binary_insert_state;

/* INSERT helpers (encode.c). */
extern void
ch_binary_prepare_insert(
    void* conn,
    const ch_query* query,
    ch_binary_insert_state* state
);
extern void
ch_binary_insert_columns(ch_binary_insert_state* state);

/* Append one slot's worth of values, one per ClickHouse column. */
extern void
ch_binary_insert_tuple(ch_binary_insert_state* state, TupleTableSlot* slot);
extern void
ch_binary_insert_autoflush(ch_binary_insert_state* state);
extern void
ch_binary_insert_state_free(void* c);

#endif /* CLICKHOUSE_BINARY_H */
