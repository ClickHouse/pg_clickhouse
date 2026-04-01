#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <uuid/uuid.h>

#include "http_internal.h"

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

void
ch_http_generate_query_id(char query_id[37])
{
	uuid_t		id;

	uuid_generate(id);
	uuid_unparse(id, query_id);
}

static bool
http_skip_setting(const char *name)
{
	return strcmp(name, "date_time_output_format") == 0 ||
		strcmp(name, "format_tsv_null_representation") == 0 ||
		strcmp(name, "output_format_tsv_crlf_end_of_line") == 0;
}

static bool
http_append_query_param(CURLU * cu, const char *name, const char *value)
{
	char	   *buf = psprintf("%s=%s", name, value);
	bool		success;

	success = curl_url_set(cu, CURLUPART_QUERY, buf,
						   CURLU_APPENDQUERY | CURLU_URLENCODE) == CURLUE_OK;
	pfree(buf);
	return success;
}

bool
ch_http_build_url(ch_http_connection_t * conn, const ch_query * query,
				  const char *query_id, char **url)
{
	CURLU	   *cu;
	kv_iter		iter;
	bool		success = false;

	*url = NULL;
	cu = curl_url();
	if (!cu)
		return false;

	if (curl_url_set(cu, CURLUPART_URL, conn->base_url, 0) != CURLUE_OK)
		goto cleanup;

	if (!http_append_query_param(cu, "query_id", query_id))
		goto cleanup;

	for (iter = new_kv_iter(query->settings);
		 !kv_iter_done(&iter);
		 kv_iter_next(&iter))
	{
		if (http_skip_setting(iter.name))
			continue;
		if (!http_append_query_param(cu, iter.name, iter.value))
			goto cleanup;
	}

	if (curl_url_set(cu, CURLUPART_QUERY, "date_time_output_format=iso",
					 CURLU_APPENDQUERY | CURLU_URLENCODE) != CURLUE_OK)
		goto cleanup;
	if (curl_url_set(cu, CURLUPART_QUERY,
					 "format_tsv_null_representation=\\N",
					 CURLU_APPENDQUERY | CURLU_URLENCODE) != CURLUE_OK)
		goto cleanup;
	if (curl_url_set(cu, CURLUPART_QUERY,
					 "output_format_tsv_crlf_end_of_line=0",
					 CURLU_APPENDQUERY | CURLU_URLENCODE) != CURLUE_OK)
		goto cleanup;
	if (curl_url_get(cu, CURLUPART_URL, url, 0) != CURLUE_OK)
		goto cleanup;

	success = true;

cleanup:
	curl_url_cleanup(cu);
	if (!success && *url)
	{
		curl_free(*url);
		*url = NULL;
	}
	return success;
}

bool
ch_http_build_headers(ch_http_connection_t * conn, struct curl_slist **headers)
{
	char	   *header;
	struct curl_slist *list;

	*headers = NULL;
	if (!conn->dbname)
		return true;

	header = psprintf("%s: %s", DATABASE_HEADER, conn->dbname);
	list = curl_slist_append(NULL, header);
	pfree(header);
	if (!list)
		return false;

	*headers = list;
	return true;
}

bool
ch_http_set_post_data(CURL *curl, const ch_query * query, curl_mime **form)
{
	int			i;

	*form = NULL;
	if (query->num_params == 0)
	{
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, query->sql);
		return true;
	}

	*form = curl_mime_init(curl);
	if (!*form)
		return false;

	{
		curl_mimepart *part = curl_mime_addpart(*form);

		if (!part)
			goto oom;
		curl_mime_name(part, "query");
		curl_mime_data(part, query->sql, CURL_ZERO_TERMINATED);
	}

	for (i = 0; i < query->num_params; i++)
	{
		char	   *name = psprintf("param_p%d", i + 1);
		curl_mimepart *part = curl_mime_addpart(*form);

		if (!part)
		{
			pfree(name);
			goto oom;
		}
		curl_mime_name(part, name);
		pfree(name);
		curl_mime_data(part, query->param_values[i], CURL_ZERO_TERMINATED);
	}

	curl_easy_setopt(curl, CURLOPT_MIMEPOST, *form);
	return true;

oom:
	curl_mime_free(*form);
	*form = NULL;
	return false;
}

void
ch_http_set_common_options(ch_http_connection_t * conn, const char *url,
						   char *errbuffer, ch_http_write_func write_func,
						   void *write_data)
{
	errbuffer[0] = '\0';
	curl_easy_reset(conn->curl);
	curl_easy_setopt(conn->curl, CURLOPT_WRITEFUNCTION, write_func);
	curl_easy_setopt(conn->curl, CURLOPT_WRITEDATA, write_data);
	curl_easy_setopt(conn->curl, CURLOPT_ERRORBUFFER, errbuffer);
	curl_easy_setopt(conn->curl, CURLOPT_PATH_AS_IS, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_URL, url);
	curl_easy_setopt(conn->curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(conn->curl, CURLOPT_VERBOSE, curl_verbose);
}

void
ch_http_set_progress_options(ch_http_connection_t * conn, void *progress_data)
{
	if (curl_progressfunc)
	{
		curl_easy_setopt(conn->curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(conn->curl, CURLOPT_XFERINFOFUNCTION, curl_progressfunc);
		curl_easy_setopt(conn->curl, CURLOPT_XFERINFODATA, progress_data);
	}
	else
		curl_easy_setopt(conn->curl, CURLOPT_NOPROGRESS, 1L);
}

void
ch_http_get_transfer_info(ch_http_connection_t * conn, double *pretransfer_time,
						  double *total_time, long *http_status)
{
	long		status = 0;

	if (curl_easy_getinfo(conn->curl, CURLINFO_PRETRANSFER_TIME,
						  pretransfer_time) != CURLE_OK)
		*pretransfer_time = 0;
	if (curl_easy_getinfo(conn->curl, CURLINFO_TOTAL_TIME,
						  total_time) != CURLE_OK)
		*total_time = 0;
	if (curl_easy_getinfo(conn->curl, CURLINFO_RESPONSE_CODE,
						  &status) == CURLE_OK &&
		(status != 0 || *http_status == 0))
		*http_status = status;
}

ch_http_response_t *
ch_http_simple_query(ch_http_connection_t * conn, const ch_query * query)
{
	char	   *url;
	CURLcode	errcode;
	static char errbuffer[CURL_ERROR_SIZE];
	struct curl_slist *headers = NULL;
	curl_mime  *form = NULL;

	ch_http_response_t *resp = calloc(sizeof(ch_http_response_t), 1);

	if (resp == NULL)
		return NULL;

	ch_http_generate_query_id(resp->query_id);

	assert(conn && conn->curl);

	if (!ch_http_build_url(conn, query, resp->query_id, &url))
	{
		resp->http_status = -1;
		resp->data = "out of memory";
		return resp;
	}

	ch_http_set_common_options(conn, url, errbuffer, write_data, resp);
	ch_http_set_progress_options(conn, conn);
	if (!ch_http_build_headers(conn, &headers))
	{
		curl_free(url);
		resp->http_status = -1;
		resp->data = "out of memory";
		return resp;
	}
	if (headers)
		curl_easy_setopt(conn->curl, CURLOPT_HTTPHEADER, headers);

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
		if (!ch_http_set_post_data(conn->curl, query, &form))
		{
			curl_free(url);
			if (headers)
				curl_slist_free_all(headers);
			resp->http_status = -1;
			resp->data = "out of memory";
			return resp;
		}
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
		resp->http_status = CH_HTTP_CANCELED_STATUS;
		return resp;
	}
	else if (errcode != CURLE_OK)
	{
		resp->http_status = CH_HTTP_COMM_ERROR_STATUS;
		resp->data = strdup(errbuffer);
		resp->datasize = strlen(errbuffer);
		return resp;
	}

	ch_http_get_transfer_info(conn, &resp->pretransfer_time,
							  &resp->total_time, &resp->http_status);
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
