#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <http.h>
#include <http_streaming.h>
#include <internal.h>

static char curl_error_buffer[CURL_ERROR_SIZE];
static bool curl_error_happened = false;
static long curl_verbose = 0;
static void *curl_progressfunc = NULL;
static bool curl_initialized = false;
static char ch_query_id_prefix[5];

void
ch_http_init(int verbose, uint32_t query_id_prefix)
{
	curl_verbose = verbose;
	snprintf(ch_query_id_prefix, 5, "%x", query_id_prefix);

	if (!curl_initialized)
	{
		curl_initialized = true;
		curl_global_init(CURL_GLOBAL_ALL);
	}
}

void
ch_http_set_progress_func(void *progressfunc)
{
	curl_progressfunc = progressfunc;
}

void	   *
ch_http_get_progress_func(void)
{
	return curl_progressfunc;
}

long
ch_http_get_verbose(void)
{
	return curl_verbose;
}

#define CLICKHOUSE_PORT 8123
#define CLICKHOUSE_TLS_PORT 8443
#define HTTP_TLS_PORT 443

ch_http_connection_t *
ch_http_connection(ch_connection_details * details)
{
	int			n;
	char	   *connstring = NULL;
	size_t		len = 20;		/* all symbols from url string + some extra */
	char	   *host = details->host,
			   *username = details->username,
			   *password = details->password;
	int			port = details->port;

	curl_error_happened = false;
	ch_http_connection_t *conn = calloc(sizeof(ch_http_connection_t), 1);

	if (!conn)
		goto cleanup;

	conn->curl = curl_easy_init();
	if (!conn->curl)
		goto cleanup;

	if (details->dbname)
	{
		conn->dbname = strdup(details->dbname);
		if (conn->dbname == NULL)
			goto cleanup;
	}


	if (!host || !*host)
		host = "localhost";

	bool		use_tls;

	switch (details->tls)
	{
		case CH_TLS_ON:
			if (!port)
				port = CLICKHOUSE_TLS_PORT;
			use_tls = true;
			break;
		case CH_TLS_OFF:
			if (!port)
				port = CLICKHOUSE_PORT;
			use_tls = false;
			break;
		default:				/* CH_TLS_AUTO */
			if (!port)
				port = ch_is_cloud_host(host) ? CLICKHOUSE_TLS_PORT : CLICKHOUSE_PORT;
			use_tls = (port == CLICKHOUSE_TLS_PORT || port == HTTP_TLS_PORT);
			break;
	}

	len += strlen(host) + snprintf(NULL, 0, "%d", port);

	if (username)
	{
		username = curl_easy_escape(conn->curl, username, 0);
		len += strlen(username);
	}

	if (password)
	{
		password = curl_easy_escape(conn->curl, password, 0);
		len += strlen(password);
	}

	connstring = calloc(len, 1);
	if (!connstring)
		goto cleanup;

	char	   *scheme = use_tls ? "https" : "http";

	if (username && password)
	{
		n = snprintf(connstring, len, "%s://%s:%s@%s:%d/", scheme, username, password, host, port);
		curl_free(username);
		curl_free(password);
	}
	else if (username)
	{
		n = snprintf(connstring, len, "%s://%s@%s:%d/", scheme, username, host, port);
		curl_free(username);
	}
	else
		n = snprintf(connstring, len, "%s://%s:%d/", scheme, host, port);

	if (n < 0)
		goto cleanup;

	conn->base_url = connstring;

	return conn;

cleanup:
	snprintf(curl_error_buffer, CURL_ERROR_SIZE, "OOM");
	curl_error_happened = true;
	if (connstring)
		free(connstring);

	if (conn)
	{
		if (conn->dbname)
			free(conn->dbname);
		free(conn);
	}

	return NULL;
}

/*
 * ch_http_simple_query — buffer the full response in memory.
 *
 * Built on top of the streaming driver with an effectively-unbounded
 * fetch_size, so the whole response lands in one batch that we then hand off
 * to the caller as a ch_http_response_t.
 */
ch_http_response_t *
ch_http_simple_query(ch_http_connection_t * conn, const ch_query * query)
{
	HttpStream *stream;
	ch_http_response_t *resp;

	stream = ch_http_stream_begin(conn, query, INT32_MAX);
	if (stream == NULL)
		return NULL;

	resp = calloc(1, sizeof(*resp));
	if (resp == NULL)
	{
		ch_http_stream_end(stream);
		return NULL;
	}

	resp->http_status = ch_http_stream_status(stream);
	resp->pretransfer_time = ch_http_stream_request_time(stream) / 1000.0;
	resp->total_time = ch_http_stream_total_time(stream) / 1000.0;
	memcpy(resp->query_id, ch_http_stream_query_id(stream), CH_HTTP_QUERY_ID_LEN);
	ch_http_stream_take_body(stream, &resp->data, &resp->datasize);

	if (curl_verbose && resp->http_status != CH_HTTP_STATUS_OK && resp->data)
		fprintf(stderr, "%s", resp->data);

	ch_http_stream_end(stream);
	return resp;
}

void
ch_http_close(ch_http_connection_t * conn)
{
	free(conn->base_url);
	if (conn->dbname)
		free(conn->dbname);
	curl_easy_cleanup(conn->curl);
}

char	   *
ch_http_last_error(void)
{
	if (curl_error_happened)
		return curl_error_buffer;

	return NULL;
}

void
ch_http_response_free(ch_http_response_t * resp)
{
	if (resp->data)
		free(resp->data);

	free(resp);
}
