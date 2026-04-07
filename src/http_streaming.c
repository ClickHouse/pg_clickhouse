/*-------------------------------------------------------------------------
 *
 * http_streaming.c
 *	  Streaming HTTP query driver for pg_clickhouse.
 *
 *	  Uses curl_multi + CURL_WRITEFUNC_PAUSE to receive ClickHouse HTTP
 *	  responses in row-aligned batches of approximately fetch_size bytes,
 *	  keeping memory proportional to batch size instead of full result set.
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <uuid/uuid.h>

#include "postgres.h"
#include "http.h"
#include "internal.h"
#include "http_streaming.h"
#include "kv_list.h"

#ifndef CURL_WRITEFUNC_ERROR
#define CURL_WRITEFUNC_ERROR 0xFFFFFFFF
#endif

#define DATABASE_HEADER "X-ClickHouse-Database"
#define INITIAL_BUF_SIZE (64 * 1024)

/* ----------------------------------------------------------------
 * HttpStream — opaque struct.
 * ----------------------------------------------------------------
 */
struct HttpStream
{
	/* Connection (borrowed, not owned) */
	ch_http_connection_t *conn;

	/* Owned CURL resources */
	CURL	   *curl;
	CURLM	   *multi;
	struct curl_slist *headers;
	curl_mime  *form;
	char	   *url;			/* allocated by curl_url_get, freed with
								 * curl_free */

	/* Stream buffer */
	char	   *buf;
	size_t		buf_allocated;
	size_t		write_pos;
	size_t		parse_pos;
	size_t		batch_end;
	int32		fetch_size;		/* approximate batch size in bytes */
	bool		paused;
	bool		started;
	bool		transfer_done;
	char		error_buffer[CURL_ERROR_SIZE];

	/* Public state readable via C accessors */
	long		http_status;
	char		query_id[CH_HTTP_QUERY_ID_LEN];
	double		pretransfer_time;
	double		total_time;
	char	   *error_msg;		/* strdup'd, freed with free() */
};

/* Forward declarations of static helpers */
static void setup_curl(HttpStream * stream, const ch_query * query);
static void capture_transfer_info(HttpStream * stream);
static void compact_buffer(HttpStream * stream);
static size_t find_batch_end(const HttpStream * stream);
static size_t write_callback(void *contents, size_t size, size_t nmemb,
							 void *userp);

/* ----------------------------------------------------------------
 * setup_curl — configure the CURL easy handle for this query.
 * Mirrors the setup portion of ch_http_simple_query() in http.c.
 * ----------------------------------------------------------------
 */
static void
setup_curl(HttpStream * stream, const ch_query * query)
{
	CURLU	   *cu = curl_url();
	char		temp_buf[512];

	/* Build URL with query_id and settings */
	curl_url_set(cu, CURLUPART_URL, stream->conn->base_url, 0);

	snprintf(temp_buf, sizeof(temp_buf), "query_id=%s", stream->query_id);
	curl_url_set(cu, CURLUPART_QUERY, temp_buf,
				 CURLU_APPENDQUERY | CURLU_URLENCODE);

	for (kv_iter iter = new_kv_iter(query->settings);
		 !kv_iter_done(&iter); kv_iter_next(&iter))
	{
		if (strcmp(iter.name, "date_time_output_format") == 0 ||
			strcmp(iter.name, "format_tsv_null_representation") == 0 ||
			strcmp(iter.name, "output_format_tsv_crlf_end_of_line") == 0)
			continue;
		snprintf(temp_buf, sizeof(temp_buf), "%s=%s",
				 iter.name, iter.value);
		curl_url_set(cu, CURLUPART_QUERY, temp_buf,
					 CURLU_APPENDQUERY | CURLU_URLENCODE);
	}

	curl_url_set(cu, CURLUPART_QUERY,
				 "date_time_output_format=iso",
				 CURLU_APPENDQUERY | CURLU_URLENCODE);
	curl_url_set(cu, CURLUPART_QUERY,
				 "format_tsv_null_representation=\\N",
				 CURLU_APPENDQUERY | CURLU_URLENCODE);
	curl_url_set(cu, CURLUPART_QUERY,
				 "output_format_tsv_crlf_end_of_line=0",
				 CURLU_APPENDQUERY | CURLU_URLENCODE);
	curl_url_get(cu, CURLUPART_URL, &stream->url, 0);
	curl_url_cleanup(cu);

	/* Configure CURL easy handle */
	curl_easy_setopt(stream->curl, CURLOPT_WRITEFUNCTION, write_callback);
	curl_easy_setopt(stream->curl, CURLOPT_WRITEDATA, stream);
	curl_easy_setopt(stream->curl, CURLOPT_ERRORBUFFER, stream->error_buffer);
	curl_easy_setopt(stream->curl, CURLOPT_PATH_AS_IS, 1L);
	curl_easy_setopt(stream->curl, CURLOPT_URL, stream->url);
	curl_easy_setopt(stream->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(stream->curl, CURLOPT_VERBOSE, ch_http_get_verbose());

	if (ch_http_get_progress_func())
	{
		curl_easy_setopt(stream->curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(stream->curl, CURLOPT_XFERINFOFUNCTION,
						 ch_http_get_progress_func());
		curl_easy_setopt(stream->curl, CURLOPT_XFERINFODATA, stream->conn);
	}
	else
		curl_easy_setopt(stream->curl, CURLOPT_NOPROGRESS, 1L);

	if (stream->conn->dbname)
	{
		snprintf(temp_buf, sizeof(temp_buf), "%s: %s",
				 DATABASE_HEADER, stream->conn->dbname);
		stream->headers = curl_slist_append(NULL, temp_buf);
		curl_easy_setopt(stream->curl, CURLOPT_HTTPHEADER, stream->headers);
	}

	/* POST body or MIME form */
	if (query->num_params == 0)
	{
		curl_easy_setopt(stream->curl, CURLOPT_POSTFIELDS, query->sql);
	}
	else
	{
		curl_mimepart *part;

		stream->form = curl_mime_init(stream->curl);
		part = curl_mime_addpart(stream->form);
		curl_mime_name(part, "query");
		curl_mime_data(part, query->sql, CURL_ZERO_TERMINATED);

		for (int i = 0; i < query->num_params; i++)
		{
			part = curl_mime_addpart(stream->form);
			snprintf(temp_buf, sizeof(temp_buf), "param_p%d", i + 1);
			curl_mime_name(part, temp_buf);
			curl_mime_data(part, query->param_values[i], CURL_ZERO_TERMINATED);
		}
		curl_easy_setopt(stream->curl, CURLOPT_MIMEPOST, stream->form);
	}
}

/* ----------------------------------------------------------------
 * write_callback — CURL write callback. Appends data to the
 * stream buffer and returns CURL_WRITEFUNC_PAUSE when buffered
 * data reaches fetch_size bytes.
 * ----------------------------------------------------------------
 */
static size_t
write_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t		realsize = size * nmemb;
	HttpStream *self = (HttpStream *) userp;
	size_t		needed;

	/*
	 * If we already have enough bytes, pause WITHOUT consuming this chunk.
	 * CURL will re-deliver the same data when resumed.
	 */
	if (self->write_pos >= (size_t) self->fetch_size)
	{
		self->paused = true;
		return CURL_WRITEFUNC_PAUSE;
	}

	needed = self->write_pos + realsize + 1;

	/* Grow buffer if needed */
	if (needed > self->buf_allocated)
	{
		size_t		newsize = self->buf_allocated * 2;
		char	   *newbuf;

		if (newsize < needed)
			newsize = needed;

		newbuf = (char *) realloc(self->buf, newsize);
		if (!newbuf)
			return CURL_WRITEFUNC_ERROR;

		self->buf = newbuf;
		self->buf_allocated = newsize;
	}

	memcpy(self->buf + self->write_pos, contents, realsize);
	self->write_pos += realsize;
	self->buf[self->write_pos] = '\0';

	return realsize;
}

/* ----------------------------------------------------------------
 * capture_transfer_info — grab HTTP status and timing from CURL.
 * ----------------------------------------------------------------
 */
static void
capture_transfer_info(HttpStream * stream)
{
	curl_easy_getinfo(stream->curl, CURLINFO_RESPONSE_CODE,
					  &stream->http_status);
	curl_easy_getinfo(stream->curl, CURLINFO_PRETRANSFER_TIME,
					  &stream->pretransfer_time);
	curl_easy_getinfo(stream->curl, CURLINFO_TOTAL_TIME,
					  &stream->total_time);
	stream->started = true;
}

/* ----------------------------------------------------------------
 * find_batch_end — find a row-aligned split point near fetch_size
 * bytes. Looks forward then backward for nearest newline.
 * ----------------------------------------------------------------
 */
static size_t
find_batch_end(const HttpStream * stream)
{
	const char *base = stream->buf;
	const char *found;

	if (stream->fetch_size <= 0)
		return stream->write_pos;

	if (stream->write_pos >= (size_t) stream->fetch_size)
	{
		/* Look forward first */
		found = memchr(base + stream->fetch_size,
					   '\n',
					   stream->write_pos - stream->fetch_size);
		if (found)
			return (found - base) + 1;

		/* Look backward */
		for (size_t i = stream->fetch_size - 1; i >= 0; i--)
			if (base[i] == '\n')
				return i + 1;
	}

	if (stream->transfer_done)
		return stream->write_pos;

	return 0;
}

/* ----------------------------------------------------------------
 * compact_buffer — shift unparsed data to the front of the buffer.
 * ----------------------------------------------------------------
 */
static void
compact_buffer(HttpStream * stream)
{
	if (stream->parse_pos > 0)
	{
		size_t		remaining = stream->write_pos - stream->parse_pos;

		memmove(stream->buf, stream->buf + stream->parse_pos, remaining);
		stream->write_pos = remaining;
		stream->parse_pos = 0;
		stream->batch_end = 0;
		stream->buf[stream->write_pos] = '\0';
	}
}

/* ----------------------------------------------------------------
 * http_stream_pump — drive curl_multi until the next batch is ready or the
 * transfer completes. Returns 0 on success, -1 on error.
 * ----------------------------------------------------------------
 */
int
ch_http_stream_pump(HttpStream * stream)
{
	int			running_handles;
	CURLMcode	mc;
	CURLMsg    *msg;
	int			msgs_left;

	/*
	 * Drop the already-consumed batch and see if there is enough buffered
	 * data for the next one before touching the network again.
	 */
	if (stream->parse_pos > 0)
		compact_buffer(stream);

	stream->batch_end = find_batch_end(stream);
	if (stream->batch_end > 0 ||
		(stream->transfer_done && stream->write_pos <= stream->parse_pos))
	{
		if (!stream->started)
			capture_transfer_info(stream);
		return 0;
	}

	/* Resume if paused from a previous batch */
	if (stream->paused)
	{
		stream->paused = false;
		curl_easy_pause(stream->curl, CURLPAUSE_CONT);
	}

	/* Drive the transfer */
	for (;;)
	{
		mc = curl_multi_perform(stream->multi, &running_handles);
		if (mc != CURLM_OK)
		{
			stream->http_status = CH_HTTP_STATUS_TRANSPORT_ERROR;
			free(stream->error_msg);
			stream->error_msg = strdup(curl_multi_strerror(mc));
			return -1;
		}

		if (running_handles == 0)
			stream->transfer_done = true;

		stream->batch_end = find_batch_end(stream);
		if (stream->batch_end > 0 || stream->paused || stream->transfer_done)
			break;

		curl_multi_wait(stream->multi, NULL, 0, 100, NULL);
	}

	capture_transfer_info(stream);
	stream->batch_end = find_batch_end(stream);

	/* Check for transfer errors */
	while ((msg = curl_multi_info_read(stream->multi, &msgs_left)))
	{
		if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK)
		{
			if (msg->data.result == CURLE_ABORTED_BY_CALLBACK)
				stream->http_status = CH_HTTP_STATUS_CANCELED;
			else
			{
				stream->http_status = CH_HTTP_STATUS_TRANSPORT_ERROR;
				free(stream->error_msg);
				stream->error_msg =
					strdup(stream->error_buffer[0] != '\0'
						   ? stream->error_buffer
						   : curl_easy_strerror(msg->data.result));
			}
			return -1;
		}
	}

	return 0;
}

/* ----------------------------------------------------------------
 * Public API — lifecycle
 * ----------------------------------------------------------------
 */

/*
 * ch_http_stream_begin — allocate and initialize a streaming HTTP query.
 * Returns NULL on failure.
 */
HttpStream *
ch_http_stream_begin(ch_http_connection_t * conn, const ch_query * query,
					 int32 fetch_size)
{
	HttpStream *stream;
	uuid_t		id;

	stream = calloc(1, sizeof(HttpStream));
	if (!stream)
		return NULL;

	stream->conn = conn;
	stream->fetch_size = fetch_size;

	/* Generate query ID */
	uuid_generate(id);
	uuid_unparse(id, stream->query_id);

	/*
	 * Create our own CURL easy handle so that multiple HttpStream instances
	 * (e.g. concurrent foreign scans in subqueries or joins) do not fight
	 * over the single handle in conn->curl.
	 */
	stream->curl = curl_easy_init();
	if (!stream->curl)
		goto fail;

	/* Allocate stream buffer */
	stream->buf = (char *) malloc(INITIAL_BUF_SIZE);
	if (!stream->buf)
		goto fail;
	stream->buf_allocated = INITIAL_BUF_SIZE;
	stream->buf[0] = '\0';

	setup_curl(stream, query);

	/* Create multi handle and kick off the transfer */
	stream->multi = curl_multi_init();
	if (!stream->multi)
		goto fail;
	curl_multi_add_handle(stream->multi, stream->curl);

	/* Pump until first batch is ready or transfer completes */
	ch_http_stream_pump(stream);

	return stream;

fail:
	ch_http_stream_end(stream);
	return NULL;
}

/*
 * ch_http_stream_end — clean up all owned resources.
 */
void
ch_http_stream_end(HttpStream * stream)
{
	if (!stream)
		return;

	if (stream->multi)
	{
		if (stream->curl)
			curl_multi_remove_handle(stream->multi, stream->curl);
		curl_multi_cleanup(stream->multi);
	}

	if (stream->curl)
		curl_easy_cleanup(stream->curl);

	if (stream->headers)
		curl_slist_free_all(stream->headers);
	if (stream->form)
		curl_mime_free(stream->form);
	if (stream->url)
		curl_free(stream->url);
	if (stream->buf)
		free(stream->buf);
	if (stream->error_msg)
		free(stream->error_msg);

	free(stream);
}

/* ----------------------------------------------------------------
 * Public API — accessors
 * ----------------------------------------------------------------
 */
char	   *
ch_http_stream_buffer(HttpStream * stream)
{
	return stream->buf + stream->parse_pos;
}

size_t
ch_http_stream_available(HttpStream * stream)
{
	return stream->batch_end > stream->parse_pos
		? stream->batch_end - stream->parse_pos
		: 0;
}

void
ch_http_stream_advance(HttpStream * stream, size_t n)
{
	stream->parse_pos += n;
	if (stream->parse_pos > stream->batch_end)
		stream->parse_pos = stream->batch_end;
}

bool
ch_http_stream_transfer_done(HttpStream * stream)
{
	return stream->transfer_done
		&& (stream->write_pos <= stream->parse_pos);
}

long
ch_http_stream_status(HttpStream * stream)
{
	return stream->http_status;
}

const char *
ch_http_stream_query_id(HttpStream * stream)
{
	return stream->query_id;
}

const char *
ch_http_stream_error(HttpStream * stream)
{
	return stream->error_msg;
}

double
ch_http_stream_request_time(HttpStream * stream)
{
	return stream->pretransfer_time * 1000;
}

double
ch_http_stream_total_time(HttpStream * stream)
{
	return stream->total_time * 1000;
}
