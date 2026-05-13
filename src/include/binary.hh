/*
 * binary.hh
 *
 * Pure-C wire ABI exposed by binary.cpp. binary.cpp is the only C++ TU in
 * the project; it contains no PostgreSQL headers, palloc, ereport, Datum or
 * Oid. PG-typed helpers live in binary_pg.h.
 *
 * Error reporting: every fallible call takes a caller-supplied (errbuf,
 * errbuf_size) pair. The C++ side never allocates for error strings —
 * malloc-returning-NULL under PG's overcommit-disabled environment would
 * silently drop messages otherwise.
 */

#ifndef CLICKHOUSE_BINARY_H
#define CLICKHOUSE_BINARY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "ch_block.h"
#include "engine.h"

/* Suggested caller-side buffer size for ch_binary_* error reporting. */
#define CH_ERR_LEN 256

#ifdef __cplusplus
extern "C"
{
#endif

	typedef struct ch_binary_connection_t ch_binary_connection_t;
	typedef struct ch_binary_response_t ch_binary_response_t;
	typedef struct ch_binary_insert_handle ch_binary_insert_handle;

	/* Column metadata returned from ch_binary_begin_insert. */
	typedef struct ch_binary_column_info
	{
		const char *name;
		const		ch_type *type;	/* underlying type (Nullable stripped) */
		bool		is_nullable;
	}			ch_binary_column_info;

	/* Connection. */
	extern ch_binary_connection_t * ch_binary_connect(ch_connection_details * details,
													  char *errbuf, size_t errbuf_size);
	extern void ch_binary_close(ch_binary_connection_t * conn);

	/* SELECT. */
	extern ch_binary_response_t * ch_binary_simple_query(ch_binary_connection_t * conn,
														 const ch_query * query,
														 bool (*check_cancel) (void));
	extern void ch_binary_response_free(ch_binary_response_t * resp);
	extern const char *ch_binary_response_error(const ch_binary_response_t * resp);
	extern bool ch_binary_response_success(const ch_binary_response_t * resp);
	extern size_t ch_binary_response_block_count(const ch_binary_response_t * resp);
	extern size_t ch_binary_response_columns(const ch_binary_response_t * resp);

	/*
	 * Materialise block #idx into `out`. Returns 0 on success, non-zero on
	 * error with errbuf populated. The block borrows from the response;
	 * freeing the response frees all materialised blocks.
	 */
	extern int	ch_binary_response_block_at(ch_binary_response_t * resp,
											size_t idx, ch_block * out,
											char *errbuf, size_t errbuf_size);

	/* INSERT. */
	extern ch_binary_insert_handle * ch_binary_begin_insert(ch_binary_connection_t * conn,
															const ch_query * query,
															ch_binary_column_info * *out_cols,
															size_t * out_n,
															char *errbuf, size_t errbuf_size);

	/*
	 * Per-row, per-column append. Set isnull for NULL values (column must be
	 * Nullable). Return 0 on success; on error return non-zero with errbuf
	 * populated.
	 */
	extern int	ch_binary_append_int(ch_binary_insert_handle * h, size_t col,
									 int64_t val, bool isnull,
									 char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_uint(ch_binary_insert_handle * h, size_t col,
									  uint64_t val, bool isnull,
									  char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_double(ch_binary_insert_handle * h, size_t col,
										double val, bool isnull,
										char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_float(ch_binary_insert_handle * h, size_t col,
									   float val, bool isnull,
									   char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_bytes(ch_binary_insert_handle * h, size_t col,
									   const void *p, size_t n, bool isnull,
									   char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_decimal(ch_binary_insert_handle * h, size_t col,
										 const char *digits, bool isnull,
										 char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_uuid(ch_binary_insert_handle * h, size_t col,
									  const uint8_t bytes[16], bool isnull,
									  char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_inet(ch_binary_insert_handle * h, size_t col,
									  const char *ip_text, bool isnull,
									  char *errbuf, size_t errbuf_size);

	/*
	 * Per-row Date/DateTime/DateTime64 sent as seconds-since-epoch (int64).
	 * For DateTime64 the value is the wire-level integer at the column's
	 * scale; binary_encode.c does the scaling.
	 */
	extern int	ch_binary_append_date_seconds(ch_binary_insert_handle * h, size_t col,
											  int64_t seconds, bool isnull,
											  char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_datetime_seconds(ch_binary_insert_handle * h, size_t col,
												  int64_t seconds, bool isnull,
												  char *errbuf, size_t errbuf_size);
	extern int	ch_binary_append_datetime64_raw(ch_binary_insert_handle * h, size_t col,
												int64_t raw, bool isnull,
												char *errbuf, size_t errbuf_size);

	/*
	 * Array element append. Open with array_begin (sets the handle's
	 * current-inner). All subsequent ch_binary_append_* calls target the
	 * inner items column regardless of `col` until ch_binary_array_end.
	 */
	extern int	ch_binary_array_begin(ch_binary_insert_handle * h, size_t col,
									  size_t length,
									  char *errbuf, size_t errbuf_size);
	extern int	ch_binary_array_end(ch_binary_insert_handle * h,
									char *errbuf, size_t errbuf_size);

	/* True when the handle has an active array context (for assertions). */
	extern bool ch_binary_array_active(const ch_binary_insert_handle * h);

	/*
	 * Inspect the underlying CH column kind. Used by binary_encode.c to
	 * dispatch on (Oid pg, ch_type_kind ch).
	 */
	extern ch_type_kind ch_binary_column_kind(const ch_binary_insert_handle * h,
											  size_t col);
	extern uint32_t ch_binary_column_datetime64_precision(const ch_binary_insert_handle * h,
														  size_t col);

	/* Send buffered rows and clear; ready for next batch. */
	extern int	ch_binary_flush_block(ch_binary_insert_handle * h,
									  char *errbuf, size_t errbuf_size);

	/*
	 * End the insert and free the handle. Idempotent: passing NULL is a
	 * no-op. Populates errbuf on failure (handle still freed).
	 */
	extern void ch_binary_end_insert(ch_binary_insert_handle * h,
									 char *errbuf, size_t errbuf_size);

#ifdef __cplusplus
}
#endif

#endif							/* CLICKHOUSE_BINARY_H */
