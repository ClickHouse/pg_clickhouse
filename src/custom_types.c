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

/*
 * Prior to PostgreSQL 14 these variables had other names or were undefined.
 * See postgres / include / server / utils / fmgroids.h
 */
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
#define F_ARRAY_AGG_ANYNONARRAY 2335
#define F_TO_TIMESTAMP_FLOAT8 1158

/*
 * Prior to Postgres 14 EXTRACT mapped directly to DATE_PART.
 * https://github.com / postgres / postgres / commit / a2da77cdb466
 */
#define F_EXTRACT_TEXT_TIMESTAMP 6202
#define F_EXTRACT_TEXT_TIMESTAMPTZ 6203
#define F_EXTRACT_TEXT_DATE 6199
#endif
/* regexp_like was added in Postgres 15; Mock it for earlier versions. */
#if PG_VERSION_NUM < 150000
#define F_REGEXP_LIKE_TEXT_TEXT 6263
#endif

/* Pre-PG14 compat for function OIDs used in new translations. */
#if PG_VERSION_NUM < 140000
#define F_SPLIT_PART 2088
#define F_REGEXP_REPLACE_TEXT_TEXT_TEXT 2284
#define F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT 2285
#define F_CONCAT_WS 3059
#define F_ARRAY_TO_STRING_ANYARRAY_TEXT 395
#define F_ARRAY_TO_STRING_ANYARRAY_TEXT_TEXT 384
#define F_TO_CHAR_TIMESTAMP_TEXT 2049
#define F_TO_CHAR_TIMESTAMPTZ_TEXT 1770
#endif

#define STR_STARTS_WITH(str, sub) strncmp(str, sub, strlen(sub)) == 0
#define STR_EQUAL(a, b) strcmp(a, b) == 0

static HTAB * custom_objects_cache = NULL;
static HTAB
* custom_columns_cache = NULL;

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
init_custom_entry(CustomObjectDef * entry)
{
	entry->cf_type = CF_USUAL;
	entry->custom_name[0] = '\0';
	entry->cf_context = NULL;
	entry->rowfunc = InvalidOid;
	entry->paren_count = 1;
}

/*
 * Return true if ordered aggregate funcid maps to a parameterized ClickHouse
 * aggregate function.
 */
inline bool
chfdw_check_for_ordered_aggregate(Aggref * agg)
{
	switch (agg->aggfnoid)
	{
		case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
		case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
			/* Ordered aggregates that map to ClickHouse functions. */
			return true;
	}

	/* Accept all ordered aggregates defined by pg_clickhouse. */
	Oid			extoid = getExtensionOfObject(ProcedureRelationId, agg->aggfnoid);
	char	   *extname = get_extension_name(extoid);

	return STR_EQUAL(extname, "pg_clickhouse");
}

/*
 * Map pg_clickhouse pushdown function names to ClickHouse case-sensitive
 * names. Must be kept in lexicographic order.
 */
static char *ch_func_map[][2] = {
	{"argmax", "argMax"},
	{"argmin", "argMin"},
	{"dictget", "dictGet"},
	{"quantileexact", "quantileExact"},
	{"touint128", "toUInt128"},
	{"touint16", "toUInt16"},
	{"touint32", "toUInt32"},
	{"touint64", "toUInt64"},
	{"touint8", "toUInt8"},
	{"uniqcombined", "uniqCombined"},
	{"uniqcombined64", "uniqCombined64"},
	{"uniqexact", "uniqExact"},
	{"uniqhll12", "uniqHLL12"},
	{"uniqtheta", "uniqTheta"},
	{NULL, NULL},
};

inline static char *
ch_func_name(char *proname)
{
	size_t		i = 0;

	while (ch_func_map[i][0] != NULL)
	{
		if (STR_EQUAL(ch_func_map[i][0], proname))
			return ch_func_map[i][1];
		i++;
	}
	return proname;
}

CustomObjectDef *
chfdw_check_for_custom_function(Oid funcid)
{
	bool		special_builtin = false;
	CustomObjectDef *entry;

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
			case F_ARRAY_AGG_ANYNONARRAY:
			case F_MD5_BYTEA:
			case F_MD5_TEXT:
			case F_TO_TIMESTAMP_FLOAT8:
			case F_SPLIT_PART:
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT:
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT:
			case F_CONCAT_WS:
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT:
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT_TEXT:
			case F_TO_CHAR_TIMESTAMP_TEXT:
			case F_TO_CHAR_TIMESTAMPTZ_TEXT:
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
			case F_ARRAY_AGG_ANYNONARRAY:
				{
					strcpy(entry->custom_name, "groupArray");
					break;
				}
			case F_MD5_BYTEA:
			case F_MD5_TEXT:
				{
					/* Special hashing function returns lowercase hex. */
					strcpy(entry->custom_name, "lower(hex(MD5");
					entry->paren_count = 3;
					break;
				}
			case F_TO_TIMESTAMP_FLOAT8:
				{
					/*
					 * ClickHouse doesn't work with subsecond precision
					 * timestamps.
					 */
					strcpy(entry->custom_name, "fromUnixTimestamp(toInt64");
					entry->paren_count = 2;
					break;
				}
			case F_SPLIT_PART:
				{
					entry->cf_type = CF_SPLIT_PART;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT:
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT:
				{
					entry->cf_type = CF_REGEXP_REPLACE;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_CONCAT_WS:
				{
					entry->cf_type = CF_CONCAT_WS;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT:
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT_TEXT:
				{
					strcpy(entry->custom_name, "arrayStringConcat");
					break;
				}
			case F_TO_CHAR_TIMESTAMP_TEXT:
			case F_TO_CHAR_TIMESTAMPTZ_TEXT:
				{
					entry->cf_type = CF_TO_CHAR;
					entry->custom_name[0] = '\1';
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
			char	   *proname;

			proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(proctup))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			procform = (Form_pg_proc) GETSTRUCT(proctup);
			proname = NameStr(procform->proname);

			if (STR_EQUAL(extname, "intarray"))
			{
				if (STR_EQUAL(proname, "idx"))
				{
					entry->cf_type = CF_INTARRAY_IDX;
					strcpy(entry->custom_name, "indexOf");
				}
			}
			else if (STR_EQUAL(extname, "pg_clickhouse"))
			{
				/* pg_clickhouse custom functions. */
				entry->cf_type = CF_CH_FUNCTION;
				strcpy(entry->custom_name, ch_func_name(proname));
			}
			ReleaseSysCache(proctup);
			pfree(extname);
		}
	}

	return entry;
}

CustomObjectDef *
chfdw_check_for_custom_type(Oid typeoid)
{
	CustomObjectDef *entry;

	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (chfdw_is_builtin(typeoid))
		return NULL;

	entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_FIND, NULL);
	if (!entry)
	{
		entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_ENTER, NULL);
		init_custom_entry(entry);
	}

	return entry;
}

/*
 * Map a builtin operator name to its custom_object_type.  Returns CF_USUAL
 * when the operator needs no special handling and should follow the normal
 * builtin shortcut (i.e. be presumed shippable with no rewrite).
 *
 * Keep this function in sync with the CF_* enum in fdw.h.
 */
static custom_object_type
classify_builtin_operator(const char *oprname)
{
	if (strcmp(oprname, "~") == 0)
		return CF_REGEX_MATCH;
	if (strcmp(oprname, "!~") == 0)
		return CF_REGEX_NO_MATCH;
	if (strcmp(oprname, "~*") == 0)
		return CF_REGEX_ICASE_MATCH;
	if (strcmp(oprname, "!~*") == 0)
		return CF_REGEX_ICASE_NO_MATCH;
	return CF_USUAL;
}

CustomObjectDef *
chfdw_check_for_custom_operator(Oid opoid, Form_pg_operator form)
{
	HeapTuple	tuple = NULL;
	custom_object_type ctype;

	CustomObjectDef *entry;

	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (chfdw_is_builtin(opoid))
	{
		switch (opoid)
		{
				/* timestamptz + interval */
			case F_TIMESTAMPTZ_PL_INTERVAL:
				break;
			default:

				/* Look up the operator name so we can classify it. */
				if (!form)
				{
					tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
					if (!HeapTupleIsValid(tuple))
						ereport(ERROR,
								errcode(ERRCODE_INTERNAL_ERROR),
								errmsg("pg_clickhouse: cache lookup failed for operator %u", opoid));
					form = (Form_pg_operator) GETSTRUCT(tuple);
				}

				ctype = classify_builtin_operator(NameStr(form->oprname));
				if (ctype != CF_USUAL)
					break;		/* fall through to cache + classify below */

				if (tuple)
					ReleaseSysCache(tuple);
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
		else if (form && classify_builtin_operator(NameStr(form->oprname)) != CF_USUAL)
			entry->cf_type = classify_builtin_operator(NameStr(form->oprname));
		else
		{
			Oid			extoid = getExtensionOfObject(OperatorRelationId, opoid);
			char	   *extname = get_extension_name(extoid);

			if (extname)
			{
				if (STR_EQUAL(extname, "hstore"))
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
chfdw_apply_custom_table_options(CHFdwRelationInfo * fpinfo, Oid relid)
{
	ListCell   *lc;
	TupleDesc	tupdesc;
	int			attnum;
	Relation	rel;
	List	   *options;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (STR_EQUAL(def->defname, "engine"))
		{
			static char *collapsing_text = "collapsingmergetree",
					   *aggregating_text = "aggregatingmergetree";

			char	   *val = defGetString(def);

			if (strncasecmp(val, collapsing_text, strlen(collapsing_text)) == 0)
			{
				char	   *start = index(val, '('),
						   *end = rindex(val, ')');
				char		sign[CH_ESCAPED_NAMEDATALEN];

				if (end - start - 1 > (CH_ESCAPED_NAMEDATALEN - 1))
					elog(ERROR, "pg_clickhouse: invalid engine parameter");

				fpinfo->ch_table_engine = CH_COLLAPSING_MERGE_TREE;
				strncpy(sign, start + 1, end - start - 1);
				sign[end - start - 1] = '\0';
				strcpy(fpinfo->ch_table_sign_field, ch_quote_ident(sign));
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
		bool		found;
		CustomColumnInfo entry_key,
				   *entry;
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

		/* If a column has the column_name FDW option, use that value */
		options = GetForeignColumnOptions(relid, attnum);
		foreach(lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (STR_EQUAL(def->defname, "column_name"))
			{
				strncpy(entry->colname, defGetString(def), NAMEDATALEN);
				entry->colname[NAMEDATALEN - 1] = '\0';
			}
			else if (STR_EQUAL(def->defname, "aggregatefunction"))
			{
				entry->is_AggregateFunction = CF_AGGR_FUNC;
			}
			else if (STR_EQUAL(def->defname, "simpleaggregatefunction"))
			{
				entry->is_AggregateFunction = CF_AGGR_SIMPLE;
			}
		}
	}
	table_close_compat(rel, NoLock);
}

/* Get foreign relation options */
CustomColumnInfo *
chfdw_get_custom_column_info(Oid relid, uint16 varattno)
{
	CustomColumnInfo entry_key,
			   *entry;

	entry_key.relid = relid;
	entry_key.varattno = varattno;

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	entry = hash_search(custom_columns_cache,
						(void *) &entry_key.relid, HASH_FIND, NULL);

	return entry;
}
