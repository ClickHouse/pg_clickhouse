#ifndef CLICKHOUSE_BINARY_H
#define CLICKHOUSE_BINARY_H

#include "postgres.h"
#include "funcapi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ch_binary_connection_t ch_binary_connection_t;
typedef struct ch_binary_response_t
{
	void			   *values;
	size_t				columns_count;
	size_t				blocks_count;
	char			   *error;
	bool				success;
} ch_binary_response_t;

typedef struct {
	ch_binary_response_t	*resp;
	Oid		*coltypes;
	Datum	*values;
	bool	*nulls;

	size_t	block;		/* current block */
	size_t	row;		/* row in current block */
	void   *gc;			/* allocated objects while reading */
	char   *error;
	bool	done;
} ch_binary_read_state_t;

typedef struct {
	Datum	*datums;
	bool	*nulls;
	size_t	 len;
	Oid		*types;
} ch_binary_tuple_t;

typedef struct {
	Datum  *datums;
	bool   *nulls;
	size_t	len;
	Oid		item_type;	/* used on selects */
	Oid		array_type;	/* used on selects */
} ch_binary_array_t;

typedef struct {
	MemoryContext	memcxt;	/* used for cleanup */
	MemoryContextCallback callback;

	TupleDesc	outdesc;
	void   *columns;	/* std::vector */
	size_t	len;
	void  *conversion_states;
	char *table_name;

	Datum	*values;
	bool	*nulls;
	bool	 success;

	ch_binary_connection_t *conn;
} ch_binary_insert_state;

extern ch_binary_connection_t *ch_binary_connect(char *host, int port,
		char *database, char *user, char *password, char **error);
extern void ch_binary_close(ch_binary_connection_t *conn);
extern ch_binary_response_t *ch_binary_simple_query(ch_binary_connection_t *conn,
		const char *query, bool (*check_cancel)(void));
extern void ch_binary_response_free(ch_binary_response_t *resp);

/* reading */
void ch_binary_read_state_init(ch_binary_read_state_t *state, ch_binary_response_t *resp);
void ch_binary_read_state_free(ch_binary_read_state_t *state);
bool ch_binary_read_row(ch_binary_read_state_t *state);
Datum ch_binary_convert_datum(void *state, Datum val);
void *ch_binary_init_convert_state(Datum val, Oid intype, Oid outtype);
void ch_binary_free_convert_state(void *);

/* insertion */
void ch_binary_prepare_insert(void *conn, char *query,
		ch_binary_insert_state *state);
void ch_binary_insert_columns(ch_binary_insert_state *state);
void ch_binary_column_append_data(ch_binary_insert_state *state, size_t colidx);
void *ch_binary_make_tuple_map(TupleDesc indesc, TupleDesc outdesc);
void ch_binary_insert_state_free(void *c);
void ch_binary_do_output_convertion(ch_binary_insert_state *insert_state,
		TupleTableSlot *slot);

#ifdef __cplusplus
}
#endif

#endif
