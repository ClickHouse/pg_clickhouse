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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "utils/memutils.h"
#include "utils/palloc.h"

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "binary_internal.h"
#include "engine.h"

#define CLICKHOUSE_SECURE_PORT 9440

static bool
cancel_adapter(void *ud)
{
	struct ch_binary_state *s = ud;

	return s->check_cancel_fn ? s->check_cancel_fn() : false;
}

/*
 * Releases OS resources owned by the connection. The chc_client and its
 * read buffer live in s->cxt and are freed by the surrounding
 * MemoryContextDelete; only fd / SSL need an explicit close.
 */
static void
binary_state_reset_cb(void *arg)
{
	struct ch_binary_state *s = arg;

	if (s->client)
	{
		chc_client_close(s->client);
		s->client = NULL;
	}
	if (s->ssl)
	{
		SSL_shutdown(s->ssl);
		SSL_free(s->ssl);
		s->ssl = NULL;
	}
	if (s->ssl_ctx)
	{
		SSL_CTX_free(s->ssl_ctx);
		s->ssl_ctx = NULL;
	}
	if (s->fd >= 0)
	{
		close(s->fd);
		s->fd = -1;
	}
}

static int
tcp_connect(const char *host, int port)
{
	struct addrinfo hints = {};
	struct addrinfo *res = NULL;
	char		port_s[16];

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(port_s, sizeof(port_s), "%d", port);
	int			rc = getaddrinfo(host, port_s, &hints, &res);

	if (rc != 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("pg_clickhouse: getaddrinfo(%s:%d): %s",
						host, port, gai_strerror(rc))));

	int			fd = -1;
	int			save_errno = ECONNREFUSED;

	for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
	{
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
		{
			save_errno = errno;
			continue;
		}
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
			break;
		save_errno = errno;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("pg_clickhouse: connect(%s:%d): %s",
						host, port, strerror(save_errno))));

	int			one = 1;

	(void) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void) setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
	return fd;
}

static void
tls_connect(struct ch_binary_state *s, const char *host)
{
	static int	openssl_init_done = 0;

	if (!openssl_init_done)
	{
		SSL_library_init();
		SSL_load_error_strings();
		OpenSSL_add_all_algorithms();
		openssl_init_done = 1;
	}

	s->ssl_ctx = SSL_CTX_new(TLS_client_method());
	if (!s->ssl_ctx)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("pg_clickhouse: failed to initialize opensssl"),
				 errdetail("SSL_CTX_new failed")));
	SSL_CTX_set_verify(s->ssl_ctx, SSL_VERIFY_NONE, NULL);

	s->ssl = SSL_new(s->ssl_ctx);
	if (!s->ssl)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("pg_clickhouse: failed to initialize opensssl"),
				 errdetail("SSL_new failed")));
	SSL_set_tlsext_host_name(s->ssl, host);
	SSL_set_fd(s->ssl, s->fd);
	if (SSL_connect(s->ssl) != 1)
	{
		unsigned long e = ERR_peek_last_error();
		char		ebuf[160];

		ERR_error_string_n(e, ebuf, sizeof(ebuf));
		ereport(ERROR,
				(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
				 errmsg("pg_clickhouse: openssl failed to connect"),
				 errdetail("%s", ebuf)));
	}
}

ch_binary_connection_t *
ch_binary_connect(ch_connection_details * details)
{
	const char *host = details->host ? details->host : "127.0.0.1";
	int			port = details->port;
	bool		tls = false;

	if (port == 0)
	{
		if (ch_is_cloud_host(host))
		{
			port = CLICKHOUSE_SECURE_PORT;
			tls = true;
		}
		else
			port = 9000;
	}
	else if (port == CLICKHOUSE_SECURE_PORT)
		tls = true;

	MemoryContext cxt = AllocSetContextCreate(CacheMemoryContext,
											  "pg_clickhouse binary connection",
											  ALLOCSET_SMALL_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	struct ch_binary_state *s;
	ch_binary_connection_t *conn;

	PG_TRY();
	{
		s = palloc0(sizeof(*s));
		conn = palloc0(sizeof(*conn));
		s->cxt = cxt;
		s->fd = -1;
		s->tls = tls;
		s->reset_cb.func = binary_state_reset_cb;
		s->reset_cb.arg = s;
		MemoryContextRegisterResetCallback(cxt, &s->reset_cb);
		conn->client = s;

		s->fd = tcp_connect(host, port);
		if (tls)
		{
			tls_connect(s, host);
			chc_openssl_io_init(&s->openssl_state, &s->io, s->ssl,
								cancel_adapter, s);
		}
		else
		{
			chc_posix_io_init(&s->posix_state, &s->io, s->fd,
							  cancel_adapter, s);
		}

		chc_client_opts opts = {
			.database = details->dbname ? details->dbname : "default",
			.user = details->username ? details->username : "default",
			.password = details->password ? details->password : "",
		};
		chc_err		err = {};
		int			rc = chc_client_init(&s->client, &opts, &pg_chc_alloc, &s->io, &err);

		if (rc != CHC_OK)
			raise_chc(&err, ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION,
					  "connection error: ");
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
ch_binary_is_broken(const ch_binary_connection_t * conn)
{
	if (!conn)
		return false;
	const struct ch_binary_state *s = (const struct ch_binary_state *) conn->client;

	return s ? s->broken : false;
}

void
ch_binary_close(ch_binary_connection_t * conn)
{
	if (!conn)
		return;
	struct ch_binary_state *s = conn_state(conn);

	if (s)
		MemoryContextDelete(s->cxt);
	/* conn itself was palloc'd inside s->cxt; freed by the delete */
}
