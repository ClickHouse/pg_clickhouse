/*
 * encode.c
 *
 * PG-side INSERT path. Reads PG Datums and dispatches into the typed
 * ch_binary_append_* shims exposed by insert.c.
 */

#include "postgres.h"

#include <string.h>
#include <sys/socket.h>			/* AF_INET, expanded by PG inet macros */

#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "nodes/pg_list.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/inet.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/timestamp.h"
#include "utils/uuid.h"

#include "binary_internal.h"

/* CH Date epoch is unix; offset to PG epoch (2000-01-01) */
#define CH_TO_PG_DATE_OFFSET (POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE)

/*
 * Map a CH column kind to the PG type our import path uses. Used when
 * constructing the TupleDesc for INSERT...VALUES.
 */
static Oid
ch_kind_to_pg_oid_for_insert(const chc_type * type, const char *colname)
{
	switch (chc_type_kind(type))
	{
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
			{
				/*
				 * postgres uses one array type per element type regardless of
				 * nesting, so unwrap nested Array layers before looking up.
				 */
				const		chc_type *leaf = type;
				Oid			item_type;
				Oid			array_type;

				while (chc_type_kind(leaf) == CHC_ARRAY)
					leaf = chc_type_child(leaf, 0);
				item_type = ch_kind_to_pg_oid_for_insert(leaf, colname);
				array_type = get_array_type(item_type);

				if (array_type == InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							 errmsg("pg_clickhouse: could not find array type for column \"%s\"",
									colname ? colname : "?")));
				return array_type;
			}
		case CHC_TUPLE:
			return RECORDOID;
		case CHC_NULLABLE:
		case CHC_LOW_CARDINALITY:
			return ch_kind_to_pg_oid_for_insert(chc_type_child(type, 0), colname);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: unsupported column type \"%s\" for \"%s\"",
							chc_type_name(type, NULL),
							colname ? colname : "?")));
	}
	return InvalidOid;
}

void
ch_binary_prepare_insert(void *conn, const ch_query * query,
						 ch_binary_insert_state * state)
{
	ch_binary_column_info *cols = NULL;
	size_t		n = 0;
	ch_binary_insert_handle *h;

	h = ch_binary_begin_insert((ch_binary_connection_t *) conn, query, &cols, &n);

	state->len = n;
	state->insert_block = h;

	if (n == 0)
		return;

	state->outdesc = CreateTemplateTupleDesc(n);
	for (size_t i = 0; i < n; i++)
	{
		Oid			pg_type = ch_kind_to_pg_oid_for_insert(cols[i].type, cols[i].name);

		/*
		 * CHC_JSON defaults to JSONBOID; honor foreign-table's JSONOID
		 * declaration when present so outdesc matches the source slot and no
		 * json↔jsonb conversion is needed.
		 */
		if (pg_type == JSONBOID && query->tupdesc && query->attr_nums)
		{
			AttrNumber	attnum = list_nth_int((List *) query->attr_nums, i);

			if (attnum >= 1 && attnum <= query->tupdesc->natts &&
				TupleDescAttr(query->tupdesc, attnum - 1)->atttypid == JSONOID)
				pg_type = JSONOID;
		}

		TupleDescInitEntry(state->outdesc, (AttrNumber) (i + 1),
						   cols[i].name ? cols[i].name : "",
						   pg_type, -1, 0);
	}
}

/*
 * Append a single value (already extracted from a Datum + isnull) into the
 * current column, dispatching on (PG Oid, CH kind). ereports on mismatch.
 */
static void
append_one(ch_binary_insert_handle * h, size_t colidx,
		   chc_kind kind, Datum val, Oid valtype, bool isnull)
{
	switch (valtype)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
			{
				int64_t		v = 0;

				/* Support mixing integer types. */
				if (!(kind == CHC_BOOL
					  || (kind >= CHC_INT8 && kind <= CHC_INT64)
					  || (kind >= CHC_UINT8 && kind <= CHC_UINT64)))
					goto type_mismatch;
				if (!isnull)
				{
					if (valtype == INT2OID)
						v = (int64_t) DatumGetInt16(val);
					else if (valtype == INT4OID)
						v = (int64_t) DatumGetInt32(val);
					else
						v = DatumGetInt64(val);
				}
				ch_binary_append_int(h, colidx, v, isnull);
				return;
			}
		case BOOLOID:
			if (kind != CHC_BOOL && kind != CHC_UINT8)
				goto type_mismatch;
			ch_binary_append_bool(h, colidx, (bool) val, isnull);
			return;
		case FLOAT4OID:
			if (kind != CHC_FLOAT32)
				goto type_mismatch;
			ch_binary_append_float(h, colidx, DatumGetFloat4(val), isnull);
			return;
		case FLOAT8OID:
			if (kind != CHC_FLOAT64)
				goto type_mismatch;
			ch_binary_append_double(h, colidx, DatumGetFloat8(val), isnull);
			return;
		case NUMERICOID:
			{
				char	   *s = NULL;

				if (kind != CHC_DECIMAL32 && kind != CHC_DECIMAL64
					&& kind != CHC_DECIMAL128 && kind != CHC_DECIMAL256)
					goto type_mismatch;
				if (!isnull)
					s = DatumGetCString(DirectFunctionCall1(numeric_out, val));
				ch_binary_append_decimal(h, colidx, s, isnull);
				if (s)
					pfree(s);
				return;
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
					case CHC_FIXED_STRING:
					case CHC_STRING:
					case CHC_ENUM8:
					case CHC_ENUM16:
						ch_binary_append_bytes(h, colidx, p, len, isnull);
						return;
					default:
						goto type_mismatch;
				}
			}
		case DATEOID:
			{
				int64_t		seconds = 0;

				if (kind != CHC_DATE && kind != CHC_DATE32)
					goto type_mismatch;
				if (!isnull)
					seconds = ((int64_t) DatumGetDateADT(val)
							   + CH_TO_PG_DATE_OFFSET) * SECS_PER_DAY;
				ch_binary_append_date_seconds(h, colidx, seconds, isnull);
				return;
			}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				switch (kind)
				{
					case CHC_DATETIME:
						{
							int64_t		seconds = isnull
								? 0
								: (int64_t) timestamptz_to_time_t(DatumGetTimestamp(val));

							ch_binary_append_datetime_seconds(h, colidx, seconds, isnull);
						} break;
					case CHC_DATETIME64:
						{
							int64_t		raw = 0;

							if (!isnull)
							{
								uint32_t	prec = ch_binary_column_datetime64_precision(h, colidx);
								Timestamp	t = DatumGetTimestamp(val);
								int64		power = pow10i[prec];
								int64		secs = t / USECS_PER_SEC;
								int64		us_rem = t % USECS_PER_SEC;

								/*
								 * floor-divide; C trunc-to-zero leaves
								 * negative remainder
								 */
								if (us_rem < 0)
								{
									secs -= 1;
									us_rem += USECS_PER_SEC;
								}
								secs += CH_TO_PG_DATE_OFFSET * SECS_PER_DAY;
								raw = secs * power + us_rem * power / USECS_PER_SEC;
							}
							ch_binary_append_datetime64_raw(h, colidx, raw, isnull);
						} break;
					default:
						goto type_mismatch;
				}
				return;
			}
		case ANYARRAYOID:
			{
				ch_binary_array_t *arr;
				chc_kind	item_kind;
				Oid			child_valtype;

				if (kind != CHC_ARRAY)
					goto type_mismatch;

				arr = (ch_binary_array_t *) DatumGetPointer(val);
				ch_binary_array_begin(h, colidx);

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

				ch_binary_array_end(h);
				return;
			}
		case UUIDOID:
			{
				uint8_t		bytes[16];

				if (kind != CHC_UUID)
					goto type_mismatch;
				if (!isnull)
					memcpy(bytes, DatumGetUUIDP(val)->data, 16);
				else
					memset(bytes, 0, 16);
				ch_binary_append_uuid(h, colidx, bytes, isnull);
				return;
			}
		case INETOID:
			{
				const		uint8_t *addr = NULL;
				size_t		addrlen = 0;

				if (kind != CHC_IPV4 && kind != CHC_IPV6)
					goto type_mismatch;
				if (!isnull)
				{
					inet	   *ipa = DatumGetInetPP(val);
					int			fam = ip_family(ipa);
					int			expected = (kind == CHC_IPV4) ? PGSQL_AF_INET : PGSQL_AF_INET6;

					if (fam != expected)
						ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("pg_clickhouse: inet family mismatch for column %zu",
										colidx)));
					addr = ip_addr(ipa);
					addrlen = ip_addrsize(ipa);
				}
				else
					addrlen = (kind == CHC_IPV4) ? 4 : 16;
				ch_binary_append_inet(h, colidx, addr, addrlen, isnull);
				return;
			}
		case JSONOID:
		case JSONBOID:
			{
				char	   *s = NULL;
				size_t		len = 0;

				if (kind != CHC_JSON && kind != CHC_OBJECT)
					goto type_mismatch;
				if (!isnull)
				{
					s = DatumGetCString(DirectFunctionCall1(valtype == JSONBOID
															? jsonb_out
															: json_out,
															val));
					len = strlen(s);
				}
				ch_binary_append_bytes(h, colidx, s, len, isnull);
				if (s)
					pfree(s);
				return;
			}
		default:
			goto type_mismatch;
	}

type_mismatch:
	ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("pg_clickhouse: unexpected PG/CH type pair for column %zu", colidx)));
}

void
ch_binary_column_append_data(ch_binary_insert_state * state, size_t colidx)
{
	Datum		val = state->values[colidx];
	Oid			valtype = TupleDescAttr(state->outdesc, colidx)->atttypid;
	bool		isnull = state->nulls[colidx];
	chc_kind	kind = ch_binary_column_kind(state->insert_block, colidx);

	append_one(state->insert_block, colidx, kind, val, valtype, isnull);
}

void
ch_binary_insert_columns(ch_binary_insert_state * state)
{
	ch_binary_flush_block(state->insert_block);
}

void
ch_binary_insert_state_free(void *c)
{
	ch_binary_insert_state *state = (ch_binary_insert_state *) c;

	if (state->insert_block == NULL)
		return;

	/*
	 * Reset-callback context: cannot ereport. Finalize runs from the FDW
	 * happy path before we get here; if it did not (mid-query abort), the
	 * release call flags the connection broken so the next query rebuilds.
	 */
	ch_binary_release_insert(state->insert_block);
	state->insert_block = NULL;
}
