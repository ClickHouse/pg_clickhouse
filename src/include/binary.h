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
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "clickhouse.h"
#include "engine.h"

typedef struct ch_binary_connection_t ch_binary_connection_t;
typedef struct ch_binary_response_t ch_binary_response_t;
typedef struct ch_binary_insert_handle ch_binary_insert_handle;

/* Column metadata returned from ch_binary_begin_insert. */
typedef struct ch_binary_column_info
{
	const char *name;
	const		chc_type *type; /* type unwrapped of Nullable + LowCardinality */
	bool		is_nullable;
}			ch_binary_column_info;

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

/*
 * Pump the next non-empty Data block off the wire. Returned pointer is
 * borrowed; valid until the next call to fetch_next_block or to
 * ch_binary_response_free. NULL when the stream ends (eos, error, or
 * canceled). After NULL, ch_binary_response_error reports the cause if
 * any.
 */
extern const chc_block *ch_binary_response_fetch_next_block(ch_binary_response_t * resp);

/* INSERT. */
extern ch_binary_insert_handle * ch_binary_begin_insert(ch_binary_connection_t * conn,
														const ch_query * query,
														ch_binary_column_info * *out_cols,
														size_t * out_n);

/*
 * Finish the insert: send final empty Data, drain the response, ereport
 * if the server raised. Idempotent; call exactly once from the FDW happy
 * path before tearing down the handle.
 */
extern void ch_binary_finalize_insert(ch_binary_insert_handle * h);

/*
 * Tear down the handle. Never raises and never talks to the server, so it
 * is safe to call from a MemoryContext reset callback during transaction
 * abort. Flags the connection broken if finalize did not run.
 */
extern void ch_binary_release_insert(ch_binary_insert_handle * h);

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
	Datum	   *datums;
	bool	   *nulls;
	size_t		len;
	Oid		   *types;
	const char *ch_type_name;
}			ch_binary_tuple_t;

/*
 * Holds an array decoded from ClickHouse or built for INSERT. For nested
 * arrays (Array(Array(...))) ndim > 1 and datums[i] points to a child
 * ch_binary_array_t with ndim-1. item_type is leaf scalar PG type,
 * array_type is postgres array type (same across nesting depths since
 * postgres uses one array type per element type regardless of ndim).
 */
typedef struct
{
	Datum	   *datums;
	bool	   *nulls;
	size_t		len;
	int			ndim;			/* nesting depth, >=1 */
	Oid			item_type;		/* leaf scalar PG type */
	Oid			array_type;		/* PG array type (same at every level) */
}			ch_binary_array_t;

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
