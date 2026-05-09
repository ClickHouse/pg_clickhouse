#ifndef CLICKHOUSE_HTTP_H
#define CLICKHOUSE_HTTP_H

#include "postgres.h"
#include "nodes/pg_list.h"
#include "lib/stringinfo.h"
#include "engine.h"

#define CH_HTTP_QUERY_ID_LEN 37

/*
 * Synthetic statuses used by the HTTP transport to surface local cancellation
 * and libcurl transport failures through the existing response machinery.
 */
#define CH_HTTP_STATUS_OK 200L
#define CH_HTTP_STATUS_CANCELED 418L
#define CH_HTTP_STATUS_TRANSPORT_ERROR 419L

typedef struct ch_http_connection_t ch_http_connection_t;
typedef struct ch_http_response_t
{
	char	   *data;
	size_t		datasize;
	long		http_status;
	char		query_id[CH_HTTP_QUERY_ID_LEN];
	double		pretransfer_time;
	double		total_time;
}			ch_http_response_t;

typedef enum
{
	CH_CONT,
	CH_EOL,
	CH_EOF
}			ch_read_status;

typedef struct
{
	char	   *data;
	size_t		datalen;
	size_t		curpos;
	StringInfoData val;
	bool		done;
	bool		is_null;		/* set when the parser saw the wire NULL
								 * marker `\N` for the field just read */
}			ch_http_read_state;

typedef struct
{
	StringInfoData sql;
	char	   *sql_begin;		/* beginning part of constructed sql */
	List	   *target_attrs;	/* list of target attribute numbers */
	int			p_nums;			/* number of parameters to transmit */
	ch_http_connection_t *conn;
}			ch_http_insert_state;

void		ch_http_init(int verbose, uint32_t query_id_prefix);
void		ch_http_set_progress_func(void *progressfunc);
void	   *ch_http_get_progress_func(void);
long		ch_http_get_verbose(void);
ch_http_connection_t *ch_http_connection(ch_connection_details * details);
void		ch_http_close(ch_http_connection_t * conn);
ch_http_response_t *ch_http_simple_query(ch_http_connection_t * conn, const ch_query * query);
char	   *ch_http_last_error(void);

/* read */
void		ch_http_read_state_init(ch_http_read_state * state, char *data, size_t datalen);
int			ch_http_read_next(ch_http_read_state * state, bool is_array);
void		ch_http_response_free(ch_http_response_t * resp);

#endif							/* CLICKHOUSE_HTTP_H */
