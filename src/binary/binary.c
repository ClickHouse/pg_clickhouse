/*
 * binary.c
 *
 * Core glue for the binary driver. Owns the chc_alloc thunks (palloc on
 * CurrentMemoryContext, MCXT_ALLOC_HUGE so block buffers can escape the 1GB
 * cap), error mapping into ereport, and a couple of small type helpers shared
 * across the subdir.
 *
 * This also holds the CHC_IMPLEMENTATION define for the clickhouse-c
 * header-only library; the other .c files in src/binary include the
 * same headers without that define and link against the bodies emitted
 * here.
 *
 * API lives in src/include/binary.h. Driver internals shared between
 * connection.c / select.c / insert.c live in binary_internal.h
 * alongside this file.
 */

#include "postgres.h"

#include "utils/palloc.h"

#define CHC_IMPLEMENTATION
#include "clickhouse.h"
#include "clickhouse-compression.h"
#include "clickhouse-posix-io.h"
#include "clickhouse-openssl.h"
#include "clickhouse-client.h"

#include "binary_internal.h"

static void *
pg_chc_palloc(void *ud pg_attribute_unused(), size_t n)
{
	return MemoryContextAllocExtended(CurrentMemoryContext, n, MCXT_ALLOC_HUGE);
}

static void *
pg_chc_repalloc(void *ud pg_attribute_unused(), void *p,
				size_t old_bytes pg_attribute_unused(), size_t new_bytes)
{
	if (!p)
		return MemoryContextAllocExtended(CurrentMemoryContext, new_bytes, MCXT_ALLOC_HUGE);
	return repalloc_huge(p, new_bytes);
}

static void
pg_chc_pfree(void *ud pg_attribute_unused(), void *p, size_t bytes pg_attribute_unused())
{
	if (p)
		pfree(p);
}

const		chc_alloc pg_chc_alloc = {
	.ud = NULL,
	.alloc = pg_chc_palloc,
	.realloc = pg_chc_repalloc,
	.free = pg_chc_pfree,
};

/* power-of-10 lookup; CH bounds DateTime64 / Decimal scale to [0, 9] */
const		int64_t pow10i[10] = {
	1, 10, 100, 1000, 10000, 100000, 1000000,
	10000000, 100000000, 1000000000
};

/*
 * output_format_native_write_json_as_string exists on the server from
 * 24.10 onwards. Sending it as `important` against an older server would
 * fail the query, so gate.
 */
bool
server_supports_json_as_string(const chc_client * c)
{
	const		chc_server_info *info = chc_client_server_info(c);

	if (!info)
		return false;
	if (info->version_major > 24)
		return true;
	if (info->version_major == 24 && info->version_minor >= 10)
		return true;
	return false;
}

void
raise_chc(const chc_err * err, int sqlstate, const char *prefix)
{
	const char *m = (err && err->msg[0]) ? err->msg : "unknown error";

	ereport(ERROR,
			(errcode(sqlstate),
			 errmsg("pg_clickhouse: %s%s", prefix ? prefix : "", m)));
}
