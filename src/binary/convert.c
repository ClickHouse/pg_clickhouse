/* Conversion functions for the binary driver. */

#include "postgres.h"

#include "funcapi.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_type.h"
#include "utils/typcache.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/arrayaccess.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "executor/tuptable.h"

#include "fdw.h"
#include "binary_internal.h"
#include <stdint.h>

typedef struct ch_convert_state ch_convert_state;
typedef struct ch_convert_output_state ch_convert_output_state;
typedef Datum(*convert_func) (ch_convert_state *, Datum);
typedef Datum(*out_convert_func) (ch_convert_output_state *, Datum);

typedef struct ch_convert_state
{
	Oid			intype;
	Oid			outtype;
	convert_func func;

	/* record */
	TupleConversionMap *tupmap;
	CustomObjectDef *cdef;
	TupleDesc	indesc;			/* for RECORD */
	TupleDesc	outdesc;		/* for RECORD */
	ch_convert_state **conversion_states;

	/* array */
	int16		typlen;
	bool		typbyval;
	char		typalign;

	/* text */
	int32		typmod;
	Oid			typinput;
	Oid			typioparam;

	/* generic */
	CoercionPathType ctype;
	Oid			castfunc;
}			ch_convert_state;

typedef struct ch_convert_output_state
{
	Oid			intype;
	Oid			outtype;
	AttrNumber	attnum;
	out_convert_func func;

	/* array */
	Oid			innertype;		/* if intype is array */
	int16		typlen;
	bool		typbyval;
	char		typalign;

	/* generic */
	CoercionPathType ctype;
	Oid			castfunc;
}			ch_convert_output_state;

static Datum
convert_record(ch_convert_state * state, Datum val)
{
	HeapTuple	temptup;
	HeapTuple	htup;
	ch_binary_tuple_t *slot = (ch_binary_tuple_t *) DatumGetPointer(val);

	for (size_t i = 0; i < slot->len; i++)
	{
		ch_convert_state *s = state->conversion_states[i];

		if (s)
			slot->datums[i] = s->func(s, slot->datums[i]);
	}

	htup = heap_form_tuple(state->indesc, slot->datums, slot->nulls);
	if (!state->outdesc)
	{
		val = heap_copy_tuple_as_datum(htup, state->indesc);

		if (state->cdef && state->cdef->rowfunc != InvalidOid)
		{
			/* there is converter from row to outtype */
			val = OidFunctionCall1(state->cdef->rowfunc, val);
		}
		else if (state->outtype == TEXTOID)
		{
			/* a lot of allocations, not so efficient */
			val = CStringGetTextDatum(DatumGetCString(
													  OidFunctionCall1(F_RECORD_OUT, val)));
		}
	}
	else
	{
		if (state->tupmap)
			temptup = execute_attr_map_tuple(htup, state->tupmap);
		else
			temptup = htup;

		val = heap_copy_tuple_as_datum(temptup, state->outdesc);
	}

	return val;
}

inline static Datum
convert_generic(ch_convert_state * state, Datum val)
{
	if (state->ctype == COERCION_PATH_FUNC)
	{
		Assert(state->castfunc != InvalidOid);
		val = OidFunctionCall1(state->castfunc, val);
	}

	return val;
}

inline static Datum
convert_out_generic(ch_convert_output_state * state, Datum val)
{
	if (state->ctype == COERCION_PATH_FUNC)
	{
		Assert(state->castfunc != InvalidOid);
		val = OidFunctionCall1(state->castfunc, val);
	}

	return val;
}

/*
 * Walk nested ch_binary_array_t into a flat datum buffer, verifying each
 * level matches the dims taken from the first child. Returns false if the
 * shape is jagged so the caller can fall back to a slower path.
 */
static bool
flatten_nested_array(ch_binary_array_t * slot, int *dims, int level,
					 Datum * values, bool *nulls, size_t * idx)
{
	if ((int) slot->len != dims[level])
		return false;

	if (slot->ndim == 1)
	{
		for (size_t i = 0; i < slot->len; i++)
		{
			values[*idx] = slot->datums[i];
			nulls[*idx] = slot->nulls[i];
			(*idx)++;
		}
	}
	else
	{
		for (size_t i = 0; i < slot->len; i++)
		{
			ch_binary_array_t *child = (ch_binary_array_t *) DatumGetPointer(slot->datums[i]);

			if (!flatten_nested_array(child, dims, level + 1, values, nulls, idx))
				return false;
		}
	}
	return true;
}

/*
 * Emit a nested ch_binary_array_t as a postgres array text literal, quoting
 * each leaf and escaping `\` and `"`. Used as the jagged fallback so binary
 * surfaces the same array_in malformed-literal error as the http path.
 */
static void
emit_nested_array_text(ch_binary_array_t * slot, FmgrInfo * outfn, StringInfo buf)
{
	appendStringInfoChar(buf, '{');
	for (size_t i = 0; i < slot->len; i++)
	{
		if (i > 0)
			appendStringInfoChar(buf, ',');

		if (slot->ndim > 1)
		{
			ch_binary_array_t *child = (ch_binary_array_t *) DatumGetPointer(slot->datums[i]);

			emit_nested_array_text(child, outfn, buf);
		}
		else if (slot->nulls[i])
			appendStringInfoString(buf, "NULL");
		else
		{
			char	   *s = OutputFunctionCall(outfn, slot->datums[i]);

			appendStringInfoChar(buf, '"');
			for (char *p = s; *p; p++)
			{
				if (*p == '"' || *p == '\\')
					appendStringInfoChar(buf, '\\');
				appendStringInfoChar(buf, *p);
			}
			appendStringInfoChar(buf, '"');
			pfree(s);
		}
	}
	appendStringInfoChar(buf, '}');
}

static Datum
convert_array(ch_convert_state * state, Datum val)
{
	ch_binary_array_t *slot = (ch_binary_array_t *) DatumGetPointer(val);

	if (slot->len == 0)
		val = PointerGetDatum(construct_empty_array(slot->item_type));
	else if (slot->ndim <= 1)
	{
		void	   *arrout = construct_array(slot->datums, slot->len, slot->item_type,
											 state->typlen, state->typbyval, state->typalign);

		val = PointerGetDatum(arrout);
	}
	else
	{
		int			dims[MAXDIM] = {};
		int			lbs[MAXDIM] = {};
		size_t		total = 1;
		size_t		idx = 0;
		Datum	   *flat;
		bool	   *flatnulls;
		ch_binary_array_t *probe = slot;

		if (slot->ndim > MAXDIM)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("pg_clickhouse: nested array depth %d exceeds maximum %d",
							slot->ndim, MAXDIM)));

		for (int d = 0; d < slot->ndim; d++)
		{
			dims[d] = (int) probe->len;
			lbs[d] = 1;
			total *= probe->len;
			if (probe->ndim > 1 && probe->len > 0)
				probe = (ch_binary_array_t *) DatumGetPointer(probe->datums[0]);
		}

		if (total == 0)
			val = PointerGetDatum(construct_empty_array(slot->item_type));
		else
		{
			flat = palloc(sizeof(Datum) * total);
			flatnulls = palloc0(sizeof(bool) * total);

			if (flatten_nested_array(slot, dims, 0, flat, flatnulls, &idx))
				val = PointerGetDatum(construct_md_array(flat, flatnulls, slot->ndim,
														 dims, lbs, slot->item_type,
														 state->typlen, state->typbyval,
														 state->typalign));
			else
			{
				/*
				 * Jagged shape: format as text and route through array_in so
				 * binary surfaces the same malformed-literal error as http.
				 */
				StringInfoData buf;
				FmgrInfo	outfn;
				Oid			out_func;
				Oid			in_func;
				Oid			ioparam;
				bool		varlena;

				pfree(flat);
				pfree(flatnulls);

				getTypeOutputInfo(slot->item_type, &out_func, &varlena);
				fmgr_info(out_func, &outfn);

				initStringInfo(&buf);
				emit_nested_array_text(slot, &outfn, &buf);

				getTypeInputInfo(state->intype, &in_func, &ioparam);
				val = OidInputFunctionCall(in_func, buf.data, ioparam, -1);

				pfree(buf.data);
			}
		}
	}

	return convert_generic(state, val);
}

static Datum
convert_remote_text(ch_convert_state * state, Datum val)
{
	return OidInputFunctionCall(state->typinput, TextDatumGetCString(val),
								state->typioparam, state->typmod);
}

/*
 * We imply that corresponding type for UInt8 (bool in ClickHouse) is
 * SMALLINT and this function covers this case
 */
static Datum
convert_bool(ch_convert_state * state, Datum val)
{
	int16		dat = DatumGetInt16(val);

	return BoolGetDatum(dat);
}

Datum
ch_binary_convert_datum(void *state, Datum val)
{
	return state ? ((ch_convert_state *) state)->func(state, val) : val;
}

/* input */

/*
 * Convert val from intype to outtype. No conversion for binary-compatible
 * types or when intype and outtype are the same. All others converted via the
 * appropriate CAST function.
*/
void	   *
ch_binary_init_convert_state(Datum val, Oid intype, Oid outtype)
{
	/* make_datum() copies all bytes, no cast needed. */
	if (intype == TEXTOID && outtype == BYTEAOID)
		return NULL;

	ch_convert_state *state = palloc0(sizeof(ch_convert_state));

	state->intype = intype;
	state->outtype = outtype;
	state->cdef = chfdw_check_for_custom_type(outtype);
	state->typmod = -1;
	state->ctype = COERCION_PATH_NONE;

	if (intype == ANYARRAYOID)
	{
		ch_binary_array_t *slot = (ch_binary_array_t *) DatumGetPointer(val);

		get_typlenbyvalalign(slot->item_type, &state->typlen, &state->typbyval,
							 &state->typalign);

		/* restore intype */
		state->intype = slot->array_type;
		intype = slot->array_type;
		state->func = convert_array;
	}

	if (intype == RECORDOID)
	{
		ch_binary_tuple_t *slot = (ch_binary_tuple_t *) DatumGetPointer(val);

		state->func = convert_record;

#if PG_VERSION_NUM < 120000
		state->indesc = CreateTemplateTupleDesc(slot->len, false);
#else
		state->indesc = CreateTemplateTupleDesc(slot->len);
#endif
		state->conversion_states = palloc(sizeof(void *) * slot->len);

		for (size_t i = 0; i < slot->len; ++i)
		{
			Oid			item_type = slot->types[i];

			if (slot->types[i] == ANYARRAYOID)
			{
				ch_binary_array_t *arr = (ch_binary_array_t *) DatumGetPointer(slot->datums[i]);

				item_type = arr->array_type;
			}
			state->conversion_states[i] = ch_binary_init_convert_state(slot->datums[i],
																	   slot->types[i], item_type);

			TupleDescInitEntry(state->indesc, (AttrNumber) i + 1, "",
							   item_type, -1, 0);
		}

		state->indesc = BlessTupleDesc(state->indesc);

		if (!(state->cdef || outtype == RECORDOID || outtype == TEXTOID))
		{
			TypeCacheEntry *typentry;
			TupleDesc	tupdesc;

			typentry = lookup_type_cache(outtype,
										 TYPECACHE_TUPDESC |
										 TYPECACHE_DOMAIN_BASE_INFO);

			if (typentry->typtype == TYPTYPE_DOMAIN)
				tupdesc = lookup_rowtype_tupdesc_noerror(typentry->domainBaseType,
														 typentry->domainBaseTypmod,
														 false);
			else
			{
				if (typentry->tupDesc == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_WRONG_OBJECT_TYPE),
							 errmsg("pg_clickhouse: cannot return %s as %s",
									slot->ch_type_name ? slot->ch_type_name : "?",
									format_type_be(outtype))));

				tupdesc = typentry->tupDesc;
				PinTupleDesc(tupdesc);
			}
			state->outdesc = CreateTupleDescCopy(tupdesc);
			state->tupmap = convert_tuples_by_position(state->indesc, state->outdesc,
													   "pg_clickhouse: could not map tuple to returned type");
			ReleaseTupleDesc(tupdesc);
		}
	}
	else if (intype != outtype)
	{
		if (!state->func)
			state->func = convert_generic;

		if (intype == TEXTOID)
		{
			Type		baseType;
			Oid			baseTypeId;
			Form_pg_type typform;

			baseTypeId = getBaseTypeAndTypmod(outtype, &state->typmod);
			if (baseTypeId != INTERVALOID)
				state->typmod = -1;

			baseType = typeidType(baseTypeId);
			typform = (Form_pg_type) GETSTRUCT(baseType);
			state->typinput = typform->typinput;
			state->typioparam = getTypeIOParam(baseType);
			state->func = convert_remote_text;
			ReleaseSysCache(baseType);
		}
		else if (outtype == BOOLOID && intype == INT2OID)
		{
			state->func = convert_bool;
		}
		else
		{
			/* try to convert */
			state->ctype = find_coercion_pathway(outtype, intype,
												 COERCION_EXPLICIT,
												 &state->castfunc);
			switch (state->ctype)
			{
				case COERCION_PATH_FUNC:
					break;
				case COERCION_PATH_RELABELTYPE:

					/*
					 * if the conversion func was not previously set, then no
					 * conversion needed
					 */
					if (state->func == NULL)
						goto no_conversion;

					/* all good */
					break;
				default:
					ereport(ERROR,
							(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							 errmsg("pg_clickhouse: could not cast value from %s to %s",
									format_type_be(intype), format_type_be(outtype))));
			}
		}
	}
	else if (!state->func)
	{
no_conversion:
		/* no conversion needed */
		pfree(state);
		state = NULL;
	}

	return state;
}

void
ch_binary_free_convert_state(void *s)
{
	ch_convert_state *state = s;

	pfree(state);
}

/* output */

static void
init_output_convert_state(ch_convert_output_state * state)
{
	if (state->outtype == state->intype)
		return;

	/* column_append() copies all bytes, no cast needed. */
	if (state->intype == BYTEAOID && state->outtype == TEXTOID)
		return;

	state->func = convert_out_generic;

	state->ctype = find_coercion_pathway(state->outtype, state->intype,
										 COERCION_EXPLICIT, &state->castfunc);

	switch (state->ctype)
	{
		case COERCION_PATH_FUNC:
			break;
		case COERCION_PATH_RELABELTYPE:
			state->func = NULL;
			return;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("pg_clickhouse: could not find a casting path from %s to %s",
							format_type_be(state->intype), format_type_be(state->outtype))));
	}
}

void	   *
ch_binary_make_tuple_map(TupleDesc indesc, TupleDesc outdesc, Oid relid)
{
	ch_convert_output_state *states;
	int			n;
	int			i;

	n = outdesc->natts;
	states = (ch_convert_output_state *) palloc0(n * sizeof(ch_convert_output_state));

	for (i = 0; i < n; i++)
	{
		ch_convert_output_state *curstate = &states[i];

		Form_pg_attribute attout = TupleDescAttr(outdesc, i);
		char	   *outattname;
		int			j;

		outattname = NameStr(attout->attname);
		curstate->outtype = attout->atttypid;

		if (NameStr(TupleDescAttr(indesc, 0)->attname)[0] == '\0')
		{
			Form_pg_attribute attin = TupleDescAttr(indesc, i);

			curstate->intype = attin->atttypid;
			init_output_convert_state(curstate);
			curstate->attnum = (AttrNumber) (i + 1);
		}
		else
		{
			for (j = 0; j < indesc->natts; j++)
			{
				Form_pg_attribute attin = TupleDescAttr(indesc, j);
				char	   *inattname;
				CustomColumnInfo *cinfo;

				if (attin->attisdropped)
					continue;

				/* Honor column_name FDW option, falls through to attname */
				cinfo = OidIsValid(relid)
					? chfdw_get_custom_column_info(relid, j + 1) : NULL;
				inattname = (cinfo && cinfo->colname[0])
					? cinfo->colname : NameStr(attin->attname);

				curstate->intype = attin->atttypid;

				if (strcmp(outattname, inattname) == 0)
				{
					init_output_convert_state(curstate);
					curstate->attnum = (AttrNumber) (j + 1);
					break;
				}
			}
		}

		curstate->innertype = get_element_type(curstate->outtype);
		if (curstate->innertype != InvalidOid)
		{
			curstate->outtype = ANYARRAYOID;
			get_typlenbyvalalign(curstate->innertype, &curstate->typlen,
								 &curstate->typbyval, &curstate->typalign);
		}


		if (curstate->attnum == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg_internal("pg_clickhouse: could not create conversion map"),
					 errdetail("Attribute \"%s\" of type %s does not exist in type %s.",
							   outattname,
							   format_type_be(indesc->tdtypeid),
							   format_type_be(outdesc->tdtypeid))));
	}

	return states;
}

/*
 * Chunk a flat postgres array (already extracted into `flat`/`flatnulls` in
 * row-major order) into the nested ch_binary_array_t tree the binary engine
 * expects for Array(Array(...)) columns. Each interior node carries
 * ndim>1 with datums[i] = PointerGetDatum(child); leaves carry ndim==1 with
 * scalar datums copied from the flat buffer.
 */
static ch_binary_array_t *
build_nested_binary_array(int level, int ndim, int *dims, Oid item_type,
						  Datum * flat, bool *flatnulls, size_t * idx)
{
	ch_binary_array_t *arr = palloc(sizeof(ch_binary_array_t));

	arr->len = dims[level];
	arr->ndim = ndim - level;
	arr->item_type = item_type;
	arr->array_type = InvalidOid;
	arr->datums = palloc(sizeof(Datum) * arr->len);
	arr->nulls = palloc0(sizeof(bool) * arr->len);

	if (level + 1 == ndim)
	{
		for (size_t i = 0; i < arr->len; i++)
		{
			arr->datums[i] = flat[*idx];
			arr->nulls[i] = flatnulls[*idx];
			(*idx)++;
		}
	}
	else
	{
		for (size_t i = 0; i < arr->len; i++)
		{
			ch_binary_array_t *child = build_nested_binary_array(level + 1, ndim, dims,
																 item_type, flat,
																 flatnulls, idx);

			arr->datums[i] = PointerGetDatum(child);
		}
	}
	return arr;
}

/*
 * For each output value, convert it, if necessary, from the Postgres Datum
 * type defined for the foreign table to a Datum that the binary INSERT
 * path in encode.c knows how to convert to a ClickHouse type. No conversion
 * for binary-compatible types; other types require a CAST.
 * ch_binary_make_tuple_map() makes this determination for each type, stored
 * in insert_state->conversion_states)
 */
void
ch_binary_do_output_conversion(ch_binary_insert_state * insert_state,
							   TupleTableSlot * slot)
{
	Datum	   *out_values = insert_state->values;
	bool	   *out_nulls = insert_state->nulls;

	for (size_t i = 0; i < insert_state->outdesc->natts; i++)
	{
		ch_convert_output_state *cstate = &((ch_convert_output_state *) insert_state->conversion_states)[i];
		AttrNumber	attnum = cstate->attnum;

		out_values[i] = slot_getattr(slot, attnum, &out_nulls[i]);
		if (!out_nulls[i])
		{
			if (cstate->func)
				out_values[i] = cstate->func(cstate, out_values[i]);
			else if (cstate->outtype == ANYARRAYOID)
			{
				AnyArrayType *v = DatumGetAnyArrayP(out_values[i]);
				ch_binary_array_t *arr;
				array_iter	iter;
				int			ndim = AARR_NDIM(v);
				int		   *dims = AARR_DIMS(v);
				size_t		total = ArrayGetNItems(ndim, dims);

				if (ndim > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("pg_clickhouse: inserted array depth %d exceeds maximum %d",
									ndim, MAXDIM)));

				if (ndim <= 1)
				{
					arr = palloc(sizeof(ch_binary_array_t));
					arr->len = total;
					arr->ndim = 1;
					arr->item_type = cstate->innertype;
					arr->array_type = InvalidOid;
					arr->datums = total ? palloc(sizeof(Datum) * total) : NULL;
					arr->nulls = total ? palloc(sizeof(bool) * total) : NULL;

					array_iter_setup(&iter, v);
					for (size_t j = 0; j < total; j++)
					{
						arr->datums[j] = array_iter_next(&iter, &arr->nulls[j], j,
														 cstate->typlen, cstate->typbyval, cstate->typalign);
					}
				}
				else
				{
					Datum	   *flat = palloc(sizeof(Datum) * total);
					bool	   *flatnulls = palloc0(sizeof(bool) * total);
					size_t		idx = 0;

					array_iter_setup(&iter, v);
					for (size_t j = 0; j < total; j++)
					{
						flat[j] = array_iter_next(&iter, &flatnulls[j], j,
												  cstate->typlen, cstate->typbyval, cstate->typalign);
					}

					arr = build_nested_binary_array(0, ndim, dims, cstate->innertype,
													flat, flatnulls, &idx);

					pfree(flat);
					pfree(flatnulls);
				}
				out_values[i] = PointerGetDatum(arr);

				/* hack: mark as unified array */
				TupleDescAttr(insert_state->outdesc, i)->atttypid = ANYARRAYOID;
			}
		}
	}
}
