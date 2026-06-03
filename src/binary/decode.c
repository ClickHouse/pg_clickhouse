/*
 * decode.c
 *
 * Build PG Datums from a chc_column produced by clickhouse-c. Walks the
 * wire-shaped column accessors directly; per-row transforms (decimal to
 * text, IPv4/IPv6 text, UUID byteswap, enum name lookup, LowCardinality
 * key->dict deref, Nullable strip) happen inline at read time.
 */

#include "postgres.h"

#include <string.h>
#include <sys/socket.h>			/* AF_INET, expanded by PG inet macros */

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "port/pg_bswap.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "binary_internal.h"

/* CH Date / Date32 epoch is unix; offset to PG epoch (2000-01-01) */
#define CH_TO_PG_DATE_OFFSET (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE)

/* Little-endian fixed-width reads at row offset. */
static inline int8_t
rd_i8(const uint8_t * p, uint64_t row)
{
	return (int8_t) p[row];
}

static inline uint8_t
rd_u8(const uint8_t * p, uint64_t row)
{
	return p[row];
}

static inline bool
rd_bool(const bool *p, uint64_t row)
{
	return (bool) p[row];
}

static inline int16_t
rd_i16(const uint8_t * p, uint64_t row)
{
	int16_t		v;

	memcpy(&v, p + row * sizeof(int16_t), sizeof(int16_t));
	return v;
}

static inline uint16_t
rd_u16(const uint8_t * p, uint64_t row)
{
	uint16_t	v;

	memcpy(&v, p + row * sizeof(uint16_t), sizeof(uint16_t));
	return v;
}

static inline int32_t
rd_i32(const uint8_t * p, uint64_t row)
{
	int32_t		v;

	memcpy(&v, p + row * sizeof(int32_t), sizeof(int32_t));
	return v;
}

static inline uint32_t
rd_u32(const uint8_t * p, uint64_t row)
{
	uint32_t	v;

	memcpy(&v, p + row * sizeof(uint32_t), sizeof(uint32_t));
	return v;
}

static inline int64_t
rd_i64(const uint8_t * p, uint64_t row)
{
	int64_t		v;

	memcpy(&v, p + row * sizeof(int64_t), sizeof(int64_t));
	return v;
}

static inline uint64_t
rd_u64(const uint8_t * p, uint64_t row)
{
	uint64_t	v;

	memcpy(&v, p + row * sizeof(uint64_t), sizeof(uint64_t));
	return v;
}

static inline float
rd_f32(const uint8_t * p, uint64_t row)
{
	float		v;

	memcpy(&v, p + row * sizeof(float), sizeof(float));
	return v;
}

static inline double
rd_f64(const uint8_t * p, uint64_t row)
{
	double		v;

	memcpy(&v, p + row * sizeof(double), sizeof(double));
	return v;
}

static inline void
slice_str(const chc_column * col, uint64_t row,
		  const char **out_ptr, size_t * out_len)
{
	const		uint64_t *offs = chc_column_string_offsets(col);
	const		uint8_t *data = chc_column_string_data(col);
	uint64_t	start = row == 0 ? 0 : offs[row - 1];
	uint64_t	end = offs[row];

	*out_ptr = (const char *) data + start;
	*out_len = (size_t) (end - start);
}

static Oid
ch_kind_to_pg_oid(const chc_type * type)
{
	switch (chc_type_kind(type))
	{
		case CHC_VOID:
		case CHC_NOTHING:
			return InvalidOid;
		case CHC_NULLABLE:
		case CHC_LOW_CARDINALITY:
			return ch_kind_to_pg_oid(chc_type_child(type, 0));
		case CHC_INT8:
		case CHC_INT16:
		case CHC_UINT8:
			return INT2OID;
		case CHC_BOOL:
			return BOOLOID;
		case CHC_INT32:
		case CHC_UINT16:
			return INT4OID;
		case CHC_INT64:
		case CHC_UINT32:
		case CHC_UINT64:
			return INT8OID;
		case CHC_FLOAT32:
			return FLOAT4OID;
		case CHC_FLOAT64:
			return FLOAT8OID;
		case CHC_DECIMAL32:
		case CHC_DECIMAL64:
		case CHC_DECIMAL128:
		case CHC_DECIMAL256:
			return NUMERICOID;
		case CHC_STRING:
		case CHC_FIXED_STRING:
		case CHC_ENUM8:
		case CHC_ENUM16:
			return TEXTOID;
		case CHC_JSON:
		case CHC_OBJECT:
			return JSONBOID;
		case CHC_DATE:
		case CHC_DATE32:
			return DATEOID;
		case CHC_DATETIME:
		case CHC_DATETIME64:
			return TIMESTAMPTZOID;
		case CHC_UUID:
			return UUIDOID;
		case CHC_IPV4:
		case CHC_IPV6:
			return INETOID;
		case CHC_ARRAY:
			return ANYARRAYOID;
		case CHC_TUPLE:
			return RECORDOID;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported column type")));
	}
	/* unreachable */
	return InvalidOid;
}

static Datum read_value(const chc_column * col, const chc_type * type,
						uint64_t row, Oid * valtype, bool *is_null);

/*
 * Format a ClickHouse Decimal (two's-complement signed integer in LE bytes of
 * width 4/8/16/32 for Decimal32/64/128/256, with `scale` fractional digits
 * carried on the column type) into `out`. Returns bytes written, -1 on overflow.
 */
static int
format_decimal_text(const uint8_t * bytes, size_t width, uint32_t scale,
					char *out, size_t out_cap)
{
	uint32_t	mag[8];
	size_t		nwords = width / 4;
	bool		neg = false;

	if (width == 0 || width > 32 || width % 4 != 0)
		return -1;
	memcpy(mag, bytes, width);
	/* top bit of MSW is sign; negate two's-complement to get magnitude */
	if (mag[nwords - 1] & 0x80000000u)
	{
		neg = true;
		for (size_t i = 0; i < nwords; i++)
			mag[i] = ~mag[i];
		uint64_t	carry = 1;

		for (size_t i = 0; i < nwords && carry; i++)
		{
			uint64_t	v = (uint64_t) mag[i] + carry;

			mag[i] = (uint32_t) v;
			carry = v >> 32;
		}
	}

	char		buf[80];
	int			n = 0;
	bool		nonzero;

	/* base-10 division of mag yields digits LSB-first */
	do
	{
		uint64_t	rem = 0;

		nonzero = false;
		for (ssize_t i = (ssize_t) nwords - 1; i >= 0; i--)
		{
			uint64_t	v = (rem << 32) | mag[i];

			mag[i] = (uint32_t) (v / 10);
			rem = v % 10;
			if (mag[i])
				nonzero = true;
		}
		buf[n++] = (char) ('0' + (uint32_t) rem);
	} while (nonzero && n < (int) sizeof(buf));

	/* pad leading zeros so digit count covers fractional portion */
	while (n <= (int) scale)
		buf[n++] = '0';

	size_t		need = (size_t) neg + (size_t) n + (scale ? 1 : 0) + 1;

	if (need > out_cap)
		return -1;
	char	   *p = out;

	if (neg)
		*p++ = '-';

	/* emit MSD-first, inserting '.' before `scale` trailing digits */
	for (int i = n - 1; i >= 0; i--)
	{
		if (i + 1 == scale)
			*p++ = '.';
		*p++ = buf[i];
	}
	*p = '\0';
	return (int) (p - out);
}

static Datum
read_decimal(const chc_column * col, const chc_type * type, uint64_t row)
{
	size_t		es;
	const		uint8_t *p = (const uint8_t *) chc_column_fixed_data(col, &es);
	uint32_t	scale = (uint32_t) chc_type_decimal_scale(type);
	char		buf[80];
	int			rc;

#if PG_VERSION_NUM >= 140000
	/* Decimal32/64 fit in int64; skip the byte-array text path. */
	if (es == 4)
		return NumericGetDatum(int64_div_fast_to_numeric(rd_i32(p, row), (int) scale));
	if (es == 8)
		return NumericGetDatum(int64_div_fast_to_numeric(rd_i64(p, row), (int) scale));
#endif

	rc = format_decimal_text(p + row * es, es, scale, buf, sizeof(buf));
	if (rc < 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("pg_clickhouse: decimal too wide")));
	return DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
							   ObjectIdGetDatum(0), Int32GetDatum(-1));
}

static Datum
read_string_as_text(const chc_column * col, uint64_t row)
{
	const char *p;
	size_t		len;

	slice_str(col, row, &p, &len);
	return PointerGetDatum(cstring_to_text_with_len(p, len));
}

static Datum
read_fixedstring_as_text(const chc_column * col, uint64_t row)
{
	size_t		width;
	const		uint8_t *base = chc_column_fixed_data(col, &width);

	return PointerGetDatum(cstring_to_text_with_len((const char *) base + row * width,
													width));
}

static Datum
read_uuid(const chc_column * col, uint64_t row)
{
	pg_uuid_t  *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
	const		uint8_t *p = (uint8_t *) chc_column_fixed_data(col, NULL) + row * 16;
	uint64_t	a,
				b;

	memcpy(&a, p, 8);
	memcpy(&b, p + 8, 8);
	a = pg_hton64(a);
	b = pg_hton64(b);
	memcpy(u->data, &a, 8);
	memcpy(u->data + 8, &b, 8);
	return UUIDPGetDatum(u);
}

/*
 * IPv4 wire format: native uint32 (LE on supported hosts). PG inet wants
 * BE bytes of dotted-quad, so pg_hton32 the value into ip_addr directly.
 */
static Datum
read_ipv4(const chc_column * col, uint64_t row)
{
	inet	   *res = (inet *) palloc0(sizeof(inet));
	uint32_t	addr;

	memcpy(&addr, (const uint8_t *) chc_column_fixed_data(col, NULL) + row * 4, 4);
	addr = pg_hton32(addr);
	ip_family(res) = PGSQL_AF_INET;
	ip_bits(res) = 32;
	memcpy(ip_addr(res), &addr, 4);
	SET_INET_VARSIZE(res);
	return InetPGetDatum(res);
}

/* IPv6 wire is already network order; same layout as PG inet ip_addr. */
static Datum
read_ipv6(const chc_column * col, uint64_t row)
{
	inet	   *res = (inet *) palloc0(sizeof(inet));

	ip_family(res) = PGSQL_AF_INET6;
	ip_bits(res) = 128;
	memcpy(ip_addr(res), (const uint8_t *) chc_column_fixed_data(col, NULL) + row * 16, 16);
	SET_INET_VARSIZE(res);
	return InetPGetDatum(res);
}

static Datum
read_enum_as_text(const chc_column * col, const chc_type * type, uint64_t row)
{
	size_t		es;
	const		uint8_t *p = (const uint8_t *) chc_column_fixed_data(col, &es);
	int64_t		v = 0;

	if (es == 1)
		v = (int8_t) p[row];
	else
	{
		int16_t		t;

		memcpy(&t, p + row * 2, 2);
		v = t;
	}

	size_t		n = chc_type_enum_count(type);

	for (size_t i = 0; i < n; i++)
	{
		const char *en;
		size_t		el;
		int64_t		ev;

		chc_type_enum_at(type, i, &en, &el, &ev);
		if (ev == v)
			return PointerGetDatum(cstring_to_text_with_len(en ? en : "", el));
	}
	return PointerGetDatum(cstring_to_text_with_len("", 0));
}

/*
 * JSON body bytes are STRING-serialized JSON document text. Run them
 * through json_in / jsonb_in depending on the caller's target valtype
 * (set by ch_kind_to_pg_oid to JSONBOID, overridable when the foreign
 * table declares the column as `json`).
 */
static Datum
read_json(const chc_column * col, uint64_t row, Oid valtype)
{
	const char *p;
	size_t		len;
	char	   *cstr;
	Datum		ret;

	slice_str(col, row, &p, &len);
	cstr = palloc(len + 1);
	memcpy(cstr, p, len);
	cstr[len] = '\0';
	ret = DirectFunctionCall1(valtype == JSONOID ? json_in : jsonb_in,
							  CStringGetDatum(cstr));
	pfree(cstr);
	return ret;
}

/*
 * LowCardinality(String) or LowCardinality(Nullable(String)). Key for
 * row indexes into the dict column; the dict's first slot is the null
 * sentinel for the Nullable variant.
 */
static Datum
read_lc_string(const chc_column * col, const chc_type * type, uint64_t row,
			   Oid * valtype, bool *is_null)
{
	int			ks = chc_column_lc_key_size(col);
	const		uint8_t *kp = (const uint8_t *) chc_column_lc_keys(col) + (size_t) row * ks;
	uint64_t	k = 0;

	switch (ks)
	{
		case 1:
			k = kp[0];
			break;
		case 2:
			{
				uint16_t	v;

				memcpy(&v, kp, 2);
				k = v;
				break;
			}
		case 4:
			{
				uint32_t	v;

				memcpy(&v, kp, 4);
				k = v;
				break;
			}
		case 8:
			memcpy(&k, kp, 8);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_ERROR),
					 errmsg("pg_clickhouse: unexpected LowCardinality key size %d", ks)));
	}

	const		chc_column *dict = chc_column_lc_dict(col);
	const		chc_type *inner_t = chc_type_child(type, 0);

	if (chc_type_kind(inner_t) == CHC_NULLABLE
		&& chc_column_layout(dict) == CHC_COL_NULLABLE)
	{
		const		uint8_t *dnm = chc_column_null_map(dict);

		if (dnm && dnm[k])
		{
			*valtype = TEXTOID;
			*is_null = true;
			return (Datum) 0;
		}
		dict = chc_column_nullable_inner(dict);
	}

	*valtype = TEXTOID;
	*is_null = false;
	if (chc_column_layout(dict) != CHC_COL_STRING)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("pg_clickhouse: unsupported LowCardinality inner type")));
	return read_string_as_text(dict, k);
}

static Datum
read_array(const chc_column * col, const chc_type * type, uint64_t row,
		   Oid * valtype, bool *is_null)
{
	const		uint64_t *offs = chc_column_array_offsets(col);
	uint64_t	start = row == 0 ? 0 : offs[row - 1];
	uint64_t	end = offs[row];
	uint64_t	len = end - start;
	const		chc_type *inner_t = chc_type_child(type, 0);
	const		chc_column *inner = chc_column_array_values(col);
	ch_binary_array_t *slot = (ch_binary_array_t *) palloc(sizeof(ch_binary_array_t));
	const		chc_type *leaf = type;
	int			ndim = 0;

	/*
	 * postgres uses one array type per element type regardless of nesting, so
	 * walk past nested Array layers to the leaf scalar type.
	 */
	while (chc_type_kind(leaf) == CHC_ARRAY)
	{
		ndim++;
		leaf = chc_type_child(leaf, 0);
	}

	slot->len = len;
	slot->ndim = ndim;
	slot->item_type = ch_kind_to_pg_oid(leaf);
	slot->array_type = get_array_type(slot->item_type);
	if (slot->array_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("pg_clickhouse: could not find array type for column type \"%s\"",
						chc_type_name(leaf, NULL))));

	if (len > 0)
	{
		Oid			scratch = slot->item_type;

		slot->datums = (Datum *) palloc0(sizeof(Datum) * len);
		slot->nulls = (bool *) palloc0(sizeof(bool) * len);

		/*
		 * For ndim==1 read_value returns leaf scalars; for ndim>1 inner_t is
		 * itself CHC_ARRAY so recursion produces nested ch_binary_array_t*.
		 * Use a scratch valtype to avoid clobbering slot->item_type.
		 */
		for (uint64_t i = 0; i < len; ++i)
			slot->datums[i] = read_value(inner, inner_t, start + i,
										 &scratch, &slot->nulls[i]);
	}
	else
	{
		slot->datums = NULL;
		slot->nulls = NULL;
	}

	*valtype = ANYARRAYOID;
	*is_null = false;
	return PointerGetDatum(slot);
}

static Datum
read_tuple(const chc_column * col, const chc_type * type, uint64_t row,
		   Oid * valtype, bool *is_null)
{
	size_t		n = chc_type_n_children(type);
	ch_binary_tuple_t *slot;

	if (n == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("pg_clickhouse: returned tuple is empty")));

	slot = (ch_binary_tuple_t *) palloc(sizeof(ch_binary_tuple_t));
	slot->datums = (Datum *) palloc(sizeof(Datum) * n);
	slot->nulls = (bool *) palloc0(sizeof(bool) * n);
	slot->types = (Oid *) palloc0(sizeof(Oid) * n);
	slot->len = n;
	slot->ch_type_name = chc_type_name(type, NULL);

	for (size_t i = 0; i < n; ++i)
	{
		const		chc_type *ft = chc_type_child(type, i);
		const		chc_column *fc = chc_column_tuple_child(col, i);

		slot->datums[i] = read_value(fc, ft, row, &slot->types[i], &slot->nulls[i]);
	}

	*valtype = RECORDOID;
	*is_null = false;
	return PointerGetDatum(slot);
}

static Datum
read_value(const chc_column * col, const chc_type * type, uint64_t row,
		   Oid * valtype, bool *is_null)
{
	/* Unwrap outer Nullable, handling nulls here. */
	if (chc_type_kind(type) == CHC_NULLABLE)
	{
		const		chc_type *inner_t = chc_type_child(type, 0);

		if (chc_column_layout(col) == CHC_COL_NULLABLE)
		{
			const		uint8_t *nm = chc_column_null_map(col);

			if (nm && nm[row])
			{
				*valtype = ch_kind_to_pg_oid(inner_t);
				*is_null = true;
				return (Datum) 0;
			}
			col = chc_column_nullable_inner(col);
		}
		type = inner_t;
	}
	*is_null = false;

	switch (chc_type_kind(type))
	{
		case CHC_VOID:
		case CHC_NOTHING:
			*valtype = InvalidOid;
			*is_null = true;
			return (Datum) 0;
		case CHC_UINT8:
			*valtype = INT2OID;
			return (Datum) rd_u8((const uint8_t *) chc_column_fixed_data(col, NULL), row);
		case CHC_BOOL:
			*valtype = BOOLOID;
			return (Datum) rd_bool((const bool *) chc_column_fixed_data(col, NULL), row);
		case CHC_INT8:
			*valtype = INT2OID;
			return (Datum) rd_i8((const uint8_t *) chc_column_fixed_data(col, NULL), row);
		case CHC_INT16:
			*valtype = INT2OID;
			return (Datum) rd_i16((const uint8_t *) chc_column_fixed_data(col, NULL), row);
		case CHC_UINT16:
			*valtype = INT4OID;
			return (Datum) rd_u16((const uint8_t *) chc_column_fixed_data(col, NULL), row);
		case CHC_INT32:
			*valtype = INT4OID;
			return (Datum) rd_i32((const uint8_t *) chc_column_fixed_data(col, NULL), row);
		case CHC_UINT32:
			*valtype = INT8OID;
			return Int64GetDatum((int64) rd_u32((const uint8_t *) chc_column_fixed_data(col, NULL), row));
		case CHC_INT64:
			*valtype = INT8OID;
			return Int64GetDatum(rd_i64((const uint8_t *) chc_column_fixed_data(col, NULL), row));
		case CHC_UINT64:
			{
				uint64_t	v = rd_u64((const uint8_t *) chc_column_fixed_data(col, NULL), row);

				if (v > (uint64_t) PG_INT64_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value " UINT64_FORMAT " is out of range of bigint",
									v)));
				*valtype = INT8OID;
				return Int64GetDatum((int64) v);
			}
		case CHC_FLOAT32:
			*valtype = FLOAT4OID;
			return Float4GetDatum(rd_f32((const uint8_t *) chc_column_fixed_data(col, NULL), row));
		case CHC_FLOAT64:
			*valtype = FLOAT8OID;
			return Float8GetDatum(rd_f64((const uint8_t *) chc_column_fixed_data(col, NULL), row));
		case CHC_DECIMAL32:
		case CHC_DECIMAL64:
		case CHC_DECIMAL128:
		case CHC_DECIMAL256:
			*valtype = NUMERICOID;
			return read_decimal(col, type, row);
		case CHC_STRING:
			*valtype = TEXTOID;
			return read_string_as_text(col, row);
		case CHC_ENUM8:
		case CHC_ENUM16:
			*valtype = TEXTOID;
			return read_enum_as_text(col, type, row);
		case CHC_JSON:
		case CHC_OBJECT:
			{
				/*
				 * *valtype arrives set to JSONBOID by default (from
				 * ch_kind_to_pg_oid) but the caller's foreign-table
				 * declaration may have requested JSONOID; honor it.
				 */
				Oid			target = (*valtype == JSONOID) ? JSONOID : JSONBOID;

				*valtype = target;
				return read_json(col, row, target);
			}
		case CHC_FIXED_STRING:
			*valtype = TEXTOID;
			return read_fixedstring_as_text(col, row);
		case CHC_DATE:
			*valtype = DATEOID;
			return DateADTGetDatum((DateADT) rd_u16((const uint8_t *) chc_column_fixed_data(col, NULL), row)
								   - CH_TO_PG_DATE_OFFSET);
		case CHC_DATE32:
			*valtype = DATEOID;
			return DateADTGetDatum((DateADT) rd_i32((const uint8_t *) chc_column_fixed_data(col, NULL), row)
								   - CH_TO_PG_DATE_OFFSET);
		case CHC_DATETIME:
			{
				uint32_t	secs = rd_u32((const uint8_t *) chc_column_fixed_data(col, NULL), row);

				*valtype = TIMESTAMPTZOID;
				return TimestampTzGetDatum(time_t_to_timestamptz((pg_time_t) secs));
			}
		case CHC_DATETIME64:
			{
				int64		raw = rd_i64((const uint8_t *) chc_column_fixed_data(col, NULL), row);
				uint32_t	scale = chc_type_datetime64_scale(type);
				int64		power = pow10i[scale];

				*valtype = TIMESTAMPTZOID;
				/* multiply before divide so scale > 6 keeps sub-second us */
				return TimestampTzGetDatum(time_t_to_timestamptz(raw / power)
										   + (raw % power) * USECS_PER_SEC / power);
			}
		case CHC_UUID:
			*valtype = UUIDOID;
			return read_uuid(col, row);
		case CHC_IPV4:
			*valtype = INETOID;
			return read_ipv4(col, row);
		case CHC_IPV6:
			*valtype = INETOID;
			return read_ipv6(col, row);
		case CHC_LOW_CARDINALITY:
			return read_lc_string(col, type, row, valtype, is_null);
		case CHC_ARRAY:
			return read_array(col, type, row, valtype, is_null);
		case CHC_TUPLE:
			return read_tuple(col, type, row, valtype, is_null);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported type in binary protocol")));
	}
	/* unreachable */
	return (Datum) 0;
}

/* ---- read state ----------------------------------------------------- */

static bool
load_block(ch_binary_read_state_t * state)
{
	state->cur = ch_binary_response_fetch_next_block(state->resp);
	if (state->cur == NULL)
	{
		const char *resp_err = ch_binary_response_error(state->resp);

		if (resp_err)
			state->error = pstrdup(resp_err);
		state->done = true;
		return false;
	}
	return true;
}

void
ch_binary_read_state_init(ch_binary_read_state_t * state, ch_binary_response_t * resp)
{
	const char *resp_err;
	size_t		ncols;

	state->resp = resp;
	state->block = 0;
	state->row = 0;
	state->done = false;
	state->error = NULL;
	state->coltypes = NULL;
	state->values = NULL;
	state->nulls = NULL;
	state->cur = NULL;

	resp_err = ch_binary_response_error(resp);
	if (resp_err)
	{
		state->done = true;
		state->error = pstrdup(resp_err);
		return;
	}

	ncols = ch_binary_response_columns(resp);
	if (ncols == 0)
	{
		state->done = true;
		return;
	}

	state->coltypes = palloc0(sizeof(Oid) * ncols);
	state->values = palloc0(sizeof(Datum) * ncols);
	state->nulls = palloc0(sizeof(bool) * ncols);

	if (!load_block(state))
		return;

	for (size_t i = 0; i < ncols; i++)
		state->coltypes[i] = ch_kind_to_pg_oid(chc_block_column_type(state->cur, i));
}

bool
ch_binary_read_row(ch_binary_read_state_t * state)
{
	size_t		ncols;

	if (state->done || state->coltypes == NULL || state->error)
		return false;

	ncols = ch_binary_response_columns(state->resp);

again:
	if (state->cur == NULL)
	{
		if (!load_block(state))
			return false;
	}

	if (state->row >= chc_block_n_rows(state->cur))
	{
		state->row = 0;
		state->cur = NULL;
		goto again;
	}

	PG_TRY();
	{
		for (size_t i = 0; i < ncols; i++)
		{
			/*
			 * Currently, read_value overwrites *valtype for most types
			 * canonical PG type for that CH kind. For CHC_JSON we honor the
			 * incoming value so callers (binary_fetch_row) can pin the Datum
			 * type to JSONOID for `data json` foreign columns and avoid the
			 * jsonb_in -> jsonb_out round-trip that would strip CH's verbatim
			 * STRING- serialized formatting.
			 */
			Oid			t = state->coltypes[i];
			const		chc_column *col = chc_block_column(state->cur, i);
			const		chc_type *ct = chc_block_column_type(state->cur, i);

			state->values[i] = read_value(col, ct, state->row, &t, &state->nulls[i]);
		}
	}
	PG_CATCH();
	{
		MemoryContext mcxt = GetMemoryChunkContext(state);
		MemoryContext oldcxt;
		ErrorData  *edata;
		const char *msg;
		static const char prefix[] = "pg_clickhouse: ";

		oldcxt = MemoryContextSwitchTo(mcxt);
		edata = CopyErrorData();
		msg = edata->message ? edata->message : "unknown error";

		/*
		 * binary_fetch_row re-prefixes with "pg_clickhouse: error while
		 * reading row:"; drop a leading "pg_clickhouse: " here so the final
		 * message doesn't carry it twice.
		 */
		if (strncmp(msg, prefix, sizeof(prefix) - 1) == 0)
			msg += sizeof(prefix) - 1;
		state->error = pstrdup(msg);
		FlushErrorState();
		FreeErrorData(edata);
		MemoryContextSwitchTo(oldcxt);
		state->done = true;
		return false;
	}
	PG_END_TRY();

	state->row++;
	return true;
}

void
ch_binary_read_state_free(ch_binary_read_state_t * state)
{
	/* state->error is palloc'd; freed with surrounding memory context. */
	state->error = NULL;
}
