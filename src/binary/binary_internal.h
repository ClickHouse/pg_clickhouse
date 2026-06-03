/*
 * binary_internal.h
 *
 * Cross-file shared state for the binary driver subdir. Not exposed via
 * src/include, anything outside src/binary should use binary.h.
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

typedef struct
{
	Datum	   *datums;
	bool	   *nulls;
	size_t		len;
	Oid		   *types;
	const char *ch_type_name;
}			ch_binary_tuple_t;

/*
 * Holds an array decoded from ClickHouse or built for INSERT. For nested
 * arrays (Array(Array(...))) ndim > 1 and datums[i] points to a child
 * ch_binary_array_t with ndim-1. item_type is leaf scalar PG type,
 * array_type is postgres array type (same across nesting depths since
 * postgres uses one array type per element type regardless of ndim).
 */
typedef struct
{
	Datum	   *datums;
	bool	   *nulls;
	size_t		len;
	int			ndim;			/* nesting depth, >=1 */
	Oid			item_type;		/* leaf scalar PG type */
	Oid			array_type;		/* PG array type (same at every level) */
}			ch_binary_array_t;

/* Column metadata returned from ch_binary_begin_insert. */
typedef struct ch_binary_column_info
{
	const char *name;
	const		chc_type *type; /* type unwrapped of Nullable + LowCardinality */
	bool		is_nullable;
}			ch_binary_column_info;

/*
 * Pump next non-empty Data block off wire. Returned pointer is borrowed,
 * valid until next fetch_next_block or ch_binary_response_free. NULL when
 * stream ends (eos, error, canceled), ch_binary_response_error reports
 * cause if any.
 */
extern const chc_block *ch_binary_response_fetch_next_block(ch_binary_response_t * resp);

extern ch_binary_insert_handle * ch_binary_begin_insert(ch_binary_connection_t * conn,
														const ch_query * query,
														ch_binary_column_info * *out_cols,
														size_t * out_n);

/*
 * Tear down handle. Never raises and never talks to server, safe to call
 * from a MemoryContext reset callback during transaction abort. Flags
 * connection broken if finalize did not run.
 */
extern void ch_binary_release_insert(ch_binary_insert_handle * h);

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

/* power-of-10 lookup; CH bounds DateTime64 / Decimal scale to [0, 9] */
extern const int64_t pow10i[10];

/* ereport ERROR carrying chc_err->msg with sqlstate / prefix. */
pg_noreturn extern void raise_chc(const chc_err * err, int sqlstate,
								  const char *prefix);

/* True if server advertises output_format_native_write_json_as_string. */
extern bool server_supports_json_as_string(const chc_client * c);

/*
 * Per-row, per-column append. Set isnull for NULL values (column must be
 * Nullable). ereports on type mismatch / NULL-into-NOT-NULL.
 */
extern void ch_binary_append_int(ch_binary_insert_handle * h, size_t col,
								 int64_t val, bool isnull);
extern void ch_binary_append_uint(ch_binary_insert_handle * h, size_t col,
								  uint64_t val, bool isnull);
extern void ch_binary_append_bool(ch_binary_insert_handle * h, size_t col,
								  bool val, bool isnull);
extern void ch_binary_append_double(ch_binary_insert_handle * h, size_t col,
									double val, bool isnull);
extern void ch_binary_append_float(ch_binary_insert_handle * h, size_t col,
								   float val, bool isnull);
extern void ch_binary_append_bytes(ch_binary_insert_handle * h, size_t col,
								   const void *p, size_t n, bool isnull);
extern void ch_binary_append_decimal(ch_binary_insert_handle * h, size_t col,
									 const char *digits, bool isnull);
extern void ch_binary_append_uuid(ch_binary_insert_handle * h, size_t col,
								  const uint8_t bytes[16], bool isnull);

/*
 * IPv4: addr_be is 4 BE bytes (matches PG inet ip_addr layout).
 * IPv6: addr_be is 16 BE bytes. Pass NULL with isnull=true.
 */
extern void ch_binary_append_inet(ch_binary_insert_handle * h, size_t col,
								  const uint8_t * addr_be, size_t addrlen,
								  bool isnull);

/*
 * Per-row Date/DateTime/DateTime64 sent as seconds-since-epoch (int64).
 * For DateTime64 value is wire-level integer at column's scale;
 * encode.c does scaling.
 */
extern void ch_binary_append_date_seconds(ch_binary_insert_handle * h, size_t col,
										  int64_t seconds, bool isnull);
extern void ch_binary_append_datetime_seconds(ch_binary_insert_handle * h, size_t col,
											  int64_t seconds, bool isnull);
extern void ch_binary_append_datetime64_raw(ch_binary_insert_handle * h, size_t col,
											int64_t raw, bool isnull);

/*
 * Array element append. Open with array_begin. All subsequent
 * ch_binary_append_* calls target inner items column regardless of
 * `col` until ch_binary_array_end.
 */
extern void ch_binary_array_begin(ch_binary_insert_handle * h, size_t col);
extern void ch_binary_array_end(ch_binary_insert_handle * h);

/* True when handle has an active array context (for assertions). */
extern bool ch_binary_array_active(const ch_binary_insert_handle * h);

/*
 * Inspect underlying CH column kind. Used by encode.c to
 * dispatch on (Oid pg, chc_kind ch). When an array context is open
 * (between ch_binary_array_begin/_end) returned kind is element kind,
 * not CHC_ARRAY.
 */
extern chc_kind ch_binary_column_kind(const ch_binary_insert_handle * h,
									  size_t col);
extern uint32_t ch_binary_column_datetime64_precision(const ch_binary_insert_handle * h,
													  size_t col);

/* Send buffered rows and clear; ready for next batch. */
extern void ch_binary_flush_block(ch_binary_insert_handle * h);

#endif							/* PG_CLICKHOUSE_BINARY_INTERNAL_H */
