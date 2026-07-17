#ifndef CLICKHOUSE_INTERNAL_H
#define CLICKHOUSE_INTERNAL_H

#include <stdbool.h>

#include "server_version.h"

typedef struct ch_http_connection_t {
    char* dbname;
    char* base_url;   /* built by curl_url_get, freed with curl_free */
    long ssl_version; /* CURLOPT_SSLVERSION min; DEFAULT means unset */
    /* Server version, fetched lazily via SELECT version() then cached. */
    ch_server_version version;
    bool version_fetched; /* version lookup ran, even if it failed */
} ch_http_connection_t;

typedef struct ch_binary_connection_t {
    void* client;
    void* options;
    char* error;
} ch_binary_connection_t;

/*
 * Check whether the given string matches a ClickHouse Cloud host name.
 */
extern int
ch_is_cloud_host(const char* host);
int
ends_with(const char* s, const char* suffix);

#endif /* CLICKHOUSE_INTERNAL_H */
