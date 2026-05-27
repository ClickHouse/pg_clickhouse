/*
 * binary_internal.h
 *
 * Cross-file shared state for the binary driver subdir.Not exposed
 * via src/include, anything outside src/binary should use binary.h.
 */

#ifndef PG_CLICKHOUSE_BINARY_INTERNAL_H
#define PG_CLICKHOUSE_BINARY_INTERNAL_H

#include "postgres.h"

#include <openssl/ssl.h>

#include "clickhouse.h"
#include "clickhouse-client.h"
#include "clickhouse-posix-io.h"
#include "clickhouse-openssl.h"

#include "binary.h"
#include "internal.h"

#if PG_VERSION_NUM < 180000
#define pg_noreturn pg_attribute_noreturn()
#endif

/*
 * Per-connection state smuggled through ch_binary_connection_t.client.
 *
 * Lives in own context `cxt` (child of CacheMemoryContext) so connection
 * survives transaction boundaries. Result block buffers don't live here:
 * pg_chc_alloc routes through CurrentMemoryContext, so blocks land in
 * whichever per-query context the caller has switched to.
 */
struct ch_binary_state
{
	/*
	 * Connection-lifetime context; holds this struct, the chc_client, and
	 * chc_client's initial read buffer. Deleted by ch_binary_close.
	 */
	MemoryContext cxt;

	/*
	 * Registered on cxt; closes fd / SSL on reset so half-built connections
	 * release OS resources when PG_CATCH deletes cxt.
	 */
	MemoryContextCallback reset_cb;

	chc_client *client;
	/* Transport vtable; backed by posix_state or openssl_state below. */
	chc_io		io;

	int			fd;				/* -1 once closed by reset_cb */
	SSL_CTX    *ssl_ctx;
	SSL		   *ssl;
	bool		tls;
	chc_posix_io posix_state;
	chc_openssl_io openssl_state;

	/* Per-query cancel callback (no userdata). */
	bool		(*check_cancel_fn) (void);

	/*
	 * Set when an unrecoverable protocol/IO error happened (server raised an
	 * exception mid-INSERT and closed the socket, write hit EPIPE, etc).
	 * Cache layer checks via ch_binary_is_broken & drops the entry.
	 */
	bool		broken;
};

static inline struct ch_binary_state *
conn_state(ch_binary_connection_t * conn)
{
	return (struct ch_binary_state *) conn->client;
}

/* chc allocator wired through palloc; defined in binary.c. */
extern const chc_alloc pg_chc_alloc;

/* ereport ERROR carrying chc_err->msg with sqlstate / prefix. */
pg_noreturn extern void raise_chc(const chc_err * err, int sqlstate,
								  const char *prefix);

/* True if server advertises output_format_native_write_json_as_string. */
extern bool server_supports_json_as_string(const chc_client * c);

#endif							/* PG_CLICKHOUSE_BINARY_INTERNAL_H */
