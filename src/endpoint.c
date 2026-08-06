#include "postgres.h"

#include <string.h>

#include "engine.h"

/* ClickHouse Cloud also accepts HTTPS on port 443. */
#define CH_HTTP_PORT 8123
#define CH_HTTP_TLS_PORT 8443
#define CH_HTTPS_PORT 443

/* These are the default ports for the native protocol. */
#define CH_NATIVE_PORT 9000
#define CH_NATIVE_TLS_PORT 9440

static int
ends_with(const char* s, const char* suffix) {
    size_t slen       = strlen(s);
    size_t suffix_len = strlen(suffix);

    return suffix_len <= slen && !strcmp(s + slen - suffix_len, suffix);
}

/*
 * Check whether the given string matches a ClickHouse Cloud host name.
 */
static int
ch_is_cloud_host(const char* host) {
    if (!host) {
        return 0;
    }
    return ends_with(host, ".clickhouse.cloud") ||
           ends_with(host, ".clickhouse-staging.com") ||
           ends_with(host, ".clickhouse-dev.com");
}

void
ch_resolve_endpoint(
    const ch_connection_details* details,
    ch_port_set ports,
    int* out_port,
    bool* out_tls
) {
    bool http  = ports == CH_PORTS_HTTP;
    int plain  = http ? CH_HTTP_PORT : CH_NATIVE_PORT;
    int secure = http ? CH_HTTP_TLS_PORT : CH_NATIVE_TLS_PORT;
    int port   = details->port;

    switch (details->tls) {
    case CH_TLS_ON:
        *out_port = port ? port : secure;
        *out_tls  = true;
        return;
    case CH_TLS_OFF:
        *out_port = port ? port : plain;
        *out_tls  = false;
        return;
    default: /* CH_TLS_AUTO uses the host and port. */
        if (!port) {
            port = ch_is_cloud_host(details->host) ? secure : plain;
        }
        *out_port = port;
        *out_tls  = port == secure || (http && port == CH_HTTPS_PORT);
        return;
    }
}
