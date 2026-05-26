/*
 * select.c
 *
 * Submit a SELECT (chc_client_send_query_ex) and drain Data packets into
 * a ch_binary_response_t that the FDW iterates in decode.c.
 * Settings come from the foreign-table KV list; we also add
 * output_format_native_write_json_as_string=1 against servers that
 * understand it. Cancel polling drives chc_io's per-read callback;
 * server-side exceptions flag the connection broken so the cache drops
 * it.
 */

#include "postgres.h"

#include <stdio.h>
#include <string.h>

#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary_internal.h"
#include "kv_list.h"

typedef struct response_block
{
	chc_block  *block;
}			response_block;

struct ch_binary_response_t
{
	MemoryContext cxt;
	bool		success;
	char	   *error;			/* NULL on success */

	size_t		columns_count;
	char	  **column_names;
	chc_type  **column_types;	/* borrowed pointers into blocks[0] */

	size_t		nblocks;
	size_t		cap_blocks;
	response_block *blocks;
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

static void
resp_push_block(ch_binary_response_t * resp, chc_block * blk)
{
	if (resp->nblocks + 1 > resp->cap_blocks)
	{
		size_t		ncap = resp->cap_blocks ? resp->cap_blocks * 2 : 4;
		size_t		bytes = ncap * sizeof(*resp->blocks);

		resp->blocks = resp->blocks
			? repalloc(resp->blocks, bytes)
			: palloc(bytes);
		resp->cap_blocks = ncap;
	}
	resp->blocks[resp->nblocks++].block = blk;
}

static void
resp_capture_schema(ch_binary_response_t * resp, chc_block * blk)
{
	if (resp->columns_count)
		return;
	size_t		nc = chc_block_n_columns(blk);

	resp->columns_count = nc;
	if (nc == 0)
		return;
	resp->column_names = palloc0(nc * sizeof(char *));
	resp->column_types = palloc0(nc * sizeof(chc_type *));
	for (size_t i = 0; i < nc; i++)
	{
		size_t		nlen;
		const char *nm = chc_block_column_name(blk, i, &nlen);

		resp->column_names[i] = pnstrdup(nm ? nm : "", nlen);
		resp->column_types[i] = (chc_type *) chc_block_column_type(blk, i);
	}
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
	chc_err		err = {0};
	int			rc = chc_client_send_query_ex(s->client, query->sql,
											  strlen(query->sql), &opts, &err);

	if (rc != CHC_OK)
	{
		resp_set_error(resp, err.msg);
		s->broken = true;
		goto done;
	}

	for (;;)
	{
		chc_packet	pkt = {0};

		err = (chc_err)
		{
			0
		};
		rc = chc_client_recv_packet(s->client, &pkt, &err);
		if (rc != CHC_OK)
		{
			resp_set_error(resp, err.msg);
			s->broken = true;
			break;
		}
		if (check_cancel && check_cancel() && !resp->error)
			resp_set_error(resp, "query was canceled");
		switch (pkt.kind)
		{
			case CHC_PKT_DATA:
				if (pkt.block && chc_block_n_columns(pkt.block) > 0)
				{
					resp_capture_schema(resp, pkt.block);
					if (chc_block_n_rows(pkt.block) > 0)
					{
						resp_push_block(resp, pkt.block);
						pkt.block = NULL;
					}
				}
				chc_packet_clear(s->client, &pkt);
				break;
			case CHC_PKT_EXCEPTION:
				resp_set_exception(resp, pkt.exception);
				chc_packet_clear(s->client, &pkt);

				/*
				 * Older servers (and some protocol states) close the socket
				 * after raising an exception, so reusing this connection for
				 * a follow-up query risks EPIPE. Match the legacy C++ driver
				 * (which always called Client::ResetConnection) and treat the
				 * connection as broken.
				 */
				s->broken = true;
				goto done;
			case CHC_PKT_END_OF_STREAM:
				chc_packet_clear(s->client, &pkt);
				goto done;
			default:
				chc_packet_clear(s->client, &pkt);
				break;
		}
		if (resp->error)
		{
			/*
			 * On user cancel, send a Cancel packet then drain the connection
			 * so the next query starts with a clean buffer. Disable cancel
			 * polling during drain — the io layer would otherwise
			 * short-circuit every refill on the still-pending
			 * QueryCancelPending.
			 */
			chc_err		ce = {0};

			(void) chc_client_send_cancel(s->client, &ce);
			s->check_cancel_fn = NULL;
			for (;;)
			{
				chc_packet	drain = {0};

				ce = (chc_err)
				{
					0
				};
				int			drc = chc_client_recv_packet(s->client, &drain, &ce);
				bool		eos = (drc == CHC_OK &&
								   (drain.kind == CHC_PKT_END_OF_STREAM ||
									drain.kind == CHC_PKT_EXCEPTION));

				chc_packet_clear(s->client, &drain);
				if (drc != CHC_OK || eos)
					break;
			}
			goto done;
		}
	}

done:
	s->check_cancel_fn = NULL;
	resp->success = (resp->error == NULL);
	MemoryContextSwitchTo(old);
	return resp;
}

void
ch_binary_response_free(ch_binary_response_t * resp)
{
	if (!resp)
		return;
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
ch_binary_response_block_count(const ch_binary_response_t * resp)
{
	return resp ? resp->nblocks : 0;
}

size_t
ch_binary_response_columns(const ch_binary_response_t * resp)
{
	return resp ? resp->columns_count : 0;
}

const chc_block *
ch_binary_response_block_at(ch_binary_response_t * resp, size_t idx)
{
	if (!resp || idx >= resp->nblocks)
		return NULL;
	return resp->blocks[idx].block;
}
