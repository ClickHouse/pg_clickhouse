#include "postgres.h"
#include "strings.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/extension.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "fdw.h"

// Prior to PostgreSQL 14 these variables had other names or were undefined.
// See postgres/include/server/utils/fmgroids.h
#if PG_VERSION_NUM < 140000
#define F_DATE_TRUNC_TEXT_TIMESTAMP F_TIMESTAMP_TRUNC
#define F_DATE_PART_TEXT_TIMESTAMP F_TIMESTAMP_PART
#define F_DATE_TRUNC_TEXT_TIMESTAMPTZ F_TIMESTAMPTZ_TRUNC
#define F_TIMEZONE_TEXT_TIMESTAMP F_TIMESTAMP_ZONE
#define F_TIMEZONE_TEXT_TIMESTAMPTZ F_TIMESTAMPTZ_ZONE
#define F_DATE_PART_TEXT_TIMESTAMPTZ F_TIMESTAMPTZ_PART
#define F_ARRAY_POSITION_ANYCOMPATIBLEARRAY_ANYCOMPATIBLE F_ARRAY_POSITION
#define F_BTRIM_TEXT_TEXT F_BTRIM
#define F_BTRIM_TEXT F_BTRIM1
#define F_STRPOS 868
#define F_DATE_PART_TEXT_DATE 1384
#define F_PERCENTILE_CONT_FLOAT8_FLOAT8 3974
#define F_PERCENTILE_CONT_FLOAT8_INTERVAL 3976

// Prior to Postgres 14 EXTRACT mapped directly to DATE_PART.
// https://github.com/postgres/postgres/commit/a2da77cdb466
#define F_EXTRACT_TEXT_TIMESTAMP 6202
#define F_EXTRACT_TEXT_TIMESTAMPTZ 6203
#define F_EXTRACT_TEXT_DATE 6199
#endif
// regexp_like was added in Postgres 15; Mock it for earlier versions.
#if PG_VERSION_NUM < 150000
#define F_REGEXP_LIKE_TEXT_TEXT 6263
#endif


static HTAB *custom_objects_cache = NULL;
static HTAB *custom_columns_cache = NULL;

static HTAB *
create_custom_objects_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(CustomObjectDef);

	return hash_create("pg_clickhouse custom functions", 20, &ctl, HASH_ELEM | HASH_BLOBS);
}

static void
invalidate_custom_columns_cache(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	CustomColumnInfo *entry;

	hash_seq_init(&status, custom_columns_cache);
	while ((entry = (CustomColumnInfo *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(custom_columns_cache,
						(void *) &entry->relid,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

static HTAB *
create_custom_columns_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid) + sizeof(int);
	ctl.entrysize = sizeof(CustomColumnInfo);

	CacheRegisterSyscacheCallback(ATTNUM,
								  invalidate_custom_columns_cache,
								  (Datum) 0);

	return hash_create("pg_clickhouse custom functions", 20, &ctl, HASH_ELEM | HASH_BLOBS);
}

inline static void
init_custom_entry(CustomObjectDef *entry)
{
	entry->cf_type = CF_USUAL;
	entry->custom_name[0] = '\0';
	entry->cf_context = NULL;
	entry->rowfunc = InvalidOid;
}

/*
 * Return true if ordered aggregate funcid maps to a parameterized ClickHouse
 * aggregate function.
 */
inline bool chfdw_check_for_builtin_ordered_aggregate(Oid funcid)
{
	switch (funcid) {
		// Ordered aggregates that map to ClickHouse functions.
		case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
		case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
			return true;
	}
	return false;
}

CustomObjectDef *chfdw_check_for_custom_function(Oid funcid)
{
	bool special_builtin = false;
	CustomObjectDef	*entry;

	if (chfdw_is_builtin(funcid))
	{
		switch (funcid)
		{
			case F_DATE_TRUNC_TEXT_TIMESTAMP:
			case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			case F_TIMEZONE_TEXT_TIMESTAMP:
			case F_TIMEZONE_TEXT_TIMESTAMPTZ:
			case F_DATE_PART_TEXT_TIMESTAMP:
			case F_DATE_PART_TEXT_TIMESTAMPTZ:
			case F_DATE_PART_TEXT_DATE:
			case F_EXTRACT_TEXT_TIMESTAMP:
			case F_EXTRACT_TEXT_TIMESTAMPTZ:
			case F_EXTRACT_TEXT_DATE:
			case F_ARRAY_POSITION_ANYCOMPATIBLEARRAY_ANYCOMPATIBLE:
			case F_STRPOS:
			case F_BTRIM_TEXT_TEXT:
			case F_BTRIM_TEXT:
			case F_REGEXP_LIKE_TEXT_TEXT:
			case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
			case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
				special_builtin = true;
				break;
			default:
				return NULL;
		}
	}

	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	entry = hash_search(custom_objects_cache, (void *) &funcid, HASH_FIND, NULL);
	if (!entry)
	{
		Oid			extoid;
		char	   *extname;

		entry = hash_search(custom_objects_cache, (void *) &funcid, HASH_ENTER, NULL);
		entry->cf_oid = funcid;
		init_custom_entry(entry);
		switch (funcid)
		{
			case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
			case F_DATE_TRUNC_TEXT_TIMESTAMP:
			{
				entry->cf_type = CF_DATE_TRUNC;
				entry->custom_name[0] = '\1';
				break;
			}
			case F_DATE_PART_TEXT_TIMESTAMPTZ:
			case F_DATE_PART_TEXT_TIMESTAMP:
			case F_DATE_PART_TEXT_DATE:
			case F_EXTRACT_TEXT_TIMESTAMP:
			case F_EXTRACT_TEXT_TIMESTAMPTZ:
			case F_EXTRACT_TEXT_DATE:
			{
				entry->cf_type = CF_DATE_PART;
				entry->custom_name[0] = '\1';
				break;
			}
			case F_TIMEZONE_TEXT_TIMESTAMP:
			case F_TIMEZONE_TEXT_TIMESTAMPTZ:
			{
				entry->cf_type = CF_TIMEZONE;
				strcpy(entry->custom_name, "toTimeZone");
				break;
			}
			case F_ARRAY_POSITION_ANYCOMPATIBLEARRAY_ANYCOMPATIBLE:
			{
				strcpy(entry->custom_name, "indexOf");
				break;
			}
			case F_BTRIM_TEXT_TEXT:
			case F_BTRIM_TEXT:
			{
				strcpy(entry->custom_name, "trimBoth");
				break;
			}
			case F_STRPOS:
			{
				strcpy(entry->custom_name, "position");
				break;
			}
			case F_REGEXP_LIKE_TEXT_TEXT:
			{
				entry->cf_type = CF_MATCH;
				strcpy(entry->custom_name, "match");
				break;
			}
			case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
			case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
			{
				strcpy(entry->custom_name, "quantile");
				break;
			}
		}

		if (special_builtin)
			return entry;

		extoid = getExtensionOfObject(ProcedureRelationId, funcid);
		extname = get_extension_name(extoid);
		if (extname)
		{
			HeapTuple	proctup;
			Form_pg_proc procform;
			char		*proname;

			proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(proctup))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			procform = (Form_pg_proc) GETSTRUCT(proctup);
			proname = NameStr(procform->proname);

			if (strcmp(extname, "istore") == 0)
			{
				if (strcmp(NameStr(procform->proname), "sum") == 0)
				{
					entry->cf_type = CF_ISTORE_SUM;
					strcpy(entry->custom_name, "sumMap");
				}
				if (strcmp(NameStr(procform->proname), "sum_up") == 0)
				{
					entry->cf_type = CF_ISTORE_SUM_UP;
					strcpy(entry->custom_name, "arraySum");
				}
				else if (strcmp(NameStr(procform->proname), "istore_seed") == 0)
				{
					entry->cf_type = CF_ISTORE_SEED;
					entry->custom_name[0] = '\1';	/* complex */
				}
				else if (strcmp(NameStr(procform->proname), "accumulate") == 0)
				{
					entry->cf_type = CF_ISTORE_ACCUMULATE;
					entry->custom_name[0] = '\1';	/* complex */
				}
				else if (strcmp(NameStr(procform->proname), "slice") == 0)
				{
					entry->cf_type = CF_UNSHIPPABLE;
					entry->custom_name[0] = '\0';	/* complex */
				}
			}
			else if (strcmp(extname, "country") == 0)
			{
				if (strcmp(NameStr(procform->proname), "country_common_name") == 0)
				{
					entry->cf_type = CF_UNSHIPPABLE;
					entry->custom_name[0] = '\0';	/* complex */
				}
			}
			else if (strcmp(extname, "ajtime") == 0)
			{
				if (strcmp(NameStr(procform->proname), "ajtime_to_timestamp") == 0)
				{
					entry->cf_type = CF_AJTIME_TO_TIMESTAMP;
					strcpy(entry->custom_name, "");
				}
				else if (strcmp(NameStr(procform->proname), "ajtime_pl_interval") == 0)
				{
					entry->cf_type = CF_AJTIME_PL_INTERVAL;
					strcpy(entry->custom_name, "addSeconds");
				}
				else if (strcmp(NameStr(procform->proname), "ajtime_mi_interval") == 0)
				{
					entry->cf_type = CF_AJTIME_MI_INTERVAL;
					strcpy(entry->custom_name, "subtractSeconds");
				}
				else if (strcmp(NameStr(procform->proname), "day_diff") == 0)
				{
					entry->cf_type = CF_AJTIME_DAY_DIFF;
					strcpy(entry->custom_name, "toInt32");
				}
				else if (strcmp(NameStr(procform->proname), "ajdate") == 0)
				{
					entry->cf_type = CF_AJTIME_AJDATE;
					strcpy(entry->custom_name, "toDate");
				}
				else if (strcmp(NameStr(procform->proname), "ajtime_out") == 0)
				{
					entry->cf_type = CF_AJTIME_OUT;
				}
			}
			else if (strcmp(extname, "ajbool") == 0)
			{
				if (strcmp(NameStr(procform->proname), "ajbool_out") == 0)
					entry->cf_type = CF_AJBOOL_OUT;
			}
			else if (strcmp(extname, "intarray") == 0)
			{
				if (strcmp(NameStr(procform->proname), "idx") == 0)
				{
					entry->cf_type = CF_INTARRAY_IDX;
					strcpy(entry->custom_name, "indexOf");
				}
			}
			else if (strcmp(extname, "pg_clickhouse") == 0)
			{
				entry->cf_type = CF_CH_FUNCTION;
				if (strcmp(proname, "argmax") == 0)
					strcpy(entry->custom_name, "argMax");
				else if (strcmp(proname, "argmin") == 0)
					strcpy(entry->custom_name, "argMin");
				else if (strcmp(proname, "uniqexact") == 0)
					strcpy(entry->custom_name, "uniqExact");
				else if (strcmp(proname, "uniqcombined") == 0)
					strcpy(entry->custom_name, "uniqCombined");
				else if (strcmp(proname, "uniqcombined64") == 0)
					strcpy(entry->custom_name, "uniqCombined64");
				else if (strcmp(proname, "uniqhll12") == 0)
					strcpy(entry->custom_name, "uniqHLL12");
				else if (strcmp(proname, "uniqtheta") == 0)
					strcpy(entry->custom_name, "uniqTheta");
				else if (strcmp(proname, "dictget") == 0)
					strcpy(entry->custom_name, "dictGet");
				else if (strcmp(proname, "params") == 0)
					entry->custom_name[0] = '\1'; // Will have no function name.
				else if (strcmp(proname, "quantileexact") == 0)
					strcpy(entry->custom_name, "quantileExact");
				else
					strcpy(entry->custom_name, proname);
			}
			ReleaseSysCache(proctup);
			pfree(extname);
		}
	}

	return entry;
}

FuncExpr * ch_get_params_function(TargetEntry *tle)
{
	Node	   *n = (Node *) tle->expr;
	if (nodeTag(n) != T_FuncExpr) return NULL;

	FuncExpr   *fe = (FuncExpr *) n;
	Oid extoid = getExtensionOfObject(ProcedureRelationId, fe->funcid);
	if (strcmp(get_extension_name(extoid), "pg_clickhouse") != 0) return NULL;
	if (strcmp(get_func_name(fe->funcid), "params") != 0) return NULL;

	return fe;
}

static Oid
find_rowfunc(char *procname, Oid rettype)
{
	Oid		argtypes[1] = {RECORDOID};
	Oid		procOid;
	List   *funcname = NIL;

	funcname = list_make1(makeString(procname));
	procOid = LookupFuncName(funcname, 1, argtypes, false);
	if (!OidIsValid(procOid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						func_signature_string(funcname, 1, NIL, argtypes))));


	if (get_func_rettype(procOid) != rettype)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("typmod_in function %s must return type %s",
						procname, format_type_be(rettype))));

	list_free_deep(funcname);
	return procOid;
}

CustomObjectDef *chfdw_check_for_custom_type(Oid typeoid)
{
	CustomObjectDef	*entry;
	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (chfdw_is_builtin(typeoid))
		return NULL;

	entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_FIND, NULL);
	if (!entry)
	{
		HeapTuple	tp;

		entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_ENTER, NULL);
		init_custom_entry(entry);

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(typeoid));
		if (HeapTupleIsValid(tp))
		{
			Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
			char *name = NameStr(typtup->typname);
			if (strcmp(name, "istore") == 0)
			{
				entry->cf_type = CF_ISTORE_TYPE; /* bigistore or istore */
				strcpy(entry->custom_name, "Tuple(Array(Int32), Array(Int64))");
				entry->rowfunc = find_rowfunc("row_to_istore", typeoid);
			}
			else if (strcmp(name, "bigistore") == 0)
			{
				entry->cf_type = CF_ISTORE_TYPE; /* bigistore or istore */
				strcpy(entry->custom_name, "Tuple(Array(Int32), Array(Int64))");
				entry->rowfunc = find_rowfunc("row_to_bigistore", typeoid);
			}
			else if (strcmp(name, "ajtime") == 0)
			{
				entry->cf_type = CF_AJTIME_TYPE; /* ajtime */
				strcpy(entry->custom_name, "timestamp");
			}
			else if (strcmp(name, "country") == 0)
			{
				entry->cf_type = CF_COUNTRY_TYPE; /* country type */
				strcpy(entry->custom_name, "text");
			}
			ReleaseSysCache(tp);
		}
	}

	return entry;
}

CustomObjectDef *chfdw_check_for_custom_operator(Oid opoid, Form_pg_operator form)
{
	HeapTuple	tuple = NULL;

	CustomObjectDef	*entry;
	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (chfdw_is_builtin(opoid))
	{
		switch (opoid) {
			/* timestamptz + interval */
			case F_TIMESTAMPTZ_PL_INTERVAL:
				break;
			default:
				return NULL;
		}
	}

	if (!form)
	{
		tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for operator %u", opoid);
		form = (Form_pg_operator) GETSTRUCT(tuple);
	}

	entry = hash_search(custom_objects_cache, (void *) &opoid, HASH_FIND, NULL);
	if (!entry)
	{
		entry = hash_search(custom_objects_cache, (void *) &opoid, HASH_ENTER, NULL);
		init_custom_entry(entry);

		if (opoid == F_TIMESTAMPTZ_PL_INTERVAL)
			entry->cf_type = CF_TIMESTAMPTZ_PL_INTERVAL;
		else
		{
			Oid		extoid = getExtensionOfObject(OperatorRelationId, opoid);
			char   *extname = get_extension_name(extoid);

			if (extname)
			{
				if (strcmp(extname, "ajtime") == 0)
					entry->cf_type = CF_AJTIME_OPERATOR;
				else if (strcmp(extname, "istore") == 0)
				{
					if (form && strcmp(NameStr(form->oprname), "->") == 0)
						entry->cf_type = CF_ISTORE_FETCHVAL;
				}
				else if (strcmp(extname, "hstore") == 0)
				{
					if (form && strcmp(NameStr(form->oprname), "->") == 0)
						entry->cf_type = CF_HSTORE_FETCHVAL;
				}
				pfree(extname);
			}
		}
	}

	if (tuple)
		ReleaseSysCache(tuple);

	return entry;
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
void
chfdw_apply_custom_table_options(CHFdwRelationInfo *fpinfo, Oid relid)
{
	ListCell	*lc;
	TupleDesc	tupdesc;
	int			attnum;
	Relation	rel;
	List	   *options;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		if (strcmp(def->defname, "engine") == 0)
		{
			static char *collapsing_text = "collapsingmergetree",
						*aggregating_text = "aggregatingmergetree";

			char *val = defGetString(def);
			if (strncasecmp(val, collapsing_text, strlen(collapsing_text)) == 0)
			{
				char   *start = index(val, '('),
					   *end = rindex(val, ')');

				fpinfo->ch_table_engine = CH_COLLAPSING_MERGE_TREE;
				if (start == end)
				{
					strcpy(fpinfo->ch_table_sign_field, "sign");
					continue;
				}

				if (end - start > NAMEDATALEN)
					elog(ERROR, "invalid format of ClickHouse engine");

				strncpy(fpinfo->ch_table_sign_field, start + 1, end - start - 1);
				fpinfo->ch_table_sign_field[end - start] = '\0';
			}
			else if (strncasecmp(val, aggregating_text, strlen(aggregating_text)) == 0)
			{
				fpinfo->ch_table_engine = CH_AGGREGATING_MERGE_TREE;
			}
		}
	}

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	rel = table_open_compat(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		bool				found;
		CustomObjectDef	   *cdef;
		CustomColumnInfo	entry_key,
						   *entry;
		custom_object_type	cf_type = CF_ISTORE_ARR;

		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		entry_key.relid = relid;
		entry_key.varattno = attnum;

		entry = hash_search(custom_columns_cache,
				(void *) &entry_key.relid, HASH_ENTER, &found);
		if (found)
			continue;

		entry->relid = relid;
		entry->varattno = attnum;
		entry->table_engine = fpinfo->ch_table_engine;
		entry->coltype = CF_USUAL;
		entry->is_AggregateFunction = CF_AGGR_USUAL;
		strcpy(entry->colname, NameStr(attr->attname));
		strcpy(entry->signfield, fpinfo->ch_table_sign_field);

		/* If a column has the column_name FDW option, use that value */
		options = GetForeignColumnOptions(relid, attnum);
		foreach (lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				strncpy(entry->colname, defGetString(def), NAMEDATALEN);
				entry->colname[NAMEDATALEN - 1] = '\0';
			}
			else if (strcmp(def->defname, "aggregatefunction") == 0)
			{
				entry->is_AggregateFunction = CF_AGGR_FUNC;
				cf_type = CF_ISTORE_COL;
			}
			else if (strcmp(def->defname, "simpleaggregatefunction") == 0)
			{
				entry->is_AggregateFunction = CF_AGGR_SIMPLE;
				cf_type = CF_ISTORE_COL;
			}
			else if (strcmp(def->defname, "arrays") == 0)
				cf_type = CF_ISTORE_ARR;
		}

		cdef = chfdw_check_for_custom_type(attr->atttypid);
		if (cdef && cdef->cf_type == CF_ISTORE_TYPE)
			entry->coltype = cf_type;
	}
	table_close_compat(rel, NoLock);
}

/* Get foreign relation options */
CustomColumnInfo *
chfdw_get_custom_column_info(Oid relid, uint16 varattno)
{
	CustomColumnInfo	entry_key,
					   *entry;

	entry_key.relid = relid;
	entry_key.varattno = varattno;

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	entry = hash_search(custom_columns_cache,
			(void *) &entry_key.relid, HASH_FIND, NULL);

	return entry;
}
