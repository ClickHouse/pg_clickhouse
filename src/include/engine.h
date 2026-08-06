#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "access/tupdesc.h"
#include "kv_list.h"

/*
 * ch_connection_details defines the details for connecting to ClickHouse.
 */
/* TLS mode for the "secure" FDW option (auto=heuristic, on=force, off=never) */
typedef enum {
    CH_TLS_AUTO = 0, /* cloud-hostname heuristic (default) */
    CH_TLS_ON,       /* force TLS; default port 8443 or 9440 */
    CH_TLS_OFF,      /* force plaintext; default port 8123 or 9000 */
} tls_mode;

/* Minimum TLS protocol version for the "min_tls_version" FDW option */
typedef enum {
    CH_TLS_DEFAULT = 0, /* library default; no minimum forced */
    CH_TLS_V1_0,
    CH_TLS_V1_1,
    CH_TLS_V1_2,
    CH_TLS_V1_3,
} tls_version;

typedef struct {
    char* driver; /* "http" or "binary" */
    char* host;
    int port;
    char* username;
    char* password;
    char* dbname;
    char* compression;
    tls_mode tls;                /* TLS mode; CH_TLS_AUTO when not specified */
    tls_version min_tls_version; /* minimum TLS version; CH_TLS_DEFAULT
                                  * when not specified */
} ch_connection_details;

/* Identifies the protocol family used to choose default ports. */
typedef enum {
    CH_PORTS_HTTP,
    CH_PORTS_NATIVE,
} ch_port_set;

/*
 * Resolves the port and TLS mode from the "port" and "secure" options. An
 * unset port selects the family default. CH_TLS_AUTO uses the cloud hostname
 * and resolved port to determine whether TLS is required.
 */
extern void
ch_resolve_endpoint(
    const ch_connection_details* details,
    ch_port_set ports,
    int* out_port,
    bool* out_tls
);

/*
 * Polled by a transport while a request is in flight; return true to abort it.
 * NULL means the request cannot be cancelled.
 */
typedef bool (*ch_cancel_check)(void);

/* Used to avoid allocating memory for error text, truncating longer messages. */
#define CH_ERROR_MSG_LEN 4096

/*
 * ch_query an SQL query to execute on ClickHouse.
 */
typedef struct {
    /* The SQL query. */
    const char* sql;
    /* The number of parameters in the query. */
    const int num_params;
    /* The list of parameters to pass when executing the query. */
    const char** param_values;
    /* A description of the Tuple for the query. */
    const TupleDesc tupdesc;
    /* The numbers of the attributes in tupdesc that the query selects. */
    const List* attr_nums;
    /* List of settings to pass to ClickHouse upon execution. */
    const kv_list* settings;
    /* Posted verbatim, already prefixed with sql; sql stays set for errors. */
    const void* body;
    const size_t body_len;
} ch_query;

#define new_query(sql, num, vals, tupdesc, attrs)                                      \
    { sql, num, vals, tupdesc, attrs, chfdw_get_session_settings() }
#define new_body_query(sql, body, len)                                                 \
    { sql, 0, NULL, NULL, NULL, chfdw_get_session_settings(), body, len }

#endif /* CLICKHOUSE_ENGINE_H */
