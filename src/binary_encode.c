/*
 * binary_encode.c
 *
 * PG-side INSERT path. Reads PG Datums and dispatches into the typed
 * ch_binary_append_* shims exposed by binary.cpp. No C++.
 */

#include "postgres.h"

#include <math.h>
#include <string.h>

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "pgtime.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "binary.hh"
#include "binary_pg.h"
#include "ch_block.h"

static void
raise_errbuf(const char *prefix, const char *errbuf)
{
	bool		have_msg = (errbuf && errbuf[0] != '\0');

	ereport(ERROR,
			(errcode(ERRCODE_FDW_ERROR),
			 errmsg("pg_clickhouse: %s%s%s",
					prefix,
					have_msg ? " - " : "",
					have_msg ? errbuf : "")));
}

/*
 * Map a CH column kind to the PG type our import path uses. Used when
 * constructing the TupleDesc for INSERT...VALUES, mirroring the legacy
 * get_corr_postgres_type from binary.cpp.
 */
static Oid
ch_kind_to_pg_oid_for_insert(const ch_type * type, const char *colname)
{
	switch (type->kind)
	{
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
			{
				/* postgres uses one array type per element type regardless of
				 * nesting, so unwrap nested Array layers before looking up. */
				const		ch_type *leaf = type;
				Oid			item_type;
				Oid			array_type;

				while (leaf->kind == CH_T_ARRAY)
					leaf = leaf->n_child > 0 ? leaf->child[0] : leaf;
				item_type = ch_kind_to_pg_oid_for_insert(leaf, colname);
				array_type = get_array_type(item_type);

				if (array_type == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							 errmsg("pg_clickhouse: could not find array type for column \"%s\"",
									colname ? colname : "?")));
				return array_type;
			}
		case CH_T_TUPLE:
			return RECORDOID;
		case CH_T_NULLABLE:
			return ch_kind_to_pg_oid_for_insert(type->child[0], colname);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported column type for \"%s\"",
							colname ? colname : "?")));
	}
	return InvalidOid;
}

void
ch_binary_prepare_insert(void *conn, const ch_query * query,
						 ch_binary_insert_state * state)
{
	char		errbuf[CH_ERR_LEN] = {0};
	ch_binary_column_info *cols = NULL;
	size_t		n = 0;
	ch_binary_insert_handle *h;

	h = ch_binary_begin_insert((ch_binary_connection_t *) conn, query,
							   &cols, &n, errbuf, sizeof(errbuf));
	if (h == NULL)
		raise_errbuf("could not prepare insert", errbuf);

	state->len = n;
	state->insert_block = h;

	if (n == 0)
		return;

	state->outdesc = CreateTemplateTupleDesc(n);
	for (size_t i = 0; i < n; i++)
	{
		Oid			pg_type = ch_kind_to_pg_oid_for_insert(cols[i].type, cols[i].name);

		TupleDescInitEntry(state->outdesc, (AttrNumber) (i + 1),
						   cols[i].name ? cols[i].name : "",
						   pg_type, -1, 0);
	}
}

/*
 * Append a single value (already extracted from a Datum + isnull) into the
 * current column, dispatching on (PG Oid, CH kind).
 */
static void
append_one(ch_binary_insert_handle * h, size_t colidx,
		   ch_type_kind kind, Datum val, Oid valtype, bool isnull)
{
	char		errbuf[CH_ERR_LEN] = {0};
	int			rc = 0;

	switch (valtype)
	{
		case INT2OID:
			switch (kind)
			{
				case CH_T_UINT8:
				case CH_T_BOOL:
					rc = ch_binary_append_uint(h, colidx, (uint64_t) (uint16) DatumGetInt16(val),
											   isnull, errbuf, sizeof(errbuf));
					break;
				case CH_T_INT8:
				case CH_T_INT16:
					rc = ch_binary_append_int(h, colidx, (int64_t) DatumGetInt16(val),
											  isnull, errbuf, sizeof(errbuf));
					break;
				default:
					goto type_mismatch;
			}
			break;
		case INT4OID:
			switch (kind)
			{
				case CH_T_INT32:
					rc = ch_binary_append_int(h, colidx, (int64_t) DatumGetInt32(val),
											  isnull, errbuf, sizeof(errbuf));
					break;
				case CH_T_UINT16:
					rc = ch_binary_append_uint(h, colidx, (uint64_t) (uint16) DatumGetInt32(val),
											   isnull, errbuf, sizeof(errbuf));
					break;
				default:
					goto type_mismatch;
			}
			break;
		case INT8OID:
			switch (kind)
			{
				case CH_T_INT64:
					rc = ch_binary_append_int(h, colidx, DatumGetInt64(val),
											  isnull, errbuf, sizeof(errbuf));
					break;
				case CH_T_UINT32:
					rc = ch_binary_append_uint(h, colidx,
											   (uint64_t) (uint32) DatumGetInt64(val),
											   isnull, errbuf, sizeof(errbuf));
					break;
				case CH_T_UINT64:
					rc = ch_binary_append_uint(h, colidx, (uint64_t) DatumGetInt64(val),
											   isnull, errbuf, sizeof(errbuf));
					break;
				default:
					goto type_mismatch;
			}
			break;
		case FLOAT4OID:
			if (kind != CH_T_FLOAT32)
				goto type_mismatch;
			rc = ch_binary_append_float(h, colidx, DatumGetFloat4(val),
										isnull, errbuf, sizeof(errbuf));
			break;
		case FLOAT8OID:
			if (kind != CH_T_FLOAT64)
				goto type_mismatch;
			rc = ch_binary_append_double(h, colidx, DatumGetFloat8(val),
										 isnull, errbuf, sizeof(errbuf));
			break;
		case NUMERICOID:
			{
				char	   *s = NULL;

				if (kind != CH_T_DECIMAL32 && kind != CH_T_DECIMAL64
					&& kind != CH_T_DECIMAL128 && kind != CH_T_DECIMAL256)
					goto type_mismatch;
				if (!isnull)
					s = DatumGetCString(DirectFunctionCall1(numeric_out, val));
				rc = ch_binary_append_decimal(h, colidx, s, isnull, errbuf, sizeof(errbuf));
				if (s)
					pfree(s);
				break;
			}
		case TEXTOID:
			{
				const char *p = NULL;
				size_t		len = 0;
				text	   *string = NULL;

				if (!isnull)
				{
					string = PG_DETOAST_DATUM(val);
					p = VARDATA(string);
					len = VARSIZE_ANY_EXHDR(string);
				}
				switch (kind)
				{
					case CH_T_FIXEDSTRING:
					case CH_T_STRING:
					case CH_T_ENUM8:
					case CH_T_ENUM16:
					case CH_T_LOWCARDINALITY:
						rc = ch_binary_append_bytes(h, colidx, p, len,
													isnull, errbuf, sizeof(errbuf));
						break;
					default:
						goto type_mismatch;
				}
				break;
			}
		case DATEOID:
			{
				int64_t		seconds;

				if (kind != CH_T_DATE && kind != CH_T_DATE32)
					goto type_mismatch;
				if (!isnull)
				{
					Timestamp	t = date2timestamp_no_overflow(DatumGetDateADT(val));

					seconds = (int64_t) timestamptz_to_time_t(t);
				}
				else
					seconds = 0;
				rc = ch_binary_append_date_seconds(h, colidx, seconds,
												   isnull, errbuf, sizeof(errbuf));
				break;
			}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				if (kind == CH_T_DATETIME)
				{
					int64_t		seconds = isnull
						? 0
						: (int64_t) timestamptz_to_time_t(DatumGetTimestamp(val));

					rc = ch_binary_append_datetime_seconds(h, colidx, seconds,
														   isnull, errbuf, sizeof(errbuf));
				}
				else if (kind == CH_T_DATETIME64)
				{
					int64_t		raw = 0;

					if (!isnull)
					{
						uint32_t	prec = ch_binary_column_datetime64_precision(h, colidx);
						Timestamp	t = DatumGetTimestamp(val);
						double		power = pow(10.0, prec);

						raw = (int64_t) (((1.0 * t) / USECS_PER_SEC
										  + ((POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY))
										 * power);
					}
					rc = ch_binary_append_datetime64_raw(h, colidx, raw,
														 isnull, errbuf, sizeof(errbuf));
				}
				else
					goto type_mismatch;
				break;
			}
		case ANYARRAYOID:
			{
				ch_binary_array_t *arr;
				ch_type_kind item_kind;
				Oid			child_valtype;

				if (kind != CH_T_ARRAY)
					goto type_mismatch;

				arr = (ch_binary_array_t *) DatumGetPointer(val);
				rc = ch_binary_array_begin(h, colidx, arr->len,
										   errbuf, sizeof(errbuf));
				if (rc != 0)
					goto fail;

				/*
				 * While array_begin is active ch_binary_column_kind targets
				 * the inner element kind. For nested arrays the children are
				 * themselves ch_binary_array_t* so recurse with ANYARRAYOID;
				 * at the leaf level use the scalar item_type.
				 */
				item_kind = ch_binary_column_kind(h, colidx);
				child_valtype = (arr->ndim > 1) ? ANYARRAYOID : arr->item_type;
				for (size_t i = 0; i < arr->len; i++)
					append_one(h, 0, item_kind, arr->datums[i], child_valtype, arr->nulls[i]);

				rc = ch_binary_array_end(h, errbuf, sizeof(errbuf));
				break;
			}
		case UUIDOID:
			{
				uint8_t		bytes[16];

				if (kind != CH_T_UUID)
					goto type_mismatch;
				if (!isnull)
					memcpy(bytes, DatumGetUUIDP(val)->data, 16);
				else
					memset(bytes, 0, 16);
				rc = ch_binary_append_uuid(h, colidx, bytes,
										   isnull, errbuf, sizeof(errbuf));
				break;
			}
		case INETOID:
			{
				char	   *s = NULL;

				if (kind != CH_T_IPV4 && kind != CH_T_IPV6)
					goto type_mismatch;
				if (!isnull)
					s = DatumGetCString(DirectFunctionCall1(inet_out, val));
				rc = ch_binary_append_inet(h, colidx, s, isnull, errbuf, sizeof(errbuf));
				if (s)
					pfree(s);
				break;
			}
		default:
			goto type_mismatch;
	}

	if (rc != 0)
		goto fail;
	return;

type_mismatch:
	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("pg_clickhouse: unexpected PG/CH type pair for column %zu", colidx)));
fail:
	raise_errbuf("could not append data to column", errbuf);
}

void
ch_binary_column_append_data(ch_binary_insert_state * state, size_t colidx)
{
	Datum		val = state->values[colidx];
	Oid			valtype = TupleDescAttr(state->outdesc, colidx)->atttypid;
	bool		isnull = state->nulls[colidx];
	ch_type_kind kind = ch_binary_column_kind(state->insert_block, colidx);

	append_one(state->insert_block, colidx, kind, val, valtype, isnull);
}

void
ch_binary_insert_columns(ch_binary_insert_state * state)
{
	char		errbuf[CH_ERR_LEN] = {0};

	if (ch_binary_flush_block(state->insert_block, errbuf, sizeof(errbuf)) != 0)
		raise_errbuf("could not insert columns", errbuf);
}

void
ch_binary_insert_state_free(void *c)
{
	ch_binary_insert_state *state = (ch_binary_insert_state *) c;
	char		errbuf[CH_ERR_LEN] = {0};

	if (state->insert_block == NULL)
		return;

	ch_binary_end_insert(state->insert_block, errbuf, sizeof(errbuf));
	state->insert_block = NULL;
	if (errbuf[0] != '\0')
		raise_errbuf("could not finish INSERT", errbuf);
}
