/*
 * select.c
 *
 * Submit a SELECT (chc_client_send_query_ex) and pump Data packets one block
 * at a time. decode.c pulls the next block via
 * ch_binary_response_fetch_next_block when it exhausts the current one, so
 * peak memory is bounded by one block plus what postgres holds for the
 * current row. Settings come from the foreign-table KV list; we also add
 * output_format_native_write_json_as_string=1 against servers that understand
 * it. Cancel polling drives chc_io's per-read callback; server-side
 * exceptions flag the connection as broken so the cache drops it. Premature
 * close (eg LIMIT, error in decode, transaction abort) sends Cancel
 * + drains in ch_binary_response_free so the connection is reusable.
 */

#include "postgres.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary_internal.h"
#include "kv_list.h"

/* Wall-clock cap on drain_until_eos. A well-behaved server acknowledges
 * Cancel by flushing the in-flight block + EndOfStream within a second. */
#define DRAIN_DEADLINE_US (5 * 1000 * 1000)

struct ch_binary_response_t
{
	MemoryContext cxt;
	struct ch_binary_state *state;	/* parent connection state */
	chc_client *client;			/* recv_packet target */
	bool		(*check_cancel) (void);

	char	   *error;			/* NULL on success */
	bool		success;
	bool		eos;			/* END_OF_STREAM or exception seen */

	size_t		columns_count;
	chc_block  *staged;			/* next block to hand out; owned */
	chc_block  *prev;			/* last block handed out; freed at next fetch */
};

static void
resp_set_error(ch_binary_response_t * resp, const char *msg)
{
	if (resp->error)
		return;
	resp->error = pstrdup(msg && *msg ? msg : "?");
}

static void
resp_set_exception(ch_binary_response_t * resp, const chc_exception * ex)
{
	if (resp->error)
		return;
	const char *msg = NULL;

	if (ex)
	{
		if (ex->display_text && ex->display_text[0])
			msg = ex->display_text;
		else if (ex->name && ex->name[0])
			msg = ex->name;
	}
	resp->error = pstrdup(msg ? msg : "server exception");
}

/*
 * Read one wire packet under resp->cxt and fold it into the response. Sets
 * resp->staged (for non-empty Data), resp->columns_count (on first Data with
 * columns), resp->error (cancel/exception/transport), or resp->eos
 * (END_OF_STREAM / exception / transport failure). Other packet kinds
 * (progress, log, profile, ...) are silently consumed.
 */
static void
pump_one(ch_binary_response_t * resp)
{
	MemoryContext old = MemoryContextSwitchTo(resp->cxt);
	chc_packet	pkt = {};
	chc_err		err = {};
	int			rc = chc_client_recv_packet(resp->client, &pkt, &err);

	if (rc != CHC_OK)
	{
		resp_set_error(resp, err.msg);
		resp->state->broken = true;
		resp->eos = true;
		MemoryContextSwitchTo(old);
		return;
	}

	if (resp->check_cancel && resp->check_cancel())
		resp_set_error(resp, "query was canceled");

	switch (pkt.kind)
	{
		case CHC_PKT_DATA:
			if (pkt.block && chc_block_n_columns(pkt.block) > 0)
			{
				if (resp->columns_count == 0)
					resp->columns_count = chc_block_n_columns(pkt.block);
				if (chc_block_n_rows(pkt.block) > 0 && resp->staged == NULL)
				{
					size_t		ncols = chc_block_n_columns(pkt.block);
					chc_err		verr = {};
					int			vrc = CHC_OK;

					for (size_t i = 0; i < ncols; i++)
					{
						vrc = chc_column_validate(chc_block_column(pkt.block, i), &verr);
						if (vrc != CHC_OK)
							break;
					}
					if (vrc != CHC_OK)
					{
						resp_set_error(resp, verr.msg);
						resp->state->broken = true;
						resp->eos = true;
					}
					else
					{
						resp->staged = pkt.block;
						pkt.block = NULL;
					}
				}
			}
			chc_packet_clear(resp->client, &pkt);
			break;

		case CHC_PKT_EXCEPTION:
			resp_set_exception(resp, pkt.exception);
			chc_packet_clear(resp->client, &pkt);

			/*
			 * Older servers (and some protocol states) close the socket after
			 * raising an exception, so reusing this connection for a
			 * follow-up query risks EPIPE. Match the legacy C++ driver (which
			 * always called Client::ResetConnection) and treat the connection
			 * as broken.
			 */
			resp->state->broken = true;
			resp->eos = true;
			break;

		case CHC_PKT_END_OF_STREAM:
			chc_packet_clear(resp->client, &pkt);
			resp->eos = true;
			break;

		default:
			chc_packet_clear(resp->client, &pkt);
			break;
	}

	MemoryContextSwitchTo(old);
}

static int64_t
drain_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void
drain_set_deadline(struct ch_binary_state *s, int64_t deadline_us)
{
	if (s->tls)
		chc_openssl_io_set_deadline(&s->openssl_state, deadline_us);
	else
		chc_posix_io_set_deadline(&s->posix_state, deadline_us);
}

/*
 * Best-effort drain of any unconsumed stream so the connection is left clean
 * for the next query. Disables cancel polling while draining so
 * QueryCancelPending doesn't short-circuit every refill, and caps total wait
 * with an IO deadline so a peer that ignores Cancel doesn't cause hang. Sets
 * state->broken on transport failure / timeout / send_cancel failure so the
 * cache drops the connection.
 */
static void
drain_until_eos(ch_binary_response_t * resp)
{
	if (resp->eos)
		return;

	MemoryContext old = MemoryContextSwitchTo(resp->cxt);
	chc_err		ce = {};

	if (chc_client_send_cancel(resp->client, &ce) != CHC_OK)
	{
		resp->state->broken = true;
		resp->eos = true;
		MemoryContextSwitchTo(old);
		return;
	}
	resp->state->check_cancel_fn = NULL;
	resp->check_cancel = NULL;

	drain_set_deadline(resp->state, drain_now_us() + DRAIN_DEADLINE_US);

	for (;;)
	{
		chc_packet	pkt = {};
		int			rc;

		ce = (chc_err)
		{
			0
		};
		rc = chc_client_recv_packet(resp->client, &pkt, &ce);

		if (rc != CHC_OK)
		{
			resp->state->broken = true;
			resp->eos = true;
			break;
		}
		bool		stop = pkt.kind == CHC_PKT_END_OF_STREAM ||
			pkt.kind == CHC_PKT_EXCEPTION;

		chc_packet_clear(resp->client, &pkt);
		if (stop)
		{
			resp->eos = true;
			break;
		}
	}

	drain_set_deadline(resp->state, 0);
	MemoryContextSwitchTo(old);
}

ch_binary_response_t *
ch_binary_simple_query(ch_binary_connection_t * conn, const ch_query * query,
					   bool (*check_cancel) (void))
{
	struct ch_binary_state *s = conn_state(conn);
	MemoryContext cxt = AllocSetContextCreate(CurrentMemoryContext,
											  "pg_clickhouse binary response",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	ch_binary_response_t *resp = palloc0(sizeof(*resp));

	resp->cxt = cxt;
	resp->state = s;
	resp->client = s->client;
	resp->check_cancel = check_cancel;
	s->check_cancel_fn = check_cancel;

	size_t		n_user_settings = 0;
	size_t		n_params = (size_t) (query->num_params > 0 ? query->num_params : 0);
	chc_query_setting *settings = NULL;
	chc_query_param *params = NULL;
	bool		want_json_as_string = server_supports_json_as_string(s->client);

	{
		const		kv_list *kv = query->settings;

		if (kv)
			n_user_settings = (size_t) kv->length;
	}
	size_t		n_settings = n_user_settings + (want_json_as_string ? 1 : 0);

	if (n_settings)
	{
		settings = palloc0(n_settings * sizeof(*settings));
		size_t		i = 0;

		for (kv_iter it = new_kv_iter(query->settings); !kv_iter_done(&it); kv_iter_next(&it), i++)
		{
			settings[i].name = it.name;
			settings[i].value = it.value;
			settings[i].important = true;
		}
		if (want_json_as_string)
		{
			settings[i].name = "output_format_native_write_json_as_string";
			settings[i].value = "1";
			settings[i].important = true;
		}
	}
	if (n_params)
	{
		params = palloc0(n_params * sizeof(*params));
		for (size_t i = 0; i < n_params; i++)
		{
			char		nm[32];

			snprintf(nm, sizeof(nm), "p%zu", i + 1);
			params[i].name = pstrdup(nm);

			/*
			 * Quote & escape the value the way clickhouse-cpp's
			 * WriteQuotedString did: wrap in single quotes, replace inner
			 * specials with backslash-escapes the server's
			 * Field::restoreFromDump understands. Without escaping inner
			 * quotes the server stops parsing at the first `'` inside the
			 * value, which breaks Array(String) parameters whose CH literal
			 * already contains quoted elements.
			 */
			const char *raw = query->param_values[i];

			if (raw)
			{
				size_t		rlen = strlen(raw);
				size_t		cap = rlen * 4 + 3;
				char	   *dst = palloc(cap);
				size_t		o = 0;

				dst[o++] = '\'';
				for (size_t j = 0; j < rlen; j++)
				{
					unsigned char ch = (unsigned char) raw[j];

					switch (ch)
					{
						case '\0':
							dst[o++] = '\\';
							dst[o++] = 'x';
							dst[o++] = '0';
							dst[o++] = '0';
							break;
						case '\b':
							dst[o++] = '\\';
							dst[o++] = 'x';
							dst[o++] = '0';
							dst[o++] = '8';
							break;
						case '\t':
							dst[o++] = '\\';
							dst[o++] = 't';
							break;
						case '\n':
							dst[o++] = '\\';
							dst[o++] = 'n';
							break;
						case '\'':
							dst[o++] = '\\';
							dst[o++] = 'x';
							dst[o++] = '2';
							dst[o++] = '7';
							break;
						case '\\':
							dst[o++] = '\\';
							dst[o++] = '\\';
							break;
						default:
							dst[o++] = (char) ch;
					}
				}
				dst[o++] = '\'';
				dst[o] = '\0';
				params[i].value = dst;
			}
			else
				params[i].value = "'\\N'";
		}
	}

	chc_query_opts opts = {
		.settings = settings,
		.n_settings = n_settings,
		.params = params,
		.n_params = n_params,
	};
	chc_err		err = {};
	int			rc = chc_client_send_query_ex(s->client, query->sql,
											  strlen(query->sql), &opts, &err);

	if (rc != CHC_OK)
	{
		resp_set_error(resp, err.msg);
		s->broken = true;
		resp->eos = true;
		goto done;
	}

	/*
	 * Pump until schema is known so callers can call
	 * ch_binary_response_columns immediately. Empty result sets exit on eos
	 * with columns_count still set from the header Data block.
	 */
	while (resp->columns_count == 0 && !resp->eos && !resp->error)
		pump_one(resp);

done:
	resp->success = (resp->error == NULL);
	MemoryContextSwitchTo(old);
	return resp;
}

void
ch_binary_response_free(ch_binary_response_t * resp)
{
	if (!resp)
		return;

	/* Avoid raising from a MemoryContextResetCallback. */
	PG_TRY();
	{
		drain_until_eos(resp);
	}
	PG_CATCH();
	{
		FlushErrorState();
		resp->state->broken = true;
	}
	PG_END_TRY();

	resp->state->check_cancel_fn = NULL;
	MemoryContextDelete(resp->cxt);
}

const char *
ch_binary_response_error(const ch_binary_response_t * resp)
{
	return resp ? resp->error : NULL;
}

bool
ch_binary_response_success(const ch_binary_response_t * resp)
{
	return resp && resp->success;
}

size_t
ch_binary_response_columns(const ch_binary_response_t * resp)
{
	return resp ? resp->columns_count : 0;
}

const		chc_block *
ch_binary_response_fetch_next_block(ch_binary_response_t * resp)
{
	if (!resp)
		return NULL;

	if (resp->prev)
	{
		chc_block_destroy(resp->prev, &pg_chc_alloc);
		resp->prev = NULL;
	}

	while (resp->staged == NULL && !resp->eos && !resp->error)
		pump_one(resp);

	if (resp->staged == NULL)
		return NULL;

	resp->prev = resp->staged;
	resp->staged = NULL;
	return resp->prev;
}
