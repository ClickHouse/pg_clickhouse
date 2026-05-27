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

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "utils/palloc.h"

#include "clickhouse.h"
#include "engine.h"

typedef struct ch_binary_connection_t ch_binary_connection_t;
typedef struct ch_binary_response_t ch_binary_response_t;
typedef struct ch_binary_insert_handle ch_binary_insert_handle;

/* Connection. */
extern ch_binary_connection_t * ch_binary_connect(ch_connection_details * details);
extern void ch_binary_close(ch_binary_connection_t * conn);

/*
 * Returns true if connection encountered an unrecoverable error
 * (server exception, IO failure, mid-protocol break). Callers should
 * drop cached connection instead of reusing it.
 */
extern bool ch_binary_is_broken(const ch_binary_connection_t * conn);

/* SELECT. */
extern ch_binary_response_t * ch_binary_simple_query(ch_binary_connection_t * conn,
													 const ch_query * query,
													 bool (*check_cancel) (void));
extern void ch_binary_response_free(ch_binary_response_t * resp);
extern const char *ch_binary_response_error(const ch_binary_response_t * resp);
extern bool ch_binary_response_success(const ch_binary_response_t * resp);
extern size_t ch_binary_response_columns(const ch_binary_response_t * resp);

/* INSERT. */

/*
 * Finish the insert: send final empty Data, drain the response, ereport
 * if the server raised. Idempotent; call exactly once from the FDW happy
 * path before tearing down the handle.
 */
extern void ch_binary_finalize_insert(ch_binary_insert_handle * h);

/* PG-typed surface follows. */

typedef struct
{
	ch_binary_response_t *resp;
	Oid		   *coltypes;
	Datum	   *values;
	bool	   *nulls;

	size_t		block;			/* current block */
	size_t		row;			/* row in current block */
	const		chc_block *cur; /* borrowed from resp; NULL when unloaded */
	void	   *gc;				/* allocated objects while reading */
	char	   *error;
	bool		done;
}			ch_binary_read_state_t;

typedef struct
{
	MemoryContext memcxt;		/* used for cleanup */
	MemoryContextCallback callback;

	TupleDesc	outdesc;
	ch_binary_insert_handle *insert_block;
	size_t		len;
	void	   *conversion_states;
	char	   *table_name;
	Oid			relid;			/* foreign table relid, for column_name
								 * lookups */

	Datum	   *values;
	bool	   *nulls;
	bool		success;

	ch_binary_connection_t *conn;
}			ch_binary_insert_state;

/* SELECT helpers (decode.c). */
extern void ch_binary_read_state_init(ch_binary_read_state_t * state, ch_binary_response_t * resp);
extern void ch_binary_read_state_free(ch_binary_read_state_t * state);
extern bool ch_binary_read_row(ch_binary_read_state_t * state);

/* SELECT/INSERT type conversion (convert.c). */
extern Datum ch_binary_convert_datum(void *state, Datum val);
extern void *ch_binary_init_convert_state(Datum val, Oid intype, Oid outtype);
extern void ch_binary_free_convert_state(void *state);

/* INSERT helpers (encode.c). */
extern void ch_binary_prepare_insert(void *conn, const ch_query * query,
									 ch_binary_insert_state * state);
extern void ch_binary_insert_columns(ch_binary_insert_state * state);
extern void ch_binary_column_append_data(ch_binary_insert_state * state, size_t colidx);
extern void *ch_binary_make_tuple_map(TupleDesc indesc, TupleDesc outdesc, Oid relid);
extern void ch_binary_insert_state_free(void *c);
extern void ch_binary_do_output_conversion(ch_binary_insert_state * state,
										   TupleTableSlot * slot);

#endif							/* CLICKHOUSE_BINARY_H */
