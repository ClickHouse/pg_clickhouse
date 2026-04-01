#include "postgres.h"

#include "http_internal.h"

#include "minicoro.h"

struct ch_http_streaming_state
{
	mco_coro   *co;
	ch_http_connection_t *conn;

	/* Owned resources that need cleanup */
	char	   *url;
	struct curl_slist *headers;
	curl_mime  *form;

	bool		transfer_done;	/* coroutine has finished */

	/* Rolling buffer: raw bytes from curl */
	char	   *buf;
	size_t		buf_alloc;
	size_t		buf_len;		/* total bytes in buf */
	size_t		batch_end;		/* offset past last complete row in batch */
	int			rows_ready;		/* complete rows accumulated */
	int			fetch_size;

	/* Error / status */
	long		http_status;
	char		errbuffer[CURL_ERROR_SIZE];
	char	   *error;
	bool		error_owned;
	double		pretransfer_time;
	double		total_time;
	char		query_id[37];
};

static const char http_streaming_oom_error[] = "out of memory";
static const char http_streaming_write_error[] = "could not buffer response";

static void
http_streaming_set_static_error(ch_http_streaming_state * st, const char *error)
{
	if (st->error)
		return;

	st->error = (char *) error;
	st->error_owned = false;
}

static void
http_streaming_set_owned_error(ch_http_streaming_state * st, const char *error)
{
	char	   *copy;

	if (st->error)
		return;

	if (!error)
	{
		http_streaming_set_static_error(st, http_streaming_write_error);
		return;
	}

	copy = strdup(error);
	if (!copy)
	{
		http_streaming_set_static_error(st, http_streaming_oom_error);
		return;
	}

	st->error = copy;
	st->error_owned = true;
}

static bool
http_streaming_resume(ch_http_streaming_state * st)
{
	mco_result	res;

	res = mco_resume(st->co);
	if (res == MCO_SUCCESS)
		return true;

	http_streaming_set_owned_error(st, mco_result_description(res));
	st->transfer_done = true;
	return false;
}

/*
 * Streaming write callback. Appends data to the rolling buffer and counts
 * complete TSV rows (terminated by '\n'). When fetch_size rows have been
 * accumulated, yields the coroutine back to the caller.
 */
static size_t
write_data_streaming(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t		realsize = size * nmemb;
	ch_http_streaming_state *st = userp;

	if (st->buf_len + realsize + 1 > st->buf_alloc)
	{
		size_t		newsize = st->buf_alloc * 2;
		char	   *newbuf;

		if (newsize < st->buf_len + realsize + 1)
			newsize = st->buf_len + realsize + 1;

		newbuf = realloc(st->buf, newsize);
		if (!newbuf)
		{
			http_streaming_set_static_error(st, http_streaming_oom_error);
			return CURL_WRITEFUNC_ERROR;
		}

		st->buf = newbuf;
		st->buf_alloc = newsize;
	}

	memcpy(st->buf + st->buf_len, contents, realsize);
	st->buf_len += realsize;
	st->buf[st->buf_len] = '\0';

	for (size_t i = st->buf_len - realsize; i < st->buf_len; i++)
	{
		if (st->buf[i] != '\n')
			continue;

		st->rows_ready++;
		st->batch_end = i + 1;
		if (st->rows_ready < st->fetch_size)
			continue;

		mco_yield(st->co);
		return realsize;
	}

	return realsize;
}

/*
 * Coroutine function: runs the entire curl_easy_perform, yielding whenever a
 * batch is ready.
 */
static void
http_streaming_coro(mco_coro * co)
{
	ch_http_streaming_state *st = mco_get_user_data(co);
	CURLcode	errcode;

	errcode = curl_easy_perform(st->conn->curl);
	if (errcode == CURLE_ABORTED_BY_CALLBACK)
		st->http_status = CH_HTTP_CANCELED_STATUS;
	else if (errcode == CURLE_WRITE_ERROR)
	{
		if (!st->error)
			http_streaming_set_static_error(st, http_streaming_write_error);
	}
	else if (errcode != CURLE_OK)
	{
		http_streaming_set_owned_error(st,
									   st->errbuffer[0] ?
									   st->errbuffer :
									   curl_easy_strerror(errcode));
	}

	ch_http_get_transfer_info(st->conn, &st->pretransfer_time,
							  &st->total_time, &st->http_status);

	if (st->buf_len > st->batch_end)
		st->batch_end = st->buf_len;

	st->transfer_done = true;
}

ch_http_streaming_state *
ch_http_begin_streaming(ch_http_connection_t * conn, const ch_query * query,
						int fetch_size)
{
	ch_http_streaming_state *st;
	mco_desc	desc;
	mco_result	res;

	st = calloc(sizeof(ch_http_streaming_state), 1);
	if (!st)
		return NULL;

	st->conn = conn;
	st->fetch_size = fetch_size > 0 ? fetch_size : 1;
	st->buf_alloc = CH_HTTP_STREAM_BUFFER_SIZE;
	st->buf = malloc(st->buf_alloc);
	if (!st->buf)
	{
		free(st);
		return NULL;
	}

	ch_http_generate_query_id(st->query_id);
	if (!ch_http_build_url(conn, query, st->query_id, &st->url))
	{
		ch_http_end_streaming(st);
		return NULL;
	}

	ch_http_set_common_options(conn, st->url, st->errbuffer,
							   write_data_streaming, st);
	ch_http_set_progress_options(conn, conn);
	if (!ch_http_build_headers(conn, &st->headers))
	{
		ch_http_end_streaming(st);
		return NULL;
	}
	if (st->headers)
		curl_easy_setopt(conn->curl, CURLOPT_HTTPHEADER, st->headers);
	if (!ch_http_set_post_data(conn->curl, query, &st->form))
	{
		ch_http_end_streaming(st);
		return NULL;
	}

	desc = mco_desc_init(http_streaming_coro, CH_MINICORO_STACK_SIZE);
	desc.user_data = st;
	res = mco_create(&st->co, &desc);
	if (res != MCO_SUCCESS)
	{
		http_streaming_set_owned_error(st, mco_result_description(res));
		st->transfer_done = true;
		return st;
	}

	if (!http_streaming_resume(st))
		return st;

	return st;
}

bool
ch_http_fetch_batch(ch_http_streaming_state * st)
{
	size_t		consumed;
	size_t		remaining;

	if (!st)
		return false;

	consumed = st->batch_end;
	remaining = st->buf_len - consumed;
	if (remaining > 0)
		memmove(st->buf, st->buf + consumed, remaining);
	st->buf_len = remaining;
	st->buf[st->buf_len] = '\0';
	st->batch_end = 0;
	st->rows_ready = 0;

	if (st->transfer_done || mco_status(st->co) == MCO_DEAD)
	{
		st->transfer_done = true;
		if (st->buf_len > 0)
		{
			st->batch_end = st->buf_len;
			return true;
		}
		return false;
	}

	if (!http_streaming_resume(st))
		return false;
	if (st->batch_end > 0)
		return true;
	if (st->transfer_done && st->buf_len > 0)
	{
		st->batch_end = st->buf_len;
		return true;
	}

	return false;
}

char	   *
ch_http_streaming_error(ch_http_streaming_state * st)
{
	return st ? st->error : NULL;
}

long
ch_http_streaming_status(ch_http_streaming_state * st)
{
	return st ? st->http_status : 0;
}

double
ch_http_streaming_request_time(ch_http_streaming_state * st)
{
	return st ? st->pretransfer_time : 0;
}

double
ch_http_streaming_total_time(ch_http_streaming_state * st)
{
	return st ? st->total_time : 0;
}

char	   *
ch_http_streaming_batch_data(ch_http_streaming_state * st)
{
	return st ? st->buf : NULL;
}

void
ch_http_streaming_init_read_state(ch_http_streaming_state * st,
								  ch_http_read_state * read_state)
{
	if (!st || !read_state)
		return;

	ch_http_read_state_init(read_state, st->buf, st->batch_end);
}

void
ch_http_end_streaming(ch_http_streaming_state * st)
{
	if (!st)
		return;

	if (st->co)
		mco_destroy(st->co);

	curl_easy_reset(st->conn->curl);

	if (st->url)
		curl_free(st->url);
	if (st->headers)
		curl_slist_free_all(st->headers);
	if (st->form)
		curl_mime_free(st->form);
	if (st->buf)
		free(st->buf);
	if (st->error && st->error_owned)
		free(st->error);

	free(st);
}
