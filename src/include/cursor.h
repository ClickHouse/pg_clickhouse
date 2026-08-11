/*
 * cursor.h
 *
 * Cursor for an open ClickHouse Native result
 *
 * Driver provides response and initializes reader. Cursor validates columns,
 * converts rows, and releases resources
 */

#ifndef CLICKHOUSE_CURSOR_H
#define CLICKHOUSE_CURSOR_H

#include "postgres.h"

#include "fdw.h"
#include "pg-clickhouse-decode.h"

typedef struct ch_cursor {
    MemoryContext memcxt; /* Delete this context to close cursor */
    MemoryContextCallback callback;

    pgch_reader reader;
    void* response; /* Driver response read by reader */
    void (*free_response)(void* response);
    /*
     * Report a driver error, such as cancellation, before reporting a decoder
     * error. Leave unset if driver needs no special handling
     */
    void (*raise_response_error)(ch_cursor* cursor);

    void* conn;
    char* query;
    double request_time;
    size_t columns_count;
    /* One conversion state and destination attribute per returned column */
    void** conversion_states;
    int* fill_dest;
} ch_cursor;

/* Describes driver response and how cursor reads it */
typedef struct ch_cursor_source {
    void* response;
    /* Initialize reader from blocks or byte chunks */
    void (*init_reader)(pgch_reader* reader, void* response);
    /* Accept NULL because error callback may release response first */
    void (*free_response)(void* response);
    /* Report driver-specific errors, or NULL if not needed */
    void (*raise_response_error)(ch_cursor* cursor);
} ch_cursor_source;

/*
 * Open cursor and take ownership of src->response. Cursor context releases
 * response even if opening fails. Delete cursor->memcxt to close cursor
 */
extern ch_cursor*
chfdw_cursor_open(void* conn, const ch_query* query, const ch_cursor_source* src);

/* Fetch next row for either driver */
extern Datum*
chfdw_cursor_fetch_row(ChFdwScanRowContext* ctx);

#endif /* CLICKHOUSE_CURSOR_H */
