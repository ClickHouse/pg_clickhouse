#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include <uuid/uuid.h>
#include <http.h>
#include <internal.h>

static char curl_error_buffer[CURL_ERROR_SIZE];
static bool curl_error_happened = false;
static long	curl_verbose = 0;
static void *curl_progressfunc = NULL;
static bool curl_initialized = false;
static char ch_query_id_prefix[5];

void ch_http_init(int verbose, uint32_t query_id_prefix)
{
	curl_verbose = verbose;
	snprintf(ch_query_id_prefix, 5, "%x", query_id_prefix);

	if (!curl_initialized)
	{
		curl_initialized = true;
		curl_global_init(CURL_GLOBAL_ALL);
	}
}

void ch_http_set_progress_func(void *progressfunc)
{
	curl_progressfunc = progressfunc;
}

static size_t write_data(void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize			= size * nmemb;
	ch_http_response_t *res	= userp;

	if (res->data == NULL)
		res->data = malloc(realsize + 1);
	else
		res->data = realloc(res->data, res->datasize + realsize + 1);

	memcpy(&(res->data[res->datasize]), contents, realsize);
	res->datasize += realsize;
	res->data[res->datasize] = 0;

	return realsize;
}

ch_http_connection_t *ch_http_connection(char *host, int port, char *username, char *password)
{
	int n;
	char *connstring = NULL;
	size_t len = 20; /* all symbols from url string + some extra */

	curl_error_happened = false;
	ch_http_connection_t *conn = calloc(sizeof(ch_http_connection_t), 1);
	if (!conn)
		goto cleanup;

	conn->curl = curl_easy_init();
	if (!conn->curl)
		goto cleanup;

	len += strlen(host) + snprintf(NULL, 0, "%d", port);

	if (username) {
		username = curl_easy_escape(conn->curl, username, 0);
		len += strlen(username);
	}

	if (password) {
		password = curl_easy_escape(conn->curl, password, 0);
		len += strlen(password);
	}

	connstring = calloc(len, 1);
	if (!connstring)
		goto cleanup;

	if (username && password)
	{
		n = snprintf(connstring, len, "http://%s:%s@%s:%d/", username, password, host, port);
		curl_free(username);
		curl_free(password);
	}
	else if (username)
	{
		n = snprintf(connstring, len, "http://%s@%s:%d/", username, host, port);
		curl_free(username);
	}
	else
		n = snprintf(connstring, len, "http://%s:%d/", host, port);

	if (n < 0)
		goto cleanup;

	conn->base_url = connstring;
	conn->base_url_len = strlen(conn->base_url);

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

static void set_query_id(ch_http_response_t *resp)
{
	uuid_t	id;
	uuid_generate(id);
	uuid_unparse(id, resp->query_id);
}

ch_http_response_t *ch_http_simple_query(ch_http_connection_t *conn, const char *query)
{
	char		*url;
	CURLcode	errcode;
	static char errbuffer[CURL_ERROR_SIZE];

	ch_http_response_t	*resp = calloc(sizeof(ch_http_response_t), 1);
	if (resp == NULL)
		return NULL;

	set_query_id(resp);

	assert(conn && conn->curl);

	/* construct url */
	url = malloc(conn->base_url_len + 37 + 12 /* query_id + ?query_id= */);
	sprintf(url, "%s?query_id=%s", conn->base_url, resp->query_id);

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
	curl_easy_setopt(conn->curl, CURLOPT_POSTFIELDS, query);
	curl_easy_setopt(conn->curl, CURLOPT_VERBOSE, curl_verbose);
	if (curl_progressfunc)
	{
		curl_easy_setopt(conn->curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(conn->curl, CURLOPT_XFERINFOFUNCTION, curl_progressfunc);
		curl_easy_setopt(conn->curl, CURLOPT_XFERINFODATA, conn);
	}
	else
		curl_easy_setopt(conn->curl, CURLOPT_NOPROGRESS, 1L);

	curl_error_happened = false;
	errcode = curl_easy_perform(conn->curl);
	free(url);

	if (errcode == CURLE_ABORTED_BY_CALLBACK)
	{
		resp->http_status = 418; /* I'm teapot */
		return resp;
	}
	else if (errcode != CURLE_OK)
	{
		resp->http_status = 419; /* unlegal http status */
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

	// all good with request, but we need http status to make sure
	// query went ok
	curl_easy_getinfo(conn->curl, CURLINFO_RESPONSE_CODE, &resp->http_status);
	if (curl_verbose && resp->http_status != 200)
		fprintf(stderr, "%s", resp->data);

	return resp;
}

void ch_http_close(ch_http_connection_t *conn)
{
	free(conn->base_url);
	curl_easy_cleanup(conn->curl);
}

char *ch_http_last_error(void)
{
	if (curl_error_happened)
		return curl_error_buffer;

	return NULL;
}

void ch_http_response_free(ch_http_response_t *resp)
{
	if (resp->data)
		free(resp->data);

	free(resp);
}
