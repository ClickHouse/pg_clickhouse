/*-------------------------------------------------------------------------
 *
 * http.c
 *	  HTTP transport for pg_clickhouse.
 *
 *	  Owns the connection, request setup, chunk delivery, status, cancellation
 *	  and cleanup. Uses curl_multi + curl_easy_pause to hand ClickHouse
 *	  responses to the caller one receive chunk at a time, keeping memory
 *	  bounded.
 *
 *	  Buffers here are malloc'd, not palloc'd: libcurl callbacks must not
 *	  longjmp out of libcurl frames, and the buffers outlive any per-tuple
 *	  context. ch_http_stream_end frees them.
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uuid/uuid.h>

#include "postgres.h"

#include "mb/pg_wchar.h"

#include "http.h"
#include "kv_list.h"

struct ch_http_connection_t {
    char* dbname;
    char* base_url;   /* Allocated by curl_url_get and freed by curl_free. */
    long ssl_version; /* Minimum CURLOPT_SSLVERSION, or DEFAULT when unset. */
    ch_server_version version;
    bool version_fetched; /* True after a version lookup, including failure. */
};

#ifndef CURL_WRITEFUNC_ERROR
#define CURL_WRITEFUNC_ERROR 0xFFFFFFFF
#endif

#define DATABASE_HEADER "X-ClickHouse-Database"
#define INITIAL_BUF_SIZE (64 * 1024)

static long curl_verbose     = 0;
static bool curl_initialized = false;

void
ch_http_init(int verbose) {
    curl_verbose = verbose;

    if (!curl_initialized) {
        curl_initialized = true;
        curl_global_init(CURL_GLOBAL_ALL);
    }
}

/* ----------------------------------------------------------------
 * Connection
 * ----------------------------------------------------------------
 */

/*
 * Map the min_tls_version option to a CURLOPT_SSLVERSION value, which libcurl
 * treats as the minimum acceptable version. Returns CURL_SSLVERSION_DEFAULT to
 * leave curl's default (no minimum forced).
 */
static long
curl_min_tls_version(tls_version v) {
    switch (v) {
    case CH_TLS_V1_0:
        return CURL_SSLVERSION_TLSv1_0;
    case CH_TLS_V1_1:
        return CURL_SSLVERSION_TLSv1_1;
    case CH_TLS_V1_2:
        return CURL_SSLVERSION_TLSv1_2;
    case CH_TLS_V1_3:
        return CURL_SSLVERSION_TLSv1_3;
    default:
        return CURL_SSLVERSION_DEFAULT;
    }
}

ch_http_connection_t*
ch_http_connection(ch_connection_details* details, const char** error) {
    CURLU* cu  = NULL;
    char* host = details->host;
    int port   = details->port;
    char port_buf[12];

    ch_http_connection_t* conn = calloc(1, sizeof(ch_http_connection_t));

    *error = "out of memory";
    if (!conn) {
        return NULL;
    }

    conn->ssl_version = curl_min_tls_version(details->min_tls_version);

    if (details->dbname) {
        conn->dbname = strdup(details->dbname);
        if (conn->dbname == NULL) {
            goto cleanup;
        }
    }

    if (!host || !*host) {
        host = "localhost";
    }

    bool use_tls;

    ch_resolve_endpoint(details, CH_PORTS_HTTP, &port, &use_tls);

    snprintf(port_buf, sizeof(port_buf), "%d", port);

    cu = curl_url();
    if (cu == NULL) {
        goto cleanup;
    }

    /* Credentials go in as components so curl escapes them for us. */
    *error = "could not build ClickHouse URL";
    if (curl_url_set(cu, CURLUPART_SCHEME, use_tls ? "https" : "http", 0) !=
            CURLUE_OK ||
        curl_url_set(cu, CURLUPART_HOST, host, 0) != CURLUE_OK ||
        curl_url_set(cu, CURLUPART_PORT, port_buf, 0) != CURLUE_OK ||
        curl_url_set(cu, CURLUPART_PATH, "/", 0) != CURLUE_OK) {
        goto cleanup;
    }

    if (details->username) {
        if (curl_url_set(cu, CURLUPART_USER, details->username, CURLU_URLENCODE) !=
            CURLUE_OK) {
            goto cleanup;
        }

        if (details->password &&
            curl_url_set(cu, CURLUPART_PASSWORD, details->password, CURLU_URLENCODE) !=
                CURLUE_OK) {
            goto cleanup;
        }
    }

    if (curl_url_get(cu, CURLUPART_URL, &conn->base_url, 0) != CURLUE_OK) {
        goto cleanup;
    }

    curl_url_cleanup(cu);
    return conn;

cleanup:
    curl_url_cleanup(cu);
    free(conn->dbname);
    free(conn);

    return NULL;
}

void
ch_http_close(ch_http_connection_t* conn) {
    curl_free(conn->base_url);
    free(conn->dbname);
    free(conn);
}

/* ----------------------------------------------------------------
 * HttpStream: opaque struct.
 * ----------------------------------------------------------------
 */
struct HttpStream {
    /* Connection (borrowed, not owned) */
    ch_http_connection_t* conn;

    /* Owned CURL resources */
    CURL* curl;
    CURLM* multi;
    struct curl_slist* headers;
    curl_mime* form;
    char* url; /* allocated by curl_url_get, freed with
                * curl_free */

    /* Stream buffer */
    char* buf;
    size_t buf_allocated;
    size_t write_pos;
    bool streaming; /* hand out one chunk at a time, else buffer whole body */
    bool paused;
    bool transfer_done;
    ch_cancel_check cancel; /* NULL leaves the transfer uninterruptible */
    char error_buffer[CURL_ERROR_SIZE];

    /* Public state readable via C accessors */
    long http_status;
    char query_id[CH_HTTP_QUERY_ID_LEN];
    double pretransfer_time;
    char* error_msg; /* strdup'd, freed with free() */
};

/* CURLOPT_XFERINFOFUNCTION adapter over the caller's cancellation check. */
static int
xferinfo_callback(
    void* clientp,
    curl_off_t dltotal,
    curl_off_t dlnow,
    curl_off_t ultotal,
    curl_off_t ulnow
) {
    HttpStream* stream = (HttpStream*)clientp;

    return stream->cancel() ? 1 : 0;
}

/* True when an override replaces the user setting of this name. */
static bool
is_overridden(const ch_http_request* req, const char* name) {
    for (int i = 0; i < req->num_overrides; i++) {
        if (strcmp(req->overrides[i].name, name) == 0) {
            return true;
        }
    }

    return false;
}

/* ----------------------------------------------------------------
 * write_callback: CURL write callback. Appends data to the stream
 * buffer and, when streaming, pauses receipt so the caller drains it.
 * ----------------------------------------------------------------
 */
static size_t
write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize  = size * nmemb;
    HttpStream* self = (HttpStream*)userp;
    size_t needed    = self->write_pos + realsize + 1;

    /* Grow buffer if needed */
    if (needed > self->buf_allocated) {
        size_t newsize = self->buf_allocated * 2;
        char* newbuf;

        if (newsize < needed) {
            newsize = needed;
        }

        newbuf = (char*)realloc(self->buf, newsize);
        if (!newbuf) {
            return CURL_WRITEFUNC_ERROR;
        }

        self->buf           = newbuf;
        self->buf_allocated = newsize;
    }

    memcpy(self->buf + self->write_pos, contents, realsize);
    self->write_pos += realsize;
    self->buf[self->write_pos] = '\0';

    if (self->streaming) {
        self->paused = true;
        curl_easy_pause(self->curl, CURLPAUSE_RECV);
    }

    return realsize;
}

/* ----------------------------------------------------------------
 * setup_curl: configure the CURL easy handle for this query.
 * ----------------------------------------------------------------
 */
static void
setup_curl(HttpStream* stream, const ch_http_request* req) {
    const ch_query* query = req->query;
    CURLU* cu             = curl_url();
    char temp_buf[512];

    /* Build URL with query_id and settings */
    curl_url_set(cu, CURLUPART_URL, stream->conn->base_url, 0);

    snprintf(temp_buf, sizeof(temp_buf), "query_id=%s", stream->query_id);
    curl_url_set(cu, CURLUPART_QUERY, temp_buf, CURLU_APPENDQUERY | CURLU_URLENCODE);

    kv_iter iter = new_kv_iter(query->settings);
    while (kv_iter_next(&iter)) {
        if (is_overridden(req, iter.name)) {
            continue;
        }
        snprintf(temp_buf, sizeof(temp_buf), "%s=%s", iter.name, iter.value);
        curl_url_set(
            cu, CURLUPART_QUERY, temp_buf, CURLU_APPENDQUERY | CURLU_URLENCODE
        );
    }

    for (int i = 0; i < req->num_overrides; i++) {
        snprintf(
            temp_buf,
            sizeof(temp_buf),
            "%s=%s",
            req->overrides[i].name,
            req->overrides[i].value
        );
        curl_url_set(
            cu, CURLUPART_QUERY, temp_buf, CURLU_APPENDQUERY | CURLU_URLENCODE
        );
    }

    curl_url_get(cu, CURLUPART_URL, &stream->url, 0);
    curl_url_cleanup(cu);

    /* Configure CURL easy handle */
    curl_easy_setopt(stream->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(stream->curl, CURLOPT_WRITEDATA, stream);
    curl_easy_setopt(stream->curl, CURLOPT_ERRORBUFFER, stream->error_buffer);
    curl_easy_setopt(stream->curl, CURLOPT_PATH_AS_IS, 1L);
    curl_easy_setopt(stream->curl, CURLOPT_URL, stream->url);
    curl_easy_setopt(stream->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(stream->curl, CURLOPT_VERBOSE, curl_verbose);

    if (stream->conn->ssl_version != CURL_SSLVERSION_DEFAULT) {
        curl_easy_setopt(stream->curl, CURLOPT_SSLVERSION, stream->conn->ssl_version);
    }

    if (stream->cancel) {
        curl_easy_setopt(stream->curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(stream->curl, CURLOPT_XFERINFOFUNCTION, xferinfo_callback);
        curl_easy_setopt(stream->curl, CURLOPT_XFERINFODATA, stream);
    } else {
        curl_easy_setopt(stream->curl, CURLOPT_NOPROGRESS, 1L);
    }

    if (stream->conn->dbname) {
        snprintf(
            temp_buf, sizeof(temp_buf), "%s: %s", DATABASE_HEADER, stream->conn->dbname
        );
        stream->headers = curl_slist_append(NULL, temp_buf);
        curl_easy_setopt(stream->curl, CURLOPT_HTTPHEADER, stream->headers);
    }

    /* POST body or MIME form */
    if (query->body != NULL) {
        curl_easy_setopt(
            stream->curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)query->body_len
        );
        curl_easy_setopt(stream->curl, CURLOPT_POSTFIELDS, query->body);
    } else if (query->num_params == 0) {
        curl_easy_setopt(stream->curl, CURLOPT_POSTFIELDS, query->sql);
    } else {
        curl_mimepart* part;

        stream->form = curl_mime_init(stream->curl);
        part         = curl_mime_addpart(stream->form);
        curl_mime_name(part, "query");
        curl_mime_data(part, query->sql, CURL_ZERO_TERMINATED);

        for (int i = 0; i < query->num_params; i++) {
            part = curl_mime_addpart(stream->form);
            snprintf(temp_buf, sizeof(temp_buf), "param_p%d", i + 1);
            curl_mime_name(part, temp_buf);
            curl_mime_data(part, query->param_values[i], CURL_ZERO_TERMINATED);
        }
        curl_easy_setopt(stream->curl, CURLOPT_MIMEPOST, stream->form);
    }
}

static int
pump(HttpStream* stream) {
    int running_handles;
    CURLMcode mc;
    CURLMsg* msg;
    int msgs_left;

    if (stream->paused) {
        stream->paused = false;
        curl_easy_pause(stream->curl, CURLPAUSE_CONT);
    }

    for (;;) {
        mc = curl_multi_perform(stream->multi, &running_handles);
        if (mc != CURLM_OK) {
            stream->http_status = CH_HTTP_STATUS_TRANSPORT_ERROR;
            free(stream->error_msg);
            stream->error_msg = strdup(curl_multi_strerror(mc));
            return -1;
        }

        if (running_handles == 0) {
            stream->transfer_done = true;
        }

        /* Buffered mode waits for complete response. */
        if (stream->paused || stream->transfer_done) {
            break;
        }

        curl_multi_wait(stream->multi, NULL, 0, 100, NULL);
    }

    curl_easy_getinfo(stream->curl, CURLINFO_RESPONSE_CODE, &stream->http_status);
    curl_easy_getinfo(
        stream->curl, CURLINFO_PRETRANSFER_TIME, &stream->pretransfer_time
    );

    while ((msg = curl_multi_info_read(stream->multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE && msg->data.result != CURLE_OK) {
            if (msg->data.result == CURLE_ABORTED_BY_CALLBACK) {
                stream->http_status = CH_HTTP_STATUS_CANCELED;
            } else {
                stream->http_status = CH_HTTP_STATUS_TRANSPORT_ERROR;
                free(stream->error_msg);
                stream->error_msg = strdup(
                    stream->error_buffer[0] != '\0'
                        ? stream->error_buffer
                        : curl_easy_strerror(msg->data.result)
                );
            }
            return -1;
        }
    }

    return 0;
}

/* Blocking chunk reader; the decoder tracks its position within the chunk. */
bool
ch_http_stream_next_chunk(void* ud, const void** data, size_t* len, char** error) {
    HttpStream* stream = (HttpStream*)ud;

    *data = NULL;
    *len  = 0;

    /*
     * ch_http_stream_begin leaves the first chunk buffered, so pump only when
     * the buffer is empty
     */
    while (stream->write_pos == 0) {
        if (stream->transfer_done) {
            return true; /* clean EOF */
        }
        if (pump(stream) < 0) {
            *error = stream->error_msg;
            return false;
        }
    }

    *data = stream->buf;
    *len  = stream->write_pos;
    /* Bytes stay readable until the next pump refills from offset 0 */
    stream->write_pos = 0;
    return true;
}

/* ----------------------------------------------------------------
 * Public API: lifecycle
 * ----------------------------------------------------------------
 */

/* True for a status whose body the caller reads as an error message. */
static bool
error_status(long status) {
    /* Synthetic statuses carry no server body. */
    if (status == CH_HTTP_STATUS_CANCELED || status == CH_HTTP_STATUS_TRANSPORT_ERROR) {
        return false;
    }

    return status > 0 && !ch_http_status_ok(status);
}

/*
 * ch_http_stream_end: clean up all owned resources.
 */
void
ch_http_stream_end(HttpStream* stream) {
    if (!stream) {
        return;
    }

    if (stream->multi) {
        if (stream->curl) {
            curl_multi_remove_handle(stream->multi, stream->curl);
        }
        curl_multi_cleanup(stream->multi);
    }

    if (stream->curl) {
        curl_easy_cleanup(stream->curl);
    }

    if (stream->headers) {
        curl_slist_free_all(stream->headers);
    }
    if (stream->form) {
        curl_mime_free(stream->form);
    }
    if (stream->url) {
        curl_free(stream->url);
    }
    if (stream->buf) {
        free(stream->buf);
    }
    if (stream->error_msg) {
        free(stream->error_msg);
    }

    free(stream);
}

/*
 * ch_http_stream_begin: allocate and initialize a streaming HTTP query.
 * Returns NULL only when an allocation or a libcurl handle init fails, other
 * failures come back on the stream as a status.
 */
HttpStream*
ch_http_stream_begin(ch_http_connection_t* conn, const ch_http_request* req) {
    HttpStream* stream;
    uuid_t id;

    stream = calloc(1, sizeof(HttpStream));
    if (!stream) {
        return NULL;
    }

    stream->conn      = conn;
    stream->cancel    = req->cancel;
    stream->streaming = req->stream_chunks;

    /* Generate query ID */
    uuid_generate(id);
    uuid_unparse(id, stream->query_id);

    /*
     * Each HttpStream owns its easy handle, so concurrent foreign scans in
     * subqueries or joins do not fight over one.
     */
    stream->curl = curl_easy_init();
    if (!stream->curl) {
        goto fail;
    }

    /* Allocate stream buffer */
    stream->buf = (char*)malloc(INITIAL_BUF_SIZE);
    if (!stream->buf) {
        goto fail;
    }
    stream->buf_allocated = INITIAL_BUF_SIZE;
    stream->buf[0]        = '\0';

    setup_curl(stream, req);

    /* Create multi handle and kick off the transfer */
    stream->multi = curl_multi_init();
    if (!stream->multi) {
        goto fail;
    }
    curl_multi_add_handle(stream->multi, stream->curl);

    pump(stream);
    /* Error bodies are reported whole, so stop streaming and buffer the rest. */
    if (stream->streaming && error_status(stream->http_status)) {
        stream->streaming = false;
        pump(stream);
    }

    return stream;

fail:
    ch_http_stream_end(stream);
    return NULL;
}

/* ----------------------------------------------------------------
 * Public API: accessors
 * ----------------------------------------------------------------
 */
char*
ch_http_stream_buffer(HttpStream* stream) {
    return stream->buf;
}

size_t
ch_http_stream_available(HttpStream* stream) {
    return stream->write_pos;
}

long
ch_http_stream_status(HttpStream* stream) {
    return stream->http_status;
}

const char*
ch_http_stream_query_id(HttpStream* stream) {
    return stream->query_id;
}

const char*
ch_http_stream_error(HttpStream* stream) {
    return stream->error_msg;
}

double
ch_http_stream_request_time(HttpStream* stream) {
    return stream->pretransfer_time * 1000;
}

/*
 * take_body: transfer ownership of the response body.
 *
 * On return, *out_data is a malloc()'d buffer the caller must free(). When
 * status is CH_HTTP_STATUS_TRANSPORT_ERROR the body is the strdup'd libcurl
 * error message; otherwise it is the accumulated response bytes, NUL
 * terminated. *out_size is set to the length in bytes, excluding the NUL.
 * Sets *out_data to NULL and *out_size to 0 when there is nothing to hand
 * off. Safe to call at most once per stream; the stream itself should still
 * be released with ch_http_stream_end().
 */
static void
take_body(HttpStream* stream, char** out_data, size_t* out_size) {
    if (stream->http_status == CH_HTTP_STATUS_TRANSPORT_ERROR && stream->error_msg) {
        *out_data         = stream->error_msg;
        *out_size         = strlen(stream->error_msg);
        stream->error_msg = NULL;
        return;
    }

    if (stream->write_pos == 0 || !stream->buf) {
        *out_data = NULL;
        *out_size = 0;
        return;
    }

    *out_data   = stream->buf;
    *out_size   = stream->write_pos;
    stream->buf = NULL;
}

/*
 * ch_http_simple_query: buffer the full TabSeparated response in memory.
 *
 * Server default format is TabSeparated, so only its dialect needs pinning:
 * ISO timestamps, \N for NULL and LF line ends, as the text parsers expect.
 */
ch_http_response_t*
ch_http_simple_query(
    ch_http_connection_t* conn,
    const ch_query* query,
    ch_cancel_check cancel
) {
    static const ch_setting tsv_overrides[] = {
        { "date_time_output_format",            "iso" },
        { "format_tsv_null_representation",     "\\N" },
        { "output_format_tsv_crlf_end_of_line", "0"   },
    };
    const ch_http_request req = {
        .query         = query,
        .overrides     = tsv_overrides,
        .num_overrides = lengthof(tsv_overrides),
        .cancel        = cancel,
    };
    HttpStream* stream;
    ch_http_response_t* resp;

    stream = ch_http_stream_begin(conn, &req);
    if (stream == NULL) {
        return NULL;
    }

    resp = calloc(1, sizeof(*resp));
    if (resp == NULL) {
        ch_http_stream_end(stream);
        return NULL;
    }

    resp->http_status = stream->http_status;
    memcpy(resp->query_id, stream->query_id, CH_HTTP_QUERY_ID_LEN);
    take_body(stream, &resp->data, &resp->datasize);

    if (curl_verbose && !ch_http_status_ok(resp->http_status) && resp->data) {
        fprintf(stderr, "%s", resp->data);
    }

    ch_http_stream_end(stream);
    return resp;
}

/*
 * Copy error, dropping ` (version X.Y.Z.W (build))` suffix ClickHouse
 * appends to every exception, so messages stay stable across servers.
 */
void
ch_http_copy_error(char* buf, size_t bufsize, const char* data, size_t len) {
    size_t n = 0;
    char* version;

    if (data && len > 0) {
        int clip = (int)Min(len, bufsize - 1);
        n        = pg_encoding_mbcliplen(PG_UTF8, data, clip, clip);
        memcpy(buf, data, n);
    }
    buf[n] = '\0';

    version = strstr(buf, " (version ");
    if (version) {
        *version = '\0';
        return;
    }

    /*
     * For some reason ClickHouse 25.12 added a newline to an auth failure
     * error. Strip it out.
     */
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\0';
    }
}

/*
 * Fetches and caches the ClickHouse server version via SELECT version().
 * Returns zeros when the version cannot be determined; a failed lookup counts
 * as fetched, so it is not retried.
 */
ch_server_version
ch_http_server_version(ch_http_connection_t* conn, ch_cancel_check cancel) {
    ch_server_version none = { 0, 0, 0 };

    if (conn == NULL) {
        return none;
    }

    if (!conn->version_fetched) {
        ch_query query           = { .sql = "SELECT version()" };
        ch_http_response_t* resp = ch_http_simple_query(conn, &query, cancel);

        conn->version_fetched = true;
        if (resp != NULL) {
            if (ch_http_status_ok(resp->http_status) && resp->data != NULL) {
                int parsed, v_tweak;
                char buf[32];
                size_t n =
                    resp->datasize < sizeof(buf) - 1 ? resp->datasize : sizeof(buf) - 1;

                memcpy(buf, resp->data, n);
                buf[n] = '\0';

                /* Parse `major.minor.patch.tweak` from `version()` output. */
                parsed = sscanf(
                    buf,
                    "%d.%d.%d.%d",
                    &conn->version.major,
                    &conn->version.minor,
                    &conn->version.patch,
                    &v_tweak
                );
                if (parsed < 4) {
                    elog(
                        WARNING,
                        "pg_clickhouse: unexpected ClickHouse version() output \"%s\"",
                        buf
                    );
                }
                if (parsed < 2) {
                    /* Version string probably trash; zero out. */
                    conn->version = none;
                }
            }
            ch_http_response_free(resp);
        }
    }

    return conn->version;
}

void
ch_http_response_free(ch_http_response_t* resp) {
    if (resp->data) {
        free(resp->data);
    }

    free(resp);
}
