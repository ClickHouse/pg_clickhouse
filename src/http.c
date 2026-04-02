#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <uuid/uuid.h>
#include <http.h>
#include <internal.h>

#define DATABASE_HEADER "X-ClickHouse-Database"

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

static size_t write_data(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t		realsize = size * nmemb;
	ch_http_response_t *res = userp;

	if (res->data == NULL)
		res->data = malloc(realsize + 1);
	else
		res->data = realloc(res->data, res->datasize + realsize + 1);

	if (res->data == NULL)
		return CURL_WRITEFUNC_ERROR;

	memcpy(&(res->data[res->datasize]), contents, realsize);
	res->datasize += realsize;
	res->data[res->datasize] = 0;

	return realsize;
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

	conn->dbname = details->dbname ? strdup(details->dbname) : NULL;


	if (!host || !*host)
		host = "localhost";

	if (!port)
		port = ch_is_cloud_host(host) ? CLICKHOUSE_TLS_PORT : CLICKHOUSE_PORT;

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

	char	   *scheme = port == CLICKHOUSE_TLS_PORT || port == HTTP_TLS_PORT ? "https" : "http";

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
		free(conn);

	return NULL;
}

static void
set_query_id(ch_http_response_t * resp)
{
	uuid_t		id;

	uuid_generate(id);
	uuid_unparse(id, resp->query_id);
}

ch_http_response_t *
ch_http_simple_query(ch_http_connection_t * conn, const ch_query * query)
{
	char	   *url;
	CURLcode	errcode;
	static char errbuffer[CURL_ERROR_SIZE];
	struct curl_slist *headers = NULL;
	CURLU	   *cu = curl_url();
	kv_iter		iter;
	char	   *buf = NULL;
	curl_mime  *form = NULL;
	int			i;

	ch_http_response_t *resp = calloc(sizeof(ch_http_response_t), 1);

	if (resp == NULL)
		return NULL;

	set_query_id(resp);

	assert(conn && conn->curl);

	/* Construct the base URL with the query ID. */
	curl_url_set(cu, CURLUPART_URL, conn->base_url, 0);
	buf = psprintf("query_id=%s", resp->query_id);
	curl_url_set(cu, CURLUPART_QUERY, buf, CURLU_APPENDQUERY | CURLU_URLENCODE);
	pfree(buf);

	/* Append each of the settings as a query param. */
	for (iter = new_kv_iter(query->settings); !kv_iter_done(&iter); kv_iter_next(&iter))
	{
		/* Skip settings that would break parsing and type conversion. */
		if (
			strcmp(iter.name, "date_time_output_format") == 0 ||
			strcmp(iter.name, "format_tsv_null_representation") == 0 ||
			strcmp(iter.name, "output_format_tsv_crlf_end_of_line") == 0
			)
			continue;
		buf = psprintf("%s=%s", iter.name, iter.value);
		curl_url_set(cu, CURLUPART_QUERY, buf, CURLU_APPENDQUERY | CURLU_URLENCODE);
		pfree(buf);
	}

	/* Always use ISO date format, \N for NULL, \n for EOL. */
	curl_url_set(cu, CURLUPART_QUERY, "date_time_output_format=iso", CURLU_APPENDQUERY | CURLU_URLENCODE);
	curl_url_set(cu, CURLUPART_QUERY, "format_tsv_null_representation=\\N", CURLU_APPENDQUERY | CURLU_URLENCODE);
	curl_url_set(cu, CURLUPART_QUERY, "output_format_tsv_crlf_end_of_line=0", CURLU_APPENDQUERY | CURLU_URLENCODE);
	curl_url_get(cu, CURLUPART_URL, &url, 0);
	curl_url_cleanup(cu);

	/* constant */
	errbuffer[0] = '\0';
	curl_easy_reset(conn->curl);
	curl_easy_setopt(conn->curl, CURLOPT_WRITEFUNCTION, write_data);
	curl_easy_setopt(conn->curl, CURLOPT_ERRORBUFFER, errbuffer);
	curl_easy_setopt(conn->curl, CURLOPT_PATH_AS_IS, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_URL, url);
	curl_easy_setopt(conn->curl, CURLOPT_NOSIGNAL, 1L);

	/* variable */
	curl_easy_setopt(conn->curl, CURLOPT_WRITEDATA, resp);
	curl_easy_setopt(conn->curl, CURLOPT_VERBOSE, curl_verbose);
	if (curl_progressfunc)
	{
		curl_easy_setopt(conn->curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(conn->curl, CURLOPT_XFERINFOFUNCTION, curl_progressfunc);
		curl_easy_setopt(conn->curl, CURLOPT_XFERINFODATA, conn);
	}
	else
		curl_easy_setopt(conn->curl, CURLOPT_NOPROGRESS, 1L);
	if (conn->dbname)
	{
		headers = curl_slist_append(headers, psprintf("%s: %s", DATABASE_HEADER, conn->dbname));
		if (!headers)
		{
			curl_free(url);
			resp->http_status = -1;
			resp->data = "out of memory";
			return resp;
		}
		curl_easy_setopt(conn->curl, CURLOPT_HTTPHEADER, headers);
	}

	if (query->num_params == 0)

		/*
		 * Send the query as the POST body. This ensures that
		 * date_time_output_format=iso will work for ClickHouse versions prior
		 * to 25.8.
		 */
		curl_easy_setopt(conn->curl, CURLOPT_POSTFIELDS, query->sql);
	else
	{
		/*
		 * Construct and add the the POST form data. Sadly, the
		 * date_time_output_format=iso setting will have no impact prior to
		 * ClickHouse 25.8. Details:
		 * https://github.com/ClickHouse/ClickHouse/pull/85570.
		 */
		curl_mimepart *part;

		form = curl_mime_init(conn->curl);
		if (!form)
		{
			curl_free(url);
			if (headers)
				curl_slist_free_all(headers);
			resp->http_status = -1;
			resp->data = "out of memory";
			return resp;
		}
		part = curl_mime_addpart(form);
		curl_mime_name(part, "query");
		curl_mime_data(part, query->sql, CURL_ZERO_TERMINATED);
		for (i = 0; i < query->num_params; i++)
		{
			part = curl_mime_addpart(form);
			curl_mime_name(part, psprintf("param_p%d", i + 1));
			curl_mime_data(part, query->param_values[i], CURL_ZERO_TERMINATED);
		}
		curl_easy_setopt(conn->curl, CURLOPT_MIMEPOST, form);
	}

	curl_error_happened = false;
	errcode = curl_easy_perform(conn->curl);
	curl_free(url);
	if (form)
		curl_mime_free(form);
	if (headers)
		curl_slist_free_all(headers);

	if (errcode == CURLE_ABORTED_BY_CALLBACK)
	{
		resp->http_status = 418;	/* I'm teapot */
		return resp;
	}
	else if (errcode != CURLE_OK)
	{
		resp->http_status = 419;	/* illegal http status */
		resp->data = strdup(errbuffer);
		resp->datasize = strlen(errbuffer);
		return resp;
	}

	errcode = curl_easy_getinfo(conn->curl, CURLINFO_PRETRANSFER_TIME,
								&resp->pretransfer_time);
	if (errcode != CURLE_OK)
		resp->pretransfer_time = 0;

	errcode = curl_easy_getinfo(conn->curl, CURLINFO_TOTAL_TIME, &resp->total_time);
	if (errcode != CURLE_OK)
		resp->total_time = 0;

	/*
	 * All good with request, but we need http status to make sure query went
	 * ok
	 */
	curl_easy_getinfo(conn->curl, CURLINFO_RESPONSE_CODE, &resp->http_status);
	if (curl_verbose && resp->http_status != 200)
		fprintf(stderr, "%s", resp->data);

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
