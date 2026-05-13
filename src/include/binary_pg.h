/*
 * binary_pg.h
 *
 * PG-typed surface for the binary driver. Lives outside binary.hh so the
 * C++ wire layer (binary.cpp) need not see Datum/Oid/MemoryContext. Callers
 * (pglink.c, convert.c, fdw.c) include this header instead of binary.hh.
 */

#ifndef PG_CLICKHOUSE_BINARY_PG_H
#define PG_CLICKHOUSE_BINARY_PG_H

#include "postgres.h"

#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary.hh"
#include "ch_block.h"
#include "engine.h"

typedef struct
{
	ch_binary_response_t *resp;
	Oid		   *coltypes;
	Datum	   *values;
	bool	   *nulls;

	size_t		block;			/* current block */
	size_t		row;			/* row in current block */
	ch_block	cur;			/* materialized current block */
	bool		cur_valid;
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
}			ch_binary_tuple_t;

/*
 * Holds an array decoded from ClickHouse or built for INSERT. For nested
 * arrays (Array(Array(...))) ndim > 1 and datums[i] points to a child
 * ch_binary_array_t with ndim-1. item_type is the leaf scalar PG type,
 * array_type the postgres array type (same across nesting depths since
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

/* SELECT helpers (binary_decode.c). */
extern void ch_binary_read_state_init(ch_binary_read_state_t * state, ch_binary_response_t * resp);
extern void ch_binary_read_state_free(ch_binary_read_state_t * state);
extern bool ch_binary_read_row(ch_binary_read_state_t * state);

/* SELECT/INSERT type conversion (convert.c). */
extern Datum ch_binary_convert_datum(void *state, Datum val);
extern void *ch_binary_init_convert_state(Datum val, Oid intype, Oid outtype);
extern void ch_binary_free_convert_state(void *state);

/* INSERT helpers (binary_encode.c). */
extern void ch_binary_prepare_insert(void *conn, const ch_query * query,
									 ch_binary_insert_state * state);
extern void ch_binary_insert_columns(ch_binary_insert_state * state);
extern void ch_binary_column_append_data(ch_binary_insert_state * state, size_t colidx);
extern void *ch_binary_make_tuple_map(TupleDesc indesc, TupleDesc outdesc, Oid relid);
extern void ch_binary_insert_state_free(void *c);
extern void ch_binary_do_output_conversion(ch_binary_insert_state * state,
										   TupleTableSlot * slot);

#endif							/* PG_CLICKHOUSE_BINARY_PG_H */
