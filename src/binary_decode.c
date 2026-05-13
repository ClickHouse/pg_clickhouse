/*
 * binary_decode.c
 *
 * Build PG Datums from a ch_block_column produced by binary.cpp's
 * transcoder. Pure C — palloc / ereport / DirectFunctionCall live here, not
 * in the C++ wire TU.
 */

#include "postgres.h"

#include <math.h>
#include <string.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "funcapi.h"
#include "pgtime.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "binary.hh"
#include "binary_pg.h"
#include "ch_block.h"

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#endif

/* Read a little-endian fixed-width value from a raw byte stream. */
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

/* Locate a variable-length row in a (offsets, data) String layout. */
static inline void
slice_str(const ch_block_column * col, uint64_t row,
		  const char **out_ptr, size_t * out_len)
{
	uint64_t	start = row == 0 ? 0 : col->d.str.offsets[row - 1];
	uint64_t	end = col->d.str.offsets[row];

	*out_ptr = (const char *) col->d.str.data + start;
	*out_len = end - start;
}

static Oid
ch_kind_to_pg_oid(const ch_type * type)
{
	switch (type->kind)
	{
		case CH_T_VOID:
			return InvalidOid;
		case CH_T_NULLABLE:
			return ch_kind_to_pg_oid(type->child[0]);
		case CH_T_INT8:
		case CH_T_INT16:
		case CH_T_UINT8:
		case CH_T_BOOL:
			return INT2OID;
		case CH_T_INT32:
		case CH_T_UINT16:
			return INT4OID;
		case CH_T_INT64:
		case CH_T_UINT32:
		case CH_T_UINT64:
			return INT8OID;
		case CH_T_FLOAT32:
			return FLOAT4OID;
		case CH_T_FLOAT64:
			return FLOAT8OID;
		case CH_T_DECIMAL32:
		case CH_T_DECIMAL64:
		case CH_T_DECIMAL128:
		case CH_T_DECIMAL256:
			return NUMERICOID;
		case CH_T_STRING:
		case CH_T_FIXEDSTRING:
		case CH_T_ENUM8:
		case CH_T_ENUM16:
		case CH_T_LOWCARDINALITY:
			return TEXTOID;
		case CH_T_DATE:
		case CH_T_DATE32:
			return DATEOID;
		case CH_T_DATETIME:
		case CH_T_DATETIME64:
			return TIMESTAMPTZOID;
		case CH_T_UUID:
			return UUIDOID;
		case CH_T_IPV4:
		case CH_T_IPV6:
			return INETOID;
		case CH_T_ARRAY:
			return ANYARRAYOID;
		case CH_T_TUPLE:
			return RECORDOID;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported column type")));
	}
	/* unreachable */
	return InvalidOid;
}

static Datum binary_make_datum(const ch_block_column * col, uint64_t row,
							   Oid * valtype, bool *is_null);

/*
 * Read a single Decimal value (pre-formatted digit string with sign and
 * decimal point) and run it through numeric_in.
 */
static Datum
read_decimal(const ch_block_column * col, uint64_t row)
{
	const char *p;
	size_t		len;
	char	   *cstr;
	Datum		ret;

	slice_str(col, row, &p, &len);
	cstr = palloc(len + 1);
	memcpy(cstr, p, len);
	cstr[len] = '\0';
	ret = DirectFunctionCall3(numeric_in, CStringGetDatum(cstr),
							  ObjectIdGetDatum(0), Int32GetDatum(-1));
	pfree(cstr);
	return ret;
}

static Datum
read_string_as_text(const ch_block_column * col, uint64_t row)
{
	const char *p;
	size_t		len;

	slice_str(col, row, &p, &len);
	return PointerGetDatum(cstring_to_text_with_len(p, len));
}

static Datum
read_fixedstring_as_text(const ch_block_column * col, uint64_t row)
{
	uint32_t	width = col->type->fixed_size;
	const char *p = (const char *) col->d.raw + row * width;

	return PointerGetDatum(cstring_to_text_with_len(p, width));
}

static Datum
read_uuid(const ch_block_column * col, uint64_t row)
{
	pg_uuid_t  *u = (pg_uuid_t *) palloc(sizeof(pg_uuid_t));
	const		uint8_t *p = col->d.raw + row * 16;

	memcpy(u->data, p, 16);
	return UUIDPGetDatum(u);
}

static Datum
read_inet(const ch_block_column * col, uint64_t row)
{
	const char *p;
	size_t		len;
	char	   *cstr;
	Datum		ret;

	slice_str(col, row, &p, &len);
	cstr = palloc(len + 1);
	memcpy(cstr, p, len);
	cstr[len] = '\0';
	ret = DirectFunctionCall1(inet_in, CStringGetDatum(cstr));
	pfree(cstr);
	return ret;
}

static Datum
read_array(const ch_block_column * col, uint64_t row,
		   Oid * valtype, bool *is_null)
{
	uint64_t	start = row == 0 ? 0 : col->d.arr.offsets[row - 1];
	uint64_t	end = col->d.arr.offsets[row];
	uint64_t	len = end - start;
	ch_binary_array_t *slot = (ch_binary_array_t *) palloc(sizeof(ch_binary_array_t));
	const		ch_block_column *inner = col->d.arr.inner;
	const		ch_type *leaf = col->type;
	int			ndim = 0;

	/* postgres uses one array type per element type regardless of nesting,
	 * so walk past nested Array layers to the leaf scalar type. */
	while (leaf && leaf->kind == CH_T_ARRAY)
	{
		ndim++;
		leaf = leaf->n_child > 0 ? leaf->child[0] : NULL;
	}

	slot->len = len;
	slot->ndim = ndim;
	slot->item_type = leaf ? ch_kind_to_pg_oid(leaf) : InvalidOid;
	slot->array_type = get_array_type(slot->item_type);
	if (slot->array_type == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
				 errmsg("pg_clickhouse: could not find array type for column type %u",
						slot->item_type)));

	if (len > 0)
	{
		Oid			scratch;

		slot->datums = (Datum *) palloc0(sizeof(Datum) * len);
		slot->nulls = (bool *) palloc0(sizeof(bool) * len);

		/* For ndim==1 inner binary_make_datum returns leaf scalars; for
		 * ndim>1 inner is itself CH_T_ARRAY so recursion produces nested
		 * ch_binary_array_t* values. Use a scratch valtype to avoid
		 * clobbering slot->item_type. */
		for (uint64_t i = 0; i < len; ++i)
			slot->datums[i] = binary_make_datum(inner, start + i,
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
read_tuple(const ch_block_column * col, uint64_t row,
		   Oid * valtype, bool *is_null)
{
	size_t		n = col->type->n_child;
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

	for (size_t i = 0; i < n; ++i)
		slot->datums[i] = binary_make_datum(&col->d.tup.fields[i], row,
											&slot->types[i], &slot->nulls[i]);

	*valtype = RECORDOID;
	*is_null = false;
	return PointerGetDatum(slot);
}

static Datum
binary_make_datum(const ch_block_column * col, uint64_t row,
				  Oid * valtype, bool *is_null)
{
	*is_null = false;

	if (col->is_nullable && col->nulls && col->nulls[row])
	{
		*valtype = ch_kind_to_pg_oid(col->type);
		*is_null = true;
		return (Datum) 0;
	}

	switch (col->type->kind)
	{
		case CH_T_VOID:
			*valtype = InvalidOid;
			*is_null = true;
			return (Datum) 0;
		case CH_T_UINT8:
		case CH_T_BOOL:
			*valtype = INT2OID;
			return (Datum) (int16) rd_u8(col->d.raw, row);
		case CH_T_INT8:
			*valtype = INT2OID;
			return (Datum) (int16) rd_i8(col->d.raw, row);
		case CH_T_INT16:
			*valtype = INT2OID;
			return (Datum) rd_i16(col->d.raw, row);
		case CH_T_UINT16:

			/*
			 * Legacy binary FDW reads UInt16 via narrowing to int16, so 65535
			 * surfaces as -1 once the INT4OID Datum is interpreted. Cast
			 * through int16 to preserve that behavior.
			 */
			*valtype = INT4OID;
			return (Datum) (int32) (int16) rd_u16(col->d.raw, row);
		case CH_T_INT32:
			*valtype = INT4OID;
			return (Datum) rd_i32(col->d.raw, row);
		case CH_T_UINT32:
			*valtype = INT8OID;
			return Int64GetDatum((int64) rd_u32(col->d.raw, row));
		case CH_T_INT64:
			*valtype = INT8OID;
			return Int64GetDatum(rd_i64(col->d.raw, row));
		case CH_T_UINT64:
			{
				uint64_t	v = rd_u64(col->d.raw, row);

				if (v > (uint64_t) PG_INT64_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value " UINT64_FORMAT " is out of range of bigint",
									v)));
				*valtype = INT8OID;
				return Int64GetDatum((int64) v);
			}
		case CH_T_FLOAT32:
			*valtype = FLOAT4OID;
			return Float4GetDatum(rd_f32(col->d.raw, row));
		case CH_T_FLOAT64:
			*valtype = FLOAT8OID;
			return Float8GetDatum(rd_f64(col->d.raw, row));
		case CH_T_DECIMAL32:
		case CH_T_DECIMAL64:
		case CH_T_DECIMAL128:
		case CH_T_DECIMAL256:
			*valtype = NUMERICOID;
			return read_decimal(col, row);
		case CH_T_STRING:
		case CH_T_ENUM8:
		case CH_T_ENUM16:
		case CH_T_LOWCARDINALITY:
			*valtype = TEXTOID;
			return read_string_as_text(col, row);
		case CH_T_FIXEDSTRING:
			*valtype = TEXTOID;
			return read_fixedstring_as_text(col, row);
		case CH_T_DATE:
		case CH_T_DATE32:
			*valtype = DATEOID;
			return DirectFunctionCall1(timestamp_date,
									   time_t_to_timestamptz((pg_time_t) rd_i64(col->d.raw, row)));
		case CH_T_DATETIME:
			*valtype = TIMESTAMPTZOID;
			return TimestampTzGetDatum(time_t_to_timestamptz((pg_time_t) rd_i64(col->d.raw, row)));
		case CH_T_DATETIME64:
			{
				int64		raw = rd_i64(col->d.raw, row);
				int64		power = (int64) pow(10, col->type->scale);

				*valtype = TIMESTAMPTZOID;
				return TimestampTzGetDatum(time_t_to_timestamptz(raw / power))
					+ (raw % power) * (USECS_PER_SEC / power);
			}
		case CH_T_UUID:
			*valtype = UUIDOID;
			return read_uuid(col, row);
		case CH_T_IPV4:
		case CH_T_IPV6:
			*valtype = INETOID;
			return read_inet(col, row);
		case CH_T_ARRAY:
			return read_array(col, row, valtype, is_null);
		case CH_T_TUPLE:
			return read_tuple(col, row, valtype, is_null);
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
load_block(ch_binary_read_state_t * state, size_t idx)
{
	char		errbuf[CH_ERR_LEN] = {0};

	memset(&state->cur, 0, sizeof(state->cur));
	if (ch_binary_response_block_at(state->resp, idx, &state->cur, errbuf, sizeof(errbuf)) != 0)
	{
		state->error = pstrdup(errbuf[0] ? errbuf : "pg_clickhouse: failed to read block");
		state->done = true;
		state->cur_valid = false;
		return false;
	}
	state->cur_valid = true;
	return true;
}

void
ch_binary_read_state_init(ch_binary_read_state_t * state, ch_binary_response_t * resp)
{
	const char *resp_err;
	size_t		blocks;
	size_t		ncols;

	state->resp = resp;
	state->block = 0;
	state->row = 0;
	state->done = false;
	state->error = NULL;
	state->coltypes = NULL;
	state->values = NULL;
	state->nulls = NULL;
	state->cur_valid = false;
	memset(&state->cur, 0, sizeof(state->cur));

	resp_err = ch_binary_response_error(resp);
	if (resp_err)
	{
		state->done = true;
		state->error = pstrdup(resp_err);
		return;
	}

	blocks = ch_binary_response_block_count(resp);
	ncols = ch_binary_response_columns(resp);
	if (ncols == 0 || blocks == 0)
	{
		state->done = true;
		return;
	}

	state->coltypes = palloc0(sizeof(Oid) * ncols);
	state->values = palloc0(sizeof(Datum) * ncols);
	state->nulls = palloc0(sizeof(bool) * ncols);

	if (!load_block(state, 0))
		return;

	for (size_t i = 0; i < ncols; i++)
		state->coltypes[i] = ch_kind_to_pg_oid(state->cur.columns[i].type);
}

bool
ch_binary_read_row(ch_binary_read_state_t * state)
{
	size_t		blocks;
	size_t		ncols;

	if (state->done || state->coltypes == NULL || state->error)
		return false;

	blocks = ch_binary_response_block_count(state->resp);
	ncols = ch_binary_response_columns(state->resp);

again:
	if (!state->cur_valid)
	{
		if (state->block >= blocks)
		{
			state->done = true;
			return false;
		}
		if (!load_block(state, state->block))
			return false;
	}

	if (state->row >= state->cur.num_rows)
	{
		state->row = 0;
		state->block++;
		state->cur_valid = false;
		if (state->block >= blocks)
		{
			state->done = true;
			return false;
		}
		goto again;
	}

	PG_TRY();
	{
		for (size_t i = 0; i < ncols; i++)
		{
			Oid			t;

			state->values[i] = binary_make_datum(&state->cur.columns[i], state->row,
												 &t, &state->nulls[i]);
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
