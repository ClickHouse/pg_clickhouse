#ifndef CLICKHOUSE_HTTP_H
#define CLICKHOUSE_HTTP_H

#include "postgres.h"

#include "engine.h"
#include "server_version.h"
#include <curl/curl.h>

#define CH_HTTP_QUERY_ID_LEN 37

/*
 * Synthetic statuses used by the HTTP transport to surface local cancellation
 * and libcurl transport failures through the existing response machinery.
 */
#define CH_HTTP_STATUS_CANCELED 418L
#define CH_HTTP_STATUS_TRANSPORT_ERROR 419L

/* ClickHouse answers 200 on success, but any 2xx means the query ran. */
static inline bool
ch_http_status_ok(long status) {
    return status >= 200 && status < 300;
}

typedef struct ch_http_connection_t ch_http_connection_t;

/*
 * Opaque handle to one in-flight HTTP query. The real type is the HttpStream
 * struct, defined in http.c.
 */
typedef struct HttpStream HttpStream;

/* A ClickHouse setting the caller sends as a URL parameter. */
typedef struct ch_setting {
    const char* name;
    const char* value;
} ch_setting;

/*
 * One HTTP request: the SQL to run plus the response policy its caller needs.
 * Overrides win over a query setting of the same name, so a caller pins what
 * its decoder requires while user settings fill in the rest.
 */
typedef struct ch_http_request {
    const ch_query* query;
    const ch_setting* overrides;
    int num_overrides;
    /* Hand out the body one receive chunk at a time, else buffer it whole */
    bool stream_chunks;
    ch_cancel_check cancel; /* NULL leaves the transfer uninterruptible */
} ch_http_request;

/* Response body buffered whole, for callers with no incremental decoder. */
typedef struct ch_http_response_t {
    char* data;
    size_t datasize;
    long http_status;
    char query_id[CH_HTTP_QUERY_ID_LEN];
} ch_http_response_t;

void
ch_http_init(int verbose);
/* Returns NULL and sets *error to a static message on failure. */
ch_http_connection_t*
ch_http_connection(ch_connection_details* details, const char** error);
void
ch_http_close(ch_http_connection_t* conn);

/* lifecycle */
HttpStream*
ch_http_stream_begin(ch_http_connection_t* conn, const ch_http_request* req);
void
ch_http_stream_end(HttpStream* stream);

/*
 * pgch_chunk_source next_chunk over the response body. Bytes stay valid until
 * the following call. Sets *len 0 at clean EOF; returns false with *error on
 * transport failure or cancellation. Takes void* so it can be assigned to the
 * callback slot without this header knowing pg-clickhouse-c.
 */
bool
ch_http_stream_next_chunk(void* stream, const void** data, size_t* len, char** error);

/* accessors — let pglink.c read stream state without seeing the struct */
char*
ch_http_stream_buffer(HttpStream* stream);
size_t
ch_http_stream_available(HttpStream* stream);
long
ch_http_stream_status(HttpStream* stream);
const char*
ch_http_stream_query_id(HttpStream* stream);
const char*
ch_http_stream_error(HttpStream* stream);
double
ch_http_stream_request_time(HttpStream* stream);

/*
 * Copy an error body into buf, trimming the ClickHouse build version off it so
 * messages stay stable across servers. Truncates at bufsize, so callers report
 * errors without allocating.
 */
void
ch_http_copy_error(char* buf, size_t bufsize, const char* data, size_t len);

ch_http_response_t*
ch_http_simple_query(
    ch_http_connection_t* conn,
    const ch_query* query,
    ch_cancel_check cancel
);
/*
 * Fetch and cache the server version, returning {0, 0, 0} when it cannot be
 * determined. Only the first call issues a query.
 */
ch_server_version
ch_http_server_version(ch_http_connection_t* conn, ch_cancel_check cancel);

void
ch_http_response_free(ch_http_response_t* resp);

#endif /* CLICKHOUSE_HTTP_H */
