/*
 * binary_internal.h
 *
 * Cross-file shared state for the binary driver subdir. Not exposed
 * via src/include — kept inside the subdir so the public binary.h ABI
 * stays surfaceable as a library boundary later. Anything outside
 * src/binary should use binary.h instead.
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

/* Per-connection state smuggled through ch_binary_connection_t.client. */
struct ch_binary_state
{
	MemoryContext cxt;
	MemoryContextCallback reset_cb;

	chc_client *client;
	chc_io		io;

	int			fd;
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

/* Strip outer Nullable + outer LowCardinality from `t`; borrows from `t`. */
extern const chc_type *unwrap_for_block_column(const chc_type * t);

/* ereport ERROR carrying chc_err->msg with sqlstate / prefix. */
pg_noreturn extern void raise_chc(const chc_err * err, int sqlstate,
								  const char *prefix);

/* True if server advertises output_format_native_write_json_as_string. */
extern bool server_supports_json_as_string(const chc_client * c);

#endif							/* PG_CLICKHOUSE_BINARY_INTERNAL_H */
