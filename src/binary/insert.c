/*
 * insert.c
 *
 * INSERT path for the binary driver. begin_insert sends the query, takes
 * the server's empty Data block as schema, classifies each column into
 * one of a handful of accumulator layouts (fixed / string / LC / array
 * variants), and exposes typed ch_binary_append_* shims. flush_block
 * materialises the accumulators into a chc_block_builder and ships one
 * Data packet; finalize_insert sends the closing empty Data and drains.
 * release_insert is reset-callback safe: it never talks to the wire.
 */

#include "postgres.h"

#include <string.h>

#include "common/hashfn.h"
#include "port/pg_bswap.h"
#include "utils/memutils.h"
#include "utils/palloc.h"

#include "binary_internal.h"

/* dynamic buffer for raw bytes; palloc-backed, freed by context delete */
typedef struct dynbuf
{
	uint8_t    *data;
	size_t		len;
	size_t		cap;
}			dynbuf;

static void
dynbuf_reserve(dynbuf * b, size_t need)
{
	if (need <= b->cap)
		return;
	size_t		ncap = b->cap ? b->cap : 64;

	while (ncap < need)
		ncap *= 2;
	b->data = b->data
		? repalloc_huge(b->data, ncap)
		: MemoryContextAllocExtended(CurrentMemoryContext, ncap, MCXT_ALLOC_HUGE);
	b->cap = ncap;
}

static void
dynbuf_append(dynbuf * b, const void *src, size_t n)
{
	dynbuf_reserve(b, b->len + n);
	if (src && n)
		memcpy(b->data + b->len, src, n);
	b->len += n;
}

static void
dynbuf_append_zero(dynbuf * b, size_t n)
{
	dynbuf_reserve(b, b->len + n);
	memset(b->data + b->len, 0, n);
	b->len += n;
}

static void
dynbuf_reset(dynbuf * b)
{
	b->len = 0;
}

/* dynamic array of u64 */
typedef struct u64buf
{
	uint64_t   *data;
	size_t		len;
	size_t		cap;
}			u64buf;

static void
u64buf_push(u64buf * b, uint64_t v)
{
	if (b->len + 1 > b->cap)
	{
		size_t		ncap = b->cap ? b->cap * 2 : 16;
		size_t		bytes = ncap * sizeof(uint64_t);

		b->data = b->data
			? repalloc_huge(b->data, bytes)
			: MemoryContextAllocExtended(CurrentMemoryContext, bytes, MCXT_ALLOC_HUGE);
		b->cap = ncap;
	}
	b->data[b->len++] = v;
}

static void
u64buf_reset(u64buf * b)
{
	b->len = 0;
}

/*
 * Insert column layout decided at begin_insert from the column's chc_type.
 * Each typed append_* appends into one of three storage groups:
 *   - body / body_offs:  fixed-width or string values for the top-level
 *     column or, when an array context is open, the inner items.
 *   - arr_offs:          cumulative outer ends for Array(*) columns.
 *   - nulls:             one byte per top-level row for Nullable(*).
 */
typedef enum
{
	IC_FIXED,
	IC_STRING,
	IC_LC_STRING,
	IC_ARRAY_FIXED,
	IC_ARRAY_STRING,
	IC_ARRAY_NESTED_FIXED,
	IC_ARRAY_NESTED_STRING,
	IC_JSON_STRING,
}			ic_layout;

static inline bool
ic_layout_array_fixed(ic_layout l)
{
	return l == IC_ARRAY_FIXED || l == IC_ARRAY_NESTED_FIXED;
}

static inline bool
ic_layout_array_string(ic_layout l)
{
	return l == IC_ARRAY_STRING || l == IC_ARRAY_NESTED_STRING;
}

typedef struct ic_col
{
	const		chc_type *t;	/* full column type (incl. Nullable wrapper) */
	const		chc_type *inner_t;	/* possibly unwrapped Nullable */
	const		chc_type *elem_t;	/* Array element type, with Nullable
									 * unwrapped */
	ic_layout	layout;
	bool		is_nullable;
	int			array_depth;	/* current open begin/end nesting */
	int			ndim;			/* >=1 for array layouts; 1 for top-level
								 * Array */
	bool		array_inner_is_string;
	size_t		elem_size;		/* fixed elem size (top-level FIXED or array
								 * element FIXED) */
	uint32_t	dt64_precision;
	dynbuf		body;			/* row-aligned for FIXED, byte-flat for
								 * STRING/LC */
	u64buf		body_offs;		/* offsets per row for STRING/LC inner */
	dynbuf		nulls;			/* per top-level row */
	u64buf		arr_offs;		/* cumulative ends for outermost array layer
								 * (or only layer when ndim==1) */
	u64buf	   *arr_offs_inner; /* for ndim>1: ndim-1 inner-layer offsets,
								 * [0]=just inside outer, [ndim-2]=adjacent to
								 * leaves */
	size_t		n_rows;			/* committed top-level rows */

	/*
	 * Cached column info exposed to callers. info.type borrows from the
	 * initial_block's chc_type tree, unwrapping Nullable + outer LC.
	 */
	ch_binary_column_info info;
}			ic_col;

struct ch_binary_insert_handle
{
	MemoryContext cxt;
	chc_client *client;
	struct ch_binary_state *state;	/* parent connection; used to flag broken
									 * state on error */
	chc_block  *initial_block;	/* schema source (server's empty Data) */
	size_t		ncols;
	ic_col	   *cols;
	bool		array_active;
	size_t		array_col_idx;
	bool		started;
	bool		finalized;		/* finalize_insert has run (success or raised) */
};

static void
classify_column(ic_col * ic, const chc_type * t)
{
	ic->t = t;
	if (chc_type_kind(t) == CHC_NULLABLE)
	{
		ic->is_nullable = true;
		t = chc_type_child(t, 0);
	}
	ic->inner_t = t;

	chc_kind	k = chc_type_kind(t);

	if (k == CHC_LOW_CARDINALITY)
	{
		const		chc_type *inner = chc_type_child(t, 0);
		bool		inner_nullable = chc_type_kind(inner) == CHC_NULLABLE;
		const		chc_type *base = inner_nullable ? chc_type_child(inner, 0) : inner;

		if (chc_type_kind(base) != CHC_STRING)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported LowCardinality variant: %s",
							chc_type_name(base, NULL))));
		}

		/*
		 * Nullable lives inside LowCardinality, not as an outer wrapper, so
		 * record it here: resolve_col tracks the per-row null bits
		 * build_lc_dict reads to map nulls onto dict slot 0.
		 */
		ic->is_nullable = inner_nullable;
		ic->layout = IC_LC_STRING;
		ic->elem_t = base;
		return;
	}
	if (k == CHC_ARRAY)
	{
		/*
		 * Walk through nested Array layers to the leaf element type so
		 * Array(Array(...)) maps to a single ic_col with ndim levels.
		 */
		const		chc_type *base = t;
		int			ndim = 0;

		while (chc_type_kind(base) == CHC_ARRAY)
		{
			ndim++;
			base = chc_type_child(base, 0);
		}
		bool		elem_nullable = chc_type_kind(base) == CHC_NULLABLE;

		/* Array(Nullable(T)) not supported yet. */
		if (elem_nullable)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: %s not currently supported",
							chc_type_name(base, NULL))));
		}
		chc_kind	ek = chc_type_kind(base);

		ic->elem_t = base;
		ic->ndim = ndim;
		if (ek == CHC_STRING || ek == CHC_FIXED_STRING)
		{
			ic->layout = ndim == 1 ? IC_ARRAY_STRING : IC_ARRAY_NESTED_STRING;
			ic->array_inner_is_string = true;
		}
		else if (chc_type_elem_size(base) > 0)
		{
			ic->layout = ndim == 1 ? IC_ARRAY_FIXED : IC_ARRAY_NESTED_FIXED;
			ic->elem_size = chc_type_elem_size(base);
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported Array element type: %s", chc_type_name(base, NULL))));
		if (ndim > 1)
			ic->arr_offs_inner = palloc0((size_t) (ndim - 1) * sizeof(u64buf));
		return;
	}
	if (k == CHC_STRING)
	{
		ic->layout = IC_STRING;
		return;
	}
	if (k == CHC_JSON)
	{
		ic->layout = IC_JSON_STRING;
		return;
	}
	size_t		es = chc_type_elem_size(t);

	if (es > 0)
	{
		ic->layout = IC_FIXED;
		ic->elem_size = es;
		if (k == CHC_DATETIME64)
			ic->dt64_precision = (uint32_t) chc_type_datetime64_scale(t);
		return;
	}
	if (k == CHC_FIXED_STRING)
	{
		ic->layout = IC_FIXED;
		ic->elem_size = (size_t) chc_type_fixed_size(t);
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
			 errmsg("pg_clickhouse: could not prepare insert - unsupported column type: %s",
					chc_type_name(t, NULL))));
}

static void
recv_initial_block(struct ch_binary_state *s, ch_binary_insert_handle * h)
{
	for (;;)
	{
		chc_packet	pkt = {};
		chc_err		err = {};
		int			rc = chc_client_recv_packet(s->client, &pkt, &err);

		if (rc != CHC_OK)
		{
			s->broken = true;
			raise_chc(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ");
		}
		if (pkt.kind == CHC_PKT_EXCEPTION)
		{
			const char *msg = "server exception";

			if (pkt.exception && pkt.exception->display_text)
				msg = pkt.exception->display_text;
			else if (pkt.exception && pkt.exception->name)
				msg = pkt.exception->name;
			char	   *msg_copy = pstrdup(msg);

			s->broken = true;
			chc_packet_clear(s->client, &pkt);
			ereport(ERROR,
					(errcode(ERRCODE_FDW_ERROR),
					 errmsg("pg_clickhouse: could not prepare insert - %s", msg_copy)));
		}
		if (pkt.kind == CHC_PKT_DATA && pkt.block &&
			chc_block_n_columns(pkt.block) > 0)
		{
			h->initial_block = pkt.block;
			pkt.block = NULL;
			chc_packet_clear(s->client, &pkt);
			return;
		}
		chc_packet_clear(s->client, &pkt);
	}
}

/*
 * After classify_column ereports the server is mid-INSERT awaiting our
 * Data; send empty Data + drain so the connection stays usable.
 */
static void
drain_aborted_insert(struct ch_binary_state *s)
{
	chc_err		ce = {};

	(void) chc_client_send_data(s->client, NULL, &ce);
	for (;;)
	{
		chc_packet	drain = {};

		ce = (chc_err)
		{
			0
		};
		int			drc = chc_client_recv_packet(s->client, &drain, &ce);
		bool		eos = (drc == CHC_OK &&
						   (drain.kind == CHC_PKT_END_OF_STREAM ||
							drain.kind == CHC_PKT_EXCEPTION));

		chc_packet_clear(s->client, &drain);
		if (drc != CHC_OK || eos)
			break;
	}
}

ch_binary_insert_handle *
ch_binary_begin_insert(ch_binary_connection_t * conn, const ch_query * query,
					   ch_binary_column_info * *out_cols, size_t * out_n)
{
	struct ch_binary_state *s = conn_state(conn);

	/*
	 * Parent h's cxt to the connection's cxt, not CurrentMemoryContext. The
	 * caller registers a reset callback on a sibling context that drains the
	 * insert via end_insert(h); if h lived under that sibling, MemoryContext
	 * tree teardown would free h before the callback fired.
	 */
	MemoryContext cxt = AllocSetContextCreate(s->cxt,
											  "pg_clickhouse binary insert",
											  ALLOCSET_DEFAULT_SIZES);
	MemoryContext old = MemoryContextSwitchTo(cxt);
	ch_binary_insert_handle *h;
	volatile bool need_drain = false;

	PG_TRY();
	{
		h = palloc0(sizeof(*h));
		h->cxt = cxt;
		h->client = s->client;
		h->state = s;

		/* Append " VALUES" so server enters insert mode. */
		size_t		sql_len = strlen(query->sql);
		char	   *sql = palloc(sql_len + 8);

		memcpy(sql, query->sql, sql_len);
		memcpy(sql + sql_len, " VALUES", 7);
		sql[sql_len + 7] = '\0';

		/*
		 * On servers that support it (24.10+), tell server to serialize any
		 * JSON columns using STRING wire format. INSERT path doesn't need
		 * this, server reads the per-column version prefix the builder
		 * writes, but we set it on the same packet for symmetry with the
		 * SELECT path and so any RETURNING-style projection on top still
		 * decodes.
		 */
		chc_query_setting json_setting = {
			.name = "output_format_native_write_json_as_string",
			.value = "1",
			.important = true,
		};
		chc_query_opts insert_opts = {};
		const		chc_query_opts *opts_ptr = NULL;

		if (server_supports_json_as_string(s->client))
		{
			insert_opts.settings = &json_setting;
			insert_opts.n_settings = 1;
			opts_ptr = &insert_opts;
		}

		chc_err		err = {};
		int			rc = chc_client_send_query_ex(s->client, sql, sql_len + 7, opts_ptr, &err);

		if (rc != CHC_OK)
		{
			s->broken = true;
			raise_chc(&err, ERRCODE_FDW_ERROR, "could not prepare insert - ");
		}

		recv_initial_block(s, h);

		/*
		 * Server is now waiting our Data; failures past this point need an
		 * empty-Data + drain so the connection stays usable.
		 */
		need_drain = true;
		h->started = true;

		size_t		nc = chc_block_n_columns(h->initial_block);

		h->ncols = nc;
		h->cols = palloc0(nc * sizeof(ic_col));
		for (size_t i = 0; i < nc; i++)
		{
			ic_col	   *c = &h->cols[i];
			const		chc_type *ct = chc_block_column_type(h->initial_block, i);
			size_t		nlen;
			const char *nm;

			classify_column(c, ct);
			nm = chc_block_column_name(h->initial_block, i, &nlen);
			c->info.name = pnstrdup(nm ? nm : "", nlen);
			c->info.is_nullable = c->is_nullable;

			/*
			 * inner_t already unwrapped Nullable; unwrap LowCardinality and
			 * perhaps its inner Nullable to expose innermost type.
			 */
			const		chc_type *vt = c->inner_t;

			if (chc_type_kind(vt) == CHC_LOW_CARDINALITY)
			{
				vt = chc_type_child(vt, 0);
				if (chc_type_kind(vt) == CHC_NULLABLE)
					vt = chc_type_child(vt, 0);
			}
			c->info.type = vt;
		}

		*out_cols = NULL;
		if (nc)
		{
			ch_binary_column_info *arr = palloc(nc * sizeof(*arr));

			for (size_t i = 0; i < nc; i++)
				arr[i] = h->cols[i].info;
			*out_cols = arr;
		}
		*out_n = nc;
	}
	PG_CATCH();
	{
		if (need_drain)
			drain_aborted_insert(s);
		MemoryContextSwitchTo(old);
		MemoryContextDelete(cxt);
		PG_RE_THROW();
	}
	PG_END_TRY();

	MemoryContextSwitchTo(old);
	return h;
}

/*
 * Resolve the storage to receive a value for column `col_idx`, accounting
 * for an active array context. Returns the ic_col* whose body buffer is
 * the target. For Nullable wrappers, records the null bit. ereports on
 * NULL-into-NOT-NULL.
 */
static ic_col *
resolve_col(ch_binary_insert_handle * h, size_t col_idx, bool isnull)
{
	ic_col	   *c = h->array_active ? &h->cols[h->array_col_idx] : &h->cols[col_idx];

	if (isnull && !h->array_active && !c->is_nullable)
	{
		size_t		tnlen;
		const char *tname = chc_type_name(c->t, &tnlen);

		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("pg_clickhouse: cannot append NULL to NOT NULL %.*s column",
						(int) tnlen, tname ? tname : "?")));
	}
	if (!h->array_active && c->is_nullable)
	{
		uint8_t		b = isnull ? 1 : 0;

		dynbuf_append(&c->nulls, &b, 1);
	}
	return c;
}

static void
append_fixed_bytes(ic_col * c, const void *p, size_t n)
{
	dynbuf_append(&c->body, p, n);
}

static void
append_fixed_zero(ic_col * c, size_t n)
{
	dynbuf_append_zero(&c->body, n);
}

static void
append_string_row(ic_col * c, const void *p, size_t n)
{
	if (n)
		dynbuf_append(&c->body, p, n);
	u64buf_push(&c->body_offs, c->body.len);
}

/*
 * Convert decimal text "[-]digits[.frac]" into a ClickHouse Decimal wire value:
 * two's-complement signed integer in `width` LE bytes (4/8/16/32 for
 * Decimal32/64/128/256), with `scale` fractional digits folded into the value.
 */
static void
decimal_text_to_bytes(const char *s, uint32_t scale, size_t width, uint8_t * out)
{
	const char *input = s;
	bool		neg = false;

	if (!s)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("pg_clickhouse: decimal parse failure")));
	if (*s == '-')
	{
		neg = true;
		s++;
	}
	else if (*s == '+')
		s++;

	/* find offsets, going to iterate digits, skipping non-digits */
	const char *dot = strchr(s, '.');
	size_t		ilen = dot ? (size_t) (dot - s) : strlen(s);
	const char *frac = dot ? dot + 1 : "";
	size_t		flen = strlen(frac);
	size_t		ndig = ilen + scale;

	uint32_t	mag[8] = {};
	size_t		nwords = width / 4;

	/* accumulate digits (padded/truncated to scale) into mag */
	for (size_t i = 0; i < ndig; i++)
	{
		char		c = i < ilen ? s[i]
			: i - ilen < flen ? frac[i - ilen]
			: '0';

		/* reject NaN / Infinity from numeric_out */
		if (c < '0' || c > '9')
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("pg_clickhouse: cannot encode \"%s\" as ClickHouse Decimal",
							input)));
		uint64_t	carry = (uint64_t) (c - '0');

		for (size_t b = 0; b < nwords; b++)
		{
			uint64_t	v = (uint64_t) mag[b] * 10 + carry;

			mag[b] = (uint32_t) v;
			carry = v >> 32;
		}
	}
	/* two's-complement negation */
	if (neg)
	{
		for (size_t b = 0; b < nwords; b++)
			mag[b] = ~mag[b];
		uint64_t	carry = 1;

		for (size_t b = 0; b < nwords && carry; b++)
		{
			uint64_t	v = (uint64_t) mag[b] + carry;

			mag[b] = (uint32_t) v;
			carry = v >> 32;
		}
	}

	memcpy(out, mag, width);
}

static void
append_int_kind(ic_col * c, int64_t val)
{
	chc_kind	k = ic_layout_array_fixed(c->layout)
		? chc_type_kind(c->elem_t)
		: chc_type_kind(c->inner_t);

	switch (k)
	{
		case CHC_INT8:
		case CHC_UINT8:
		case CHC_BOOL:
			{
				int8_t		v = (int8_t) val;

				append_fixed_bytes(c, &v, 1);
				return;
			}
		case CHC_INT16:
		case CHC_UINT16:
			{
				int16_t		v = (int16_t) val;

				append_fixed_bytes(c, &v, 2);
				return;
			}
		case CHC_INT32:
		case CHC_UINT32:
			{
				int32_t		v = (int32_t) val;

				append_fixed_bytes(c, &v, 4);
				return;
			}
		case CHC_INT64:
		case CHC_UINT64:
			{
				append_fixed_bytes(c, &val, 8);
				return;
			}
		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("pg_clickhouse: int value into non-integer column")));
	}
}

void
ch_binary_append_int(ch_binary_insert_handle * h, size_t col, int64_t val, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);

	if (isnull)
		append_fixed_zero(c, c->elem_size);
	else
		append_int_kind(c, val);
	MemoryContextSwitchTo(old);
}

void
ch_binary_append_uint(ch_binary_insert_handle * h, size_t col, uint64_t val, bool isnull)
{
	ch_binary_append_int(h, col, (int64_t) val, isnull);
}

void
ch_binary_append_bool(ch_binary_insert_handle * h, size_t col, bool val, bool isnull)
{
	ch_binary_append_int(h, col, val, isnull);
}

void
ch_binary_append_double(ch_binary_insert_handle * h, size_t col, double val, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);

	if (isnull)
		append_fixed_zero(c, 8);
	else
		append_fixed_bytes(c, &val, 8);
	MemoryContextSwitchTo(old);
}

void
ch_binary_append_float(ch_binary_insert_handle * h, size_t col, float val, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);

	if (isnull)
		append_fixed_zero(c, 4);
	else
		append_fixed_bytes(c, &val, 4);
	MemoryContextSwitchTo(old);
}

void
ch_binary_append_bytes(ch_binary_insert_handle * h, size_t col, const void *p,
					   size_t n, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	chc_kind	k = ic_layout_array_string(c->layout)
		? chc_type_kind(c->elem_t)
		: chc_type_kind(c->inner_t);

	if (c->layout == IC_LC_STRING)
	{
		/* Accumulate via body+body_offs; dict materialized at flush. */
		if (isnull)
			append_string_row(c, NULL, 0);
		else
			append_string_row(c, p, n);
		MemoryContextSwitchTo(old);
		return;
	}

	if (k == CHC_FIXED_STRING)
	{
		size_t		w = (size_t) chc_type_fixed_size(ic_layout_array_string(c->layout) ? c->elem_t : c->inner_t);

		if (isnull)
			append_fixed_zero(c, w);
		else
		{
			size_t		take = n < w ? n : w;

			if (take)
				dynbuf_append(&c->body, p, take);
			if (take < w)
				dynbuf_append_zero(&c->body, w - take);
		}
		MemoryContextSwitchTo(old);
		return;
	}
	if (k == CHC_ENUM8 || k == CHC_ENUM16)
	{
		size_t		ew = (k == CHC_ENUM8) ? 1 : 2;

		if (isnull)
		{
			append_fixed_zero(c, ew);
			MemoryContextSwitchTo(old);
			return;
		}
		const		chc_type *et = ic_layout_array_string(c->layout) ? c->elem_t : c->inner_t;
		size_t		nenum = chc_type_enum_count(et);
		int64_t		val = 0;
		bool		found = false;

		for (size_t i = 0; i < nenum; i++)
		{
			const char *en;
			size_t		el;
			int64_t		ev;

			chc_type_enum_at(et, i, &en, &el, &ev);
			if (el == n && memcmp(en, p, n) == 0)
			{
				val = ev;
				found = true;
				break;
			}
		}
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("pg_clickhouse: enum value '%.*s' not found",
							(int) n, (const char *) p)));
		if (k == CHC_ENUM8)
		{
			int8_t		v = (int8_t) val;

			append_fixed_bytes(c, &v, 1);
		}
		else
		{
			int16_t		v = (int16_t) val;

			append_fixed_bytes(c, &v, 2);
		}
		MemoryContextSwitchTo(old);
		return;
	}
	if (k == CHC_STRING || k == CHC_JSON || k == CHC_OBJECT)
	{
		if (isnull)
			append_string_row(c, NULL, 0);
		else
			append_string_row(c, p, n);
		MemoryContextSwitchTo(old);
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("pg_clickhouse: bytes into non-text column")));
}

void
ch_binary_append_decimal(ch_binary_insert_handle * h, size_t col,
						 const char *digits, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	const		chc_type *t = ic_layout_array_fixed(c->layout) ? c->elem_t : c->inner_t;
	chc_kind	k = chc_type_kind(t);
	size_t		w;

	switch (k)
	{
		case CHC_DECIMAL32:
			w = 4;
			break;
		case CHC_DECIMAL64:
			w = 8;
			break;
		case CHC_DECIMAL128:
			w = 16;
			break;
		case CHC_DECIMAL256:
			w = 32;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("pg_clickhouse: decimal into non-decimal column")));
	}
	uint8_t		raw[32] = {};

	if (!isnull && digits)
	{
		uint32_t	scale = (uint32_t) chc_type_decimal_scale(t);

		decimal_text_to_bytes(digits, scale, w, raw);
	}
	append_fixed_bytes(c, raw, w);
	MemoryContextSwitchTo(old);
}

void
ch_binary_append_uuid(ch_binary_insert_handle * h, size_t col,
					  const uint8_t bytes[16], bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	uint8_t		wire[16] = {};

	if (!isnull)
	{
		uint64_t	a,
					b;

		memcpy(&a, bytes, 8);
		memcpy(&b, bytes + 8, 8);
		a = pg_ntoh64(a);
		b = pg_ntoh64(b);
		memcpy(wire, &a, 8);
		memcpy(wire + 8, &b, 8);
	}
	append_fixed_bytes(c, wire, 16);
	MemoryContextSwitchTo(old);
}

/*
 * addr_be is BE bytes (PG inet ip_addr layout). For IPv4 CH wire is a
 * host-order uint32; pg_ntoh32 turns BE bytes into the right host value.
 * For IPv6 CH wire matches PG byte order.
 */
void
ch_binary_append_inet(ch_binary_insert_handle * h, size_t col,
					  const uint8_t * addr_be, size_t addrlen, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	const		chc_type *t = ic_layout_array_fixed(c->layout) ? c->elem_t : c->inner_t;
	chc_kind	k = chc_type_kind(t);

	if (k == CHC_IPV4 && addrlen == 4)
	{
		uint32_t	addr = 0;

		if (!isnull && addr_be)
		{
			uint32_t	be;

			memcpy(&be, addr_be, 4);
			addr = pg_ntoh32(be);
		}
		append_fixed_bytes(c, &addr, 4);
		MemoryContextSwitchTo(old);
		return;
	}
	if (k == CHC_IPV6 && addrlen == 16)
	{
		uint8_t		raw[16] = {};

		if (!isnull && addr_be)
			memcpy(raw, addr_be, 16);
		append_fixed_bytes(c, raw, 16);
		MemoryContextSwitchTo(old);
		return;
	}
	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("pg_clickhouse: cannot insert inet into non-inet column")));
}

void
ch_binary_append_date_seconds(ch_binary_insert_handle * h, size_t col,
							  int64_t seconds, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	const		chc_type *t = ic_layout_array_fixed(c->layout) ? c->elem_t : c->inner_t;
	chc_kind	k = chc_type_kind(t);

	if (k == CHC_DATE)
	{
		uint16_t	days = isnull ? 0 : (uint16_t) (seconds / 86400);

		append_fixed_bytes(c, &days, 2);
	}
	else if (k == CHC_DATE32)
	{
		int32_t		days = isnull ? 0 : (int32_t) (seconds / 86400);

		append_fixed_bytes(c, &days, 4);
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("pg_clickhouse: date into non-date column")));
	MemoryContextSwitchTo(old);
}

void
ch_binary_append_datetime_seconds(ch_binary_insert_handle * h, size_t col,
								  int64_t seconds, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	uint32_t	v = isnull ? 0 : (uint32_t) seconds;

	append_fixed_bytes(c, &v, 4);
	MemoryContextSwitchTo(old);
}

void
ch_binary_append_datetime64_raw(ch_binary_insert_handle * h, size_t col,
								int64_t raw, bool isnull)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	ic_col	   *c = resolve_col(h, col, isnull);
	int64_t		v = isnull ? 0 : raw;

	append_fixed_bytes(c, &v, 8);
	MemoryContextSwitchTo(old);
}

void
ch_binary_array_begin(ch_binary_insert_handle * h, size_t col)
{
	/*
	 * Nested arrays recurse via append_one with col=0, so once an array is
	 * open the caller's col is meaningless, resolve from array_col_idx.
	 */
	size_t		idx = h->array_active ? h->array_col_idx : col;

	if (idx >= h->ncols)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("pg_clickhouse: array_begin: col out of range")));
	ic_col	   *c = &h->cols[idx];

	if (c->layout != IC_ARRAY_FIXED && c->layout != IC_ARRAY_STRING
		&& c->layout != IC_ARRAY_NESTED_FIXED && c->layout != IC_ARRAY_NESTED_STRING)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("pg_clickhouse: array_begin: column is not Array")));
	if (c->array_depth >= c->ndim)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("pg_clickhouse: array_begin: nesting exceeds column ndim")));
	if (c->array_depth == 0)
	{
		if (c->is_nullable)
		{
			uint8_t		b = 0;
			MemoryContext old = MemoryContextSwitchTo(h->cxt);

			dynbuf_append(&c->nulls, &b, 1);
			MemoryContextSwitchTo(old);
		}
		h->array_active = true;
		h->array_col_idx = idx;
	}
	c->array_depth++;
}

void
ch_binary_array_end(ch_binary_insert_handle * h)
{
	if (!h->array_active)
		return;
	ic_col	   *c = &h->cols[h->array_col_idx];

	if (c->array_depth == 0)
		return;

	uint64_t	end;

	/*
	 * At innermost depth, count is leaf-element count; at outer levels the
	 * count is total children written so far at the level below.
	 */
	if (c->array_depth == c->ndim)
	{
		if (c->layout == IC_ARRAY_FIXED || c->layout == IC_ARRAY_NESTED_FIXED)
			end = c->elem_size ? (uint64_t) (c->body.len / c->elem_size) : 0;
		else
			end = c->body_offs.len;
	}
	else
		end = c->arr_offs_inner[c->array_depth - 1].len;

	MemoryContext old = MemoryContextSwitchTo(h->cxt);

	if (c->array_depth == 1)
	{
		u64buf_push(&c->arr_offs, end);
		c->n_rows++;
		h->array_active = false;
	}
	else
		u64buf_push(&c->arr_offs_inner[c->array_depth - 2], end);
	MemoryContextSwitchTo(old);
	c->array_depth--;
}

bool
ch_binary_array_active(const ch_binary_insert_handle * h)
{
	return h && h->array_active;
}

chc_kind
ch_binary_column_kind(const ch_binary_insert_handle * h, size_t col)
{
	if (col >= h->ncols)
		return CHC_VOID;
	const		ic_col *c = &h->cols[col];

	if (h->array_active)
	{
		c = &h->cols[h->array_col_idx];

		/*
		 * While nested, surface CHC_ARRAY until the innermost layer is open;
		 * at that point return the leaf kind so encode targets scalars.
		 */
		if (c->array_depth < c->ndim)
			return CHC_ARRAY;
		return chc_type_kind(c->elem_t);
	}
	if (c->layout == IC_ARRAY_FIXED || c->layout == IC_ARRAY_STRING
		|| c->layout == IC_ARRAY_NESTED_FIXED || c->layout == IC_ARRAY_NESTED_STRING)
		return CHC_ARRAY;
	if (c->layout == IC_LC_STRING)
		return CHC_STRING;		/* PG side targets TEXT */
	return chc_type_kind(c->inner_t);
}

uint32_t
ch_binary_column_datetime64_precision(const ch_binary_insert_handle * h, size_t col)
{
	if (col >= h->ncols)
		return 0;
	return h->cols[col].dt64_precision;
}

/* Dedup map for LowCardinality dict */
typedef struct lcd_key
{
	const		uint8_t *bytes;
	size_t		len;
}			lcd_key;

typedef struct lcd_entry
{
	uint32		status;
	lcd_key		key;
	uint32		idx;
}			lcd_entry;

#define SH_PREFIX				lcd
#define SH_ELEMENT_TYPE			lcd_entry
#define SH_KEY_TYPE				lcd_key
#define SH_KEY					key
#define SH_HASH_KEY(tb, key)	hash_bytes((key).bytes, (int) (key).len)
#define SH_EQUAL(tb, a, b)		\
	((a).len == (b).len && memcmp((a).bytes, (b).bytes, (a).len) == 0)
#define SH_SCOPE				static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* Build LC dict (collect unique strings in insertion order) */
static void
build_lc_dict(ic_col * c, bool nullable,
			  uint64_t * *out_dict_offs, uint8_t * *out_dict_data,
			  size_t * out_dict_n, void **out_keys, int *out_key_size,
			  size_t * out_n_rows)
{
	size_t		n_rows = c->body_offs.len;
	uint64_t   *dict_offs = NULL;
	uint8_t    *dict_data = NULL;
	uint32_t   *keys = n_rows ? palloc(n_rows * sizeof(uint32_t)) : NULL;
	size_t		dict_n = 0;
	size_t		dict_cap = 0;
	size_t		data_len = 0;
	lcd_hash   *ht = n_rows
		? lcd_create(CurrentMemoryContext,
					 (uint32) Min(n_rows, (size_t) PG_UINT32_MAX), NULL)
		: NULL;

	if (nullable)
	{
		/* dict[0] = "" sentinel. */
		dict_cap = 8;
		dict_offs = palloc(dict_cap * sizeof(uint64_t));
		dict_offs[0] = 0;
		dict_n = 1;
	}

	for (size_t i = 0; i < n_rows; i++)
	{
		uint64_t	start = i == 0 ? 0 : c->body_offs.data[i - 1];
		uint64_t	end = c->body_offs.data[i];
		size_t		len = (size_t) (end - start);
		const		uint8_t *bytes = c->body.data + start;
		lcd_key		k = {bytes, len};
		lcd_entry  *entry;
		bool		found;

		/* If nullable and this row was null (signalled by null bit), key 0. */
		if (nullable && c->nulls.data[i])
		{
			keys[i] = 0;
			continue;
		}

		entry = lcd_insert(ht, k, &found);
		if (found)
		{
			keys[i] = entry->idx;
			continue;
		}

		if (dict_n == dict_cap)
		{
			dict_cap = dict_cap ? dict_cap * 2 : 64;
			dict_offs = dict_offs
				? repalloc(dict_offs, dict_cap * sizeof(uint64_t))
				: palloc(dict_cap * sizeof(uint64_t));
		}
		data_len += len;
		dict_offs[dict_n] = data_len;
		entry->idx = (uint32) dict_n;
		keys[i] = (uint32) dict_n;
		dict_n++;
	}

	if (data_len)
	{
		lcd_iterator it;
		lcd_entry  *e;

		dict_data = MemoryContextAllocExtended(CurrentMemoryContext, data_len,
											   MCXT_ALLOC_HUGE);
		lcd_start_iterate(ht, &it);
		while ((e = lcd_iterate(ht, &it)) != NULL)
		{
			uint64_t	s = e->idx == 0 ? 0 : dict_offs[e->idx - 1];

			memcpy(dict_data + s, e->key.bytes, e->key.len);
		}
	}
	if (ht)
		lcd_destroy(ht);
	*out_dict_offs = dict_offs;
	*out_dict_data = dict_data;
	*out_dict_n = dict_n;
	*out_keys = keys;
	*out_key_size = 4;
	*out_n_rows = n_rows;
}

void
ch_binary_flush_block(ch_binary_insert_handle * h)
{
	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	chc_err		err = {};
	chc_block_builder *bb = NULL;
	int			rc = chc_block_builder_init(&bb, &pg_chc_alloc, &err);

	if (rc != CHC_OK)
		raise_chc(&err, ERRCODE_FDW_ERROR, "could not append data to column - ");

	for (size_t i = 0; i < h->ncols; i++)
	{
		ic_col	   *c = &h->cols[i];
		const char *name = c->info.name ? c->info.name : "";
		size_t		nlen = strlen(name);

		switch (c->layout)
		{
			case IC_FIXED:
				{
					size_t		n_rows = c->elem_size ? c->body.len / c->elem_size : 0;

					if (c->is_nullable)
						rc = chc_block_builder_append_nullable_fixed(bb, name, nlen, c->t,
																	 c->nulls.data, c->body.data,
																	 n_rows, &err);
					else
						rc = chc_block_builder_append_fixed(bb, name, nlen, c->t,
															c->body.data, n_rows, &err);
					break;
				}
			case IC_STRING:
				{
					size_t		n_rows = c->body_offs.len;

					if (c->is_nullable)
						rc = chc_block_builder_append_nullable_string(bb, name, nlen, c->t,
																	  c->nulls.data,
																	  c->body_offs.data,
																	  c->body.data,
																	  n_rows, &err);
					else
						rc = chc_block_builder_append_string(bb, name, nlen,
															 c->body_offs.data,
															 c->body.data,
															 n_rows, &err);
					break;
				}
			case IC_JSON_STRING:
				{
					/*
					 * JSON columns share IC_STRING's per-row accumulator; the
					 * library emits the 8-byte SerializationObject::STRING
					 * prefix and writes rows as String-Binary.
					 */
					size_t		n_rows = c->body_offs.len;

					rc = chc_block_builder_append_json_string(bb, name, nlen, c->t,
															  c->body_offs.data,
															  c->body.data,
															  n_rows, &err);
					break;
				}
			case IC_LC_STRING:
				{
					bool		nullable_inner = chc_type_kind(chc_type_child(c->inner_t, 0)) == CHC_NULLABLE;
					size_t		dict_n,
								n_rows;
					int			key_size;
					uint64_t   *lc_offs;
					uint8_t    *lc_data;
					void	   *lc_keys;

					build_lc_dict(c, nullable_inner, &lc_offs, &lc_data,
								  &dict_n, &lc_keys, &key_size, &n_rows);
					rc = chc_block_builder_append_low_cardinality_string(bb, name, nlen, c->t,
																		 key_size, lc_keys,
																		 lc_offs, lc_data,
																		 dict_n, n_rows, &err);
					break;
				}
			case IC_ARRAY_FIXED:
				{
					size_t		n_rows = c->arr_offs.len;

					rc = chc_block_builder_append_array_fixed(bb, name, nlen, c->t,
															  c->arr_offs.data,
															  c->body.data,
															  n_rows, &err);
					break;
				}
			case IC_ARRAY_STRING:
				{
					size_t		n_rows = c->arr_offs.len;

					rc = chc_block_builder_append_array_string(bb, name, nlen, c->t,
															   c->arr_offs.data,
															   c->body_offs.data,
															   c->body.data,
															   n_rows, &err);
					break;
				}
			case IC_ARRAY_NESTED_FIXED:
			case IC_ARRAY_NESTED_STRING:
				{
					size_t		n_rows = c->arr_offs.len;
					const		uint64_t **lvl_offsets = palloc((size_t) c->ndim * sizeof(*lvl_offsets));
					size_t	   *lvl_lens = palloc((size_t) c->ndim * sizeof(*lvl_lens));

					lvl_offsets[0] = c->arr_offs.data;
					lvl_lens[0] = c->arr_offs.len;
					for (int lvl = 1; lvl < c->ndim; lvl++)
					{
						lvl_offsets[lvl] = c->arr_offs_inner[lvl - 1].data;
						lvl_lens[lvl] = c->arr_offs_inner[lvl - 1].len;
					}
					if (c->layout == IC_ARRAY_NESTED_FIXED)
						rc = chc_block_builder_append_array_nested_fixed(
																		 bb, name, nlen, c->t, c->ndim,
																		 lvl_offsets, lvl_lens,
																		 c->body.data, n_rows, &err);
					else
						rc = chc_block_builder_append_array_nested_string(
																		  bb, name, nlen, c->t, c->ndim,
																		  lvl_offsets, lvl_lens,
																		  c->body_offs.data, c->body.data,
																		  n_rows, &err);
					break;
				}
		}
		if (rc != CHC_OK)
			raise_chc(&err, ERRCODE_FDW_ERROR,
					  "could not append data to column - ");
	}

	rc = chc_client_send_data(h->client, bb, &err);
	chc_block_builder_destroy(bb);
	if (rc != CHC_OK)
	{
		if (h->state)
			h->state->broken = true;
		raise_chc(&err, ERRCODE_FDW_ERROR, "could not insert columns - ");
	}

	/* Reset per-column buffers for next batch. */
	for (size_t i = 0; i < h->ncols; i++)
	{
		ic_col	   *c = &h->cols[i];

		dynbuf_reset(&c->body);
		dynbuf_reset(&c->nulls);
		u64buf_reset(&c->body_offs);
		u64buf_reset(&c->arr_offs);
		if (c->arr_offs_inner)
		{
			for (int lvl = 0; lvl + 1 < c->ndim; lvl++)
				u64buf_reset(&c->arr_offs_inner[lvl]);
		}
		c->n_rows = 0;
	}
	MemoryContextSwitchTo(old);
}

/*
 * Send final empty Data + drain. May ereport on server exception or
 * transport failure. Idempotent via h->finalized. Leaves h->cxt alive;
 * ch_binary_release_insert deletes it.
 */
void
ch_binary_finalize_insert(ch_binary_insert_handle * h)
{
	if (!h || h->finalized)
		return;

	/*
	 * Set early so an ereport(ERROR) below still leaves h in the "do not
	 * touch the wire" state for the release callback.
	 */
	h->finalized = true;

	if (!(h->started && h->client))
		return;

	MemoryContext old = MemoryContextSwitchTo(h->cxt);
	char	   *exc_msg = NULL;
	bool		broke = false;
	chc_err		err = {};
	int			rc = chc_client_send_data(h->client, NULL, &err);

	if (rc != CHC_OK)
	{
		broke = true;
		exc_msg = pstrdup(err.msg[0] ? err.msg : "send_data failed");
	}
	else
	{
		/* Drain until EOS or exception. */
		for (;;)
		{
			chc_packet	pkt = {};

			err = (chc_err)
			{
				0
			};
			rc = chc_client_recv_packet(h->client, &pkt, &err);
			if (rc != CHC_OK)
			{
				broke = true;
				exc_msg = pstrdup(err.msg[0] ? err.msg : "recv_packet failed");
				chc_packet_clear(h->client, &pkt);
				break;
			}
			if (pkt.kind == CHC_PKT_EXCEPTION)
			{
				const char *msg = "server exception";

				if (pkt.exception && pkt.exception->display_text)
					msg = pkt.exception->display_text;
				else if (pkt.exception && pkt.exception->name)
					msg = pkt.exception->name;
				exc_msg = pstrdup(msg);
				broke = true;
				chc_packet_clear(h->client, &pkt);
				break;
			}
			chc_packet_clear(h->client, &pkt);
			if (pkt.kind == CHC_PKT_END_OF_STREAM)
				break;
		}
	}

	/*
	 * Server raised mid-INSERT and typically closes the socket; the next op
	 * would EPIPE. Mark broken so the cache rebuilds.
	 */
	if (broke && h->state)
		h->state->broken = true;

	MemoryContextSwitchTo(old);

	if (exc_msg)
	{
		/* exc_msg lives in h->cxt; copy into parent before raising. */
		char	   *parent_msg = pstrdup(exc_msg);

		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("pg_clickhouse: could not finish INSERT - %s", parent_msg)));
	}
}

/*
 * Teardown counterpart to finalize. Safe from a MemoryContext reset
 * callback: never raises, never talks to the server. If finalize did not
 * run (mid-query abort), flags the connection broken so it rebuilds on
 * next use.
 */
void
ch_binary_release_insert(ch_binary_insert_handle * h)
{
	if (!h)
		return;

	if (!h->finalized && h->started && h->state)
		h->state->broken = true;

	MemoryContextDelete(h->cxt);
}
