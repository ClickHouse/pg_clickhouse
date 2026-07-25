/*
 * connection.c
 *
 * Open / probe / close for the binary driver. TCP + optional TLS, then
 * chc_client_init handshakes the protocol. Cleanup runs through a
 * MemoryContext reset callback so half-built state still releases the
 * fd / SSL pair on transaction abort.
 */

#include "postgres.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils/memutils.h"
#include "utils/palloc.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "binary_internal.h"
#include "engine.h"

#define CLICKHOUSE_SECURE_PORT 9440
#define CLICKHOUSE_PLAIN_PORT 9000

static bool
cancel_adapter(void* ud) {
    struct ch_binary_state* s = ud;

    return s->check_cancel_fn ? s->check_cancel_fn() : false;
}

/*
 * Map "compression" option to send method. NULL/unset defaults to LZ4
 * (ClickHouse's native network default). Decode side picks codec from each
 * frame's method byte, so callers fill both codec families regardless.
 */
static chc_compression
parse_compression(const char* s) {
    if (s == NULL || s[0] == '\0' || pg_strcasecmp(s, "lz4") == 0) {
        return CHC_COMP_LZ4;
    }
    if (pg_strcasecmp(s, "none") == 0) {
        return CHC_COMP_NONE;
    }
    if (pg_strcasecmp(s, "zstd") == 0) {
        return CHC_COMP_ZSTD;
    }

    ereport(
        ERROR,
        errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
        errmsg("pg_clickhouse: invalid compression \"%s\"", s),
        errhint("valid values: none, lz4, zstd")
    );
}

/*
 * Releases OS resources owned by the connection. The chc_client and its
 * read buffer live in s->cxt and are freed by the surrounding
 * MemoryContextDelete; only fd / SSL need an explicit close.
 */
static void
binary_state_reset_cb(void* arg) {
    struct ch_binary_state* s = arg;

    if (s->client) {
        chc_client_close(s->client);
        s->client = NULL;
    }
    if (s->ssl) {
        SSL_shutdown(s->ssl);
        SSL_free(s->ssl);
        s->ssl = NULL;
    }
    if (s->ssl_ctx) {
        SSL_CTX_free(s->ssl_ctx);
        s->ssl_ctx = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static int
tcp_connect(const char* host, int port) {
    struct addrinfo hints = {};
    struct addrinfo* res  = NULL;
    char port_s[16];

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_s, sizeof(port_s), "%d", port);
    int rc = getaddrinfo(host, port_s, &hints, &res);

    if (rc != 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg(
                "pg_clickhouse: getaddrinfo(%s:%d): %s", host, port, gai_strerror(rc)
            )
        );
    }

    int fd         = -1;
    int save_errno = ECONNREFUSED;

    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            save_errno = errno;
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        save_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg(
                "pg_clickhouse: connect(%s:%d): %s", host, port, strerror(save_errno)
            )
        );
    }

    int one = 1;

    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return fd;
}

/*
 * Map the min_tls_version option to an OpenSSL protocol-version constant.
 * Returns 0 to leave OpenSSL's default (no minimum forced).
 */
static int
openssl_min_proto_version(tls_version v) {
    switch (v) {
    case CH_TLS_V1_0:
        return TLS1_VERSION;
    case CH_TLS_V1_1:
        return TLS1_1_VERSION;
    case CH_TLS_V1_2:
        return TLS1_2_VERSION;
    case CH_TLS_V1_3:
        return TLS1_3_VERSION;
    default:
        return 0;
    }
}

static void
tls_connect(struct ch_binary_state* s, const char* host, tls_version min_version) {
    static int openssl_init_done = 0;

    if (!openssl_init_done) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        openssl_init_done = 1;
    }

    s->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!s->ssl_ctx) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: failed to initialize opensssl"),
            errdetail("SSL_CTX_new failed")
        );
    }

    int min_proto = openssl_min_proto_version(min_version);

    if (min_proto != 0 && SSL_CTX_set_min_proto_version(s->ssl_ctx, min_proto) != 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: failed to initialize openssl"),
            errdetail("could not set minimum TLS protocol version")
        );
    }
    /* Authenticate server: verify chain against system CAs and hostname. */
    SSL_CTX_set_verify(s->ssl_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_set_default_verify_paths(s->ssl_ctx) != 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: failed to initialize openssl"),
            errdetail("could not load default CA certificates")
        );
    }

    s->ssl = SSL_new(s->ssl_ctx);
    if (!s->ssl) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: failed to initialize opensssl"),
            errdetail("SSL_new failed")
        );
    }
    SSL_set_tlsext_host_name(s->ssl, host);
    SSL_set_hostflags(s->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    if (SSL_set1_host(s->ssl, host) != 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: failed to initialize openssl"),
            errdetail("could not set certificate verification host")
        );
    }
    SSL_set_fd(s->ssl, s->fd);
    if (SSL_connect(s->ssl) != 1) {
        /* verification failures leave error queue empty, use verify result */
        long vr         = SSL_get_verify_result(s->ssl);
        unsigned long e = ERR_peek_last_error();
        char ebuf[256];

        if (vr != X509_V_OK) {
            snprintf(
                ebuf,
                sizeof(ebuf),
                "certificate verify failed: %s",
                X509_verify_cert_error_string(vr)
            );
        } else if (e) {
            ERR_error_string_n(e, ebuf, sizeof(ebuf));
        } else {
            snprintf(ebuf, sizeof(ebuf), "SSL_connect failed");
        }

        ereport(
            ERROR,
            errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
            errmsg("pg_clickhouse: openssl failed to connect"),
            errdetail("%s", ebuf)
        );
    }
}

ch_binary_connection_t*
ch_binary_connect(ch_connection_details* details) {
    const char* host = details->host ? details->host : "127.0.0.1";
    /* volatile: set before PG_TRY setjmp, read inside */
    volatile int port = details->port;
    bool tls;

    switch (details->tls) {
    case CH_TLS_ON:
        if (port == 0) {
            port = CLICKHOUSE_SECURE_PORT;
        }
        tls = true;
        break;
    case CH_TLS_OFF:
        if (port == 0) {
            port = CLICKHOUSE_PLAIN_PORT;
        }
        tls = false;
        break;
    default: /* CH_TLS_AUTO */
        if (port == 0) {
            port =
                ch_is_cloud_host(host) ? CLICKHOUSE_SECURE_PORT : CLICKHOUSE_PLAIN_PORT;
        }
        tls = (port == CLICKHOUSE_SECURE_PORT);
        break;
    }

    chc_compression comp = parse_compression(details->compression);

    MemoryContext cxt = AllocSetContextCreate(
        CacheMemoryContext, "pg_clickhouse binary connection", ALLOCSET_SMALL_SIZES
    );
    MemoryContext old = MemoryContextSwitchTo(cxt);
    struct ch_binary_state* s;
    ch_binary_connection_t* conn;

    PG_TRY();
    {
        s                = palloc0(sizeof(*s));
        conn             = palloc0(sizeof(*conn));
        s->cxt           = cxt;
        s->fd            = -1;
        s->tls           = tls;
        s->reset_cb.func = binary_state_reset_cb;
        s->reset_cb.arg  = s;
        MemoryContextRegisterResetCallback(cxt, &s->reset_cb);
        conn->client = s;

        s->fd = tcp_connect(host, port);
        if (tls) {
            tls_connect(s, host, details->min_tls_version);
            chc_openssl_io_init(&s->openssl_state, &s->io, s->ssl, cancel_adapter, s);
        } else {
            chc_posix_io_init(&s->posix_state, &s->io, s->fd, cancel_adapter, s);
        }

        if (comp != CHC_COMP_NONE) {
            /* both families: server may answer in a different method */
            chc_lz4_codec_init(&s->codec);
            chc_zstd_codec_init(&s->codec);
        }

        chc_client_opts opts = {
            .database    = details->dbname ? details->dbname : "default",
            .user        = details->username ? details->username : "default",
            .password    = details->password ? details->password : "",
            .codec       = comp != CHC_COMP_NONE ? &s->codec : NULL,
            .compression = comp,
        };
        chc_err err = {};
        int rc      = chc_client_init(&s->client, &opts, &pgch_alloc, &s->io, &err);

        if (rc != CHC_OK) {
            pgch_raise(
                &err, ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION, "connection error: "
            );
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(old);
        MemoryContextDelete(cxt);
        PG_RE_THROW();
    }
    PG_END_TRY();

    MemoryContextSwitchTo(old);
    return conn;
}

bool
ch_binary_is_broken(const ch_binary_connection_t* conn) {
    if (!conn) {
        return false;
    }
    const struct ch_binary_state* s = (const struct ch_binary_state*)conn->client;

    return s ? s->broken : false;
}

/*
 * Reports the ClickHouse server version (major.minor.patch) for a binary
 * connection. The native protocol reports the server version during the
 * handshake, so it reads the cached handshake info rather than issuing a
 * query. Writes 0 to all out-params when the version is unavailable.
 */
void
ch_binary_server_version(
    const ch_binary_connection_t* conn,
    int* major,
    int* minor,
    int* patch
) {
    *major = *minor = *patch = 0;
    if (!conn) {
        return;
    }

    const struct ch_binary_state* s = (const struct ch_binary_state*)conn->client;
    if (!s || !s->client) {
        return;
    }

    const chc_server_info* info = chc_client_server_info(s->client);
    if (info) {
        *major = (int)info->version_major;
        *minor = (int)info->version_minor;
        *patch = (int)info->version_patch;
    }
}

void
ch_binary_close(ch_binary_connection_t* conn) {
    if (!conn) {
        return;
    }
    struct ch_binary_state* s = conn_state(conn);

    if (s) {
        MemoryContextDelete(s->cxt);
    }
    /* conn itself was palloc'd inside s->cxt; freed by the delete */
}
