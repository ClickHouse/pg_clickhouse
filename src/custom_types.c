#include "postgres.h"
#include "strings.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "catalog/dependency.h"
#include "catalog/pg_operator_d.h"
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
#define F_TRANSACTION_TIMESTAMP 2647
#define F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT 2767
#define F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT_TEXT 2768
#define F_REGEXP_REPLACE_TEXT_TEXT_TEXT 2284
#define F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT 2285

/*
 * Prior to Postgres 14 EXTRACT mapped directly to DATE_PART.
 * https://github.com / postgres / postgres / commit / a2da77cdb466
 */
#define F_EXTRACT_TEXT_TIMESTAMP 6202
#define F_EXTRACT_TEXT_TIMESTAMPTZ 6203
#define F_EXTRACT_TEXT_DATE 6199
#define F_TRIM_ARRAY 6172
#define F_STRING_TO_ARRAY_TEXT_TEXT 394
#define F_STRING_TO_ARRAY_TEXT_TEXT_TEXT 376
#define F_SPLIT_PART 2088
#define F_ARRAY_TO_STRING_ANYARRAY_TEXT 395
#define F_ARRAY_TO_STRING_ANYARRAY_TEXT_TEXT 384
#define F_ARRAY_FILL_ANYELEMENT__INT4 F_ARRAY_FILL
#define F_ARRAY_FILL_ANYELEMENT__INT4__INT4 F_ARRAY_FILL_WITH_LOWER_BOUNDS
#define F_CARDINALITY F_ARRAY_CARDINALITY
#endif
/* regexp_like was added in Postgres 15 */
#if PG_VERSION_NUM < 150000
#define F_REGEXP_LIKE_TEXT_TEXT 6263
#define F_REGEXP_LIKE_TEXT_TEXT_TEXT 6264
#endif
/* array_shuffle, array_sample added in Postgres 16 */
#if PG_VERSION_NUM < 160000
#define F_ARRAY_SHUFFLE 6215
#define F_ARRAY_SAMPLE 6216
#endif
/* array_reverse, array_sort added in Postgres 18 */
#if PG_VERSION_NUM < 180000
#define F_ARRAY_REVERSE 6381
#define F_ARRAY_SORT_ANYARRAY 6388
#define F_ARRAY_SORT_ANYARRAY_BOOL 6389
#define F_ARRAY_SORT_ANYARRAY_BOOL_BOOL 6390
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
 * Map sans-prefix pg_re2 function names to ClickHouse
 * case-sensitive names. Must be kept in lexicographic order.
 */
static char *re2_func_map[][2] = {
	{"countmatches", "countMatches"},
	{"countmatchescaseinsensitive", "countMatchesCaseInsensitive"},
	{"extractall", "extractAll"},
	{"extractgroups", "extractGroups"},
	{"multimatchallindices", "multiMatchAllIndices"},
	{"multimatchany", "multiMatchAny"},
	{"multimatchanyindex", "multiMatchAnyIndex"},
	{"regexpextract", "regexpExtract"},
	{"replaceregexpall", "replaceRegexpAll"},
	{"replaceregexpone", "replaceRegexpOne"},
	{NULL, NULL},
};

inline static char *
re2_func_name(char *proname)
{
	Assert(strncmp(proname, "re2", 3) == 0);
	char	   *stripped = proname + 3;
	size_t		i = 0;

	while (re2_func_map[i][0] != NULL)
	{
		if (STR_EQUAL(re2_func_map[i][0], stripped))
			return re2_func_map[i][1];
		i++;
	}
	return stripped;
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
			case F_REGEXP_LIKE_TEXT_TEXT_TEXT:
			case F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT:
			case F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT_TEXT:
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT:
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT:
			case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
			case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
			case F_ARRAY_AGG_ANYNONARRAY:
			case F_MD5_BYTEA:
			case F_MD5_TEXT:
			case F_TO_TIMESTAMP_FLOAT8:
			case F_JSONB_EXTRACT_PATH:
			case F_JSONB_EXTRACT_PATH_TEXT:
			case F_NOW:
			case F_STATEMENT_TIMESTAMP:
			case F_TRANSACTION_TIMESTAMP:
			case F_CLOCK_TIMESTAMP:
			case F_CURRENT_SCHEMA:
			case F_CURRENT_DATABASE:
				/* array functions: simple mappings */
			case F_ARRAY_CAT:
			case F_ARRAY_APPEND:
			case F_ARRAY_REMOVE:
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT:
			case F_CARDINALITY:
			case F_ARRAY_REVERSE:
			case F_ARRAY_SORT_ANYARRAY:
			case F_ARRAY_SHUFFLE:
			case F_ARRAY_SAMPLE:
				/* array functions: arg rewriting */
			case F_ARRAY_LENGTH:
			case F_ARRAY_PREPEND:
			case F_STRING_TO_ARRAY_TEXT_TEXT:
			case F_SPLIT_PART:
			case F_TRIM_ARRAY:
			case F_ARRAY_FILL_ANYELEMENT__INT4:
			case F_ARRAY_SORT_ANYARRAY_BOOL:
				/* array functions: unshippable */
			case F_ARRAY_DIMS:
			case F_ARRAY_NDIMS:
			case F_ARRAY_LOWER:
			case F_ARRAY_UPPER:
			case F_ARRAY_REPLACE:
			case F_ARRAY_POSITIONS:
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT_TEXT:
			case F_STRING_TO_ARRAY_TEXT_TEXT_TEXT:
			case F_ARRAY_FILL_ANYELEMENT__INT4__INT4:
			case F_ARRAY_SORT_ANYARRAY_BOOL_BOOL:
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
			case F_REGEXP_LIKE_TEXT_TEXT_TEXT:
				{
					entry->cf_type = CF_MATCH;
					strcpy(entry->custom_name, "match");
					break;
				}
			case F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT:
			case F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT_TEXT:
				{
					entry->cf_type = CF_SPLIT_BY_REGEX;
					strcpy(entry->custom_name, "splitByRegexp");
					break;
				}
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT:
			case F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT:
				{
					entry->cf_type = CF_REPLACE_REGEX;
					entry->custom_name[0] = '\1';
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
			case F_JSONB_EXTRACT_PATH_TEXT:
				{
					entry->cf_type = CF_JSONB_EXTRACT_PATH_TEXT;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_JSONB_EXTRACT_PATH:
				{
					entry->cf_type = CF_JSONB_EXTRACT_PATH;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_NOW:
				{
					/*
					 * Postgres NOW() produces subsecond precision, to map to
					 * now64()
					 */
					strcpy(entry->custom_name, "now64");
					break;
				}
			case F_STATEMENT_TIMESTAMP:
			case F_TRANSACTION_TIMESTAMP:
			case F_CLOCK_TIMESTAMP:
				{
					entry->cf_type = CF_CLOCK_TIMESTAMP;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_CURRENT_SCHEMA:
				{
					entry->cf_type = CF_CURRENT_SCHEMA;
					entry->custom_name[0] = '\1';
					break;
				}
			case F_CURRENT_DATABASE:
				{
					entry->cf_type = CF_CURRENT_DATABASE;
					entry->custom_name[0] = '\1';
					break;
				}
				/* array functions: unshippable */
			case F_ARRAY_DIMS:
			case F_ARRAY_NDIMS:
			case F_ARRAY_LOWER:
			case F_ARRAY_UPPER:
			case F_ARRAY_REPLACE:
			case F_ARRAY_POSITIONS:
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT_TEXT:
			case F_STRING_TO_ARRAY_TEXT_TEXT_TEXT:
			case F_ARRAY_FILL_ANYELEMENT__INT4__INT4:
			case F_ARRAY_SORT_ANYARRAY_BOOL_BOOL:
				entry->cf_type = CF_UNSHIPPABLE;
				break;
				/* array functions: simple mappings */
			case F_ARRAY_CAT:
				strcpy(entry->custom_name, "arrayConcat");
				break;
			case F_ARRAY_APPEND:
				strcpy(entry->custom_name, "arrayPushBack");
				break;
			case F_ARRAY_REMOVE:
				strcpy(entry->custom_name, "arrayRemove");
				break;
			case F_ARRAY_TO_STRING_ANYARRAY_TEXT:
				strcpy(entry->custom_name, "arrayStringConcat");
				break;
			case F_CARDINALITY:
			case F_ARRAY_LENGTH:
				entry->cf_type = CF_ARRAY_LENGTH;
				strcpy(entry->custom_name, "length");
				break;
			case F_ARRAY_REVERSE:
				strcpy(entry->custom_name, "arrayReverse");
				break;
			case F_ARRAY_SORT_ANYARRAY:
				strcpy(entry->custom_name, "arraySort");
				break;
			case F_ARRAY_SHUFFLE:
				strcpy(entry->custom_name, "arrayShuffle");
				break;
			case F_ARRAY_SAMPLE:
				strcpy(entry->custom_name, "arrayRandomSample");
				break;
			case F_ARRAY_PREPEND:
				entry->cf_type = CF_ARRAY_PREPEND;
				strcpy(entry->custom_name, "arrayPushFront");
				break;
			case F_STRING_TO_ARRAY_TEXT_TEXT:
				entry->cf_type = CF_STRING_TO_ARRAY;
				strcpy(entry->custom_name, "splitByString");
				break;
			case F_SPLIT_PART:
				entry->cf_type = CF_STRING_TO_ARRAY_PART;
				strcpy(entry->custom_name, "splitByString");
				break;
			case F_TRIM_ARRAY:
				entry->cf_type = CF_TRIM_ARRAY;
				strcpy(entry->custom_name, "arrayResize");
				break;
			case F_ARRAY_FILL_ANYELEMENT__INT4:
				entry->cf_type = CF_ARRAY_FILL;
				strcpy(entry->custom_name, "arrayWithConstant");
				break;
			case F_ARRAY_SORT_ANYARRAY_BOOL:
				entry->cf_type = CF_ARRAY_SORT_DESC;
				entry->custom_name[0] = '\1';
				break;
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
			else if (STR_EQUAL(extname, "re2"))
			{
				/* pg_re2: 1:1 pushdown to ClickHouse RE2 functions. */
				entry->cf_type = CF_CH_FUNCTION;
				strlcpy(entry->custom_name, re2_func_name(proname), NAMEDATALEN);
			}
			else if (STR_EQUAL(extname, "fuzzystrmatch"))
			{
				if (STR_EQUAL(proname, "levenshtein") &&
					procform->pronargs == 2)
					strcpy(entry->custom_name, "editDistance");
				else if (!(STR_EQUAL(proname, "soundex")))
					entry->cf_type = CF_UNSHIPPABLE;
			}
			else if (STR_EQUAL(extname, "pg_clickhouse"))
			{
				/* pg_clickhouse custom functions. */
				entry->cf_type = CF_CH_FUNCTION;
				strlcpy(entry->custom_name, ch_func_name(proname), NAMEDATALEN);
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

/* pg_operator_d.h only has oid_symbol for some operators */
#define OID_TEXT_REGEX_NE_OP		642
#define OID_TEXT_IREGEX_NE_OP		1229
#define OID_JSONB_FETCHVAL_OP		3211
#define OID_JSONB_FETCHVAL_TEXT_OP	3477

/*
 * Map a builtin operator OID to its custom_object_type. Returns CF_USUAL
 * when the operator needs no special handling.
 */
static custom_object_type
classify_builtin_operator(Oid opoid)
{
	switch (opoid)
	{
		case OID_TEXT_REGEXEQ_OP:
			return CF_REGEX_MATCH;
		case OID_TEXT_REGEX_NE_OP:
			return CF_REGEX_NO_MATCH;
		case OID_TEXT_ICREGEXEQ_OP:
			return CF_REGEX_ICASE_MATCH;
		case OID_TEXT_IREGEX_NE_OP:
			return CF_REGEX_ICASE_NO_MATCH;
		case OID_JSONB_FETCHVAL_OP:
			return CF_JSONB_FETCHVAL;
		case OID_JSONB_FETCHVAL_TEXT_OP:
			return CF_JSONB_FETCHVAL_TEXT;
		case OID_ARRAY_CONTAINS_OP:
			return CF_ARRAY_CONTAINS;
		case OID_ARRAY_CONTAINED_OP:
			return CF_ARRAY_CONTAINED_BY;
		case OID_ARRAY_OVERLAP_OP:
			return CF_ARRAY_OVERLAP;
		default:
			return CF_USUAL;
	}
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
		ctype = classify_builtin_operator(opoid);
		if (ctype == CF_USUAL && opoid != F_TIMESTAMPTZ_PL_INTERVAL)
		{
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

		ctype = classify_builtin_operator(opoid);
		if (opoid == F_TIMESTAMPTZ_PL_INTERVAL)
			entry->cf_type = CF_TIMESTAMPTZ_PL_INTERVAL;
		else if (ctype != CF_USUAL)
			entry->cf_type = ctype;
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
static void
			populate_custom_column_entry(CustomColumnInfo * entry, Oid relid,
										 AttrNumber attnum, const char *attname,
										 CHRemoteTableEngine table_engine);

void
chfdw_apply_custom_table_options(CHFdwRelationInfo * fpinfo, Oid relid)
{
	ListCell   *lc;
	TupleDesc	tupdesc;
	int			attnum;
	Relation	rel;

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
					ereport(ERROR,
							(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
							 errmsg("pg_clickhouse: invalid engine parameter")));

				fpinfo->ch_table_engine = CH_COLLAPSING_MERGE_TREE;
				strncpy(sign, start + 1, end - start - 1);
				sign[end - start - 1] = '\0';
				strlcpy(fpinfo->ch_table_sign_field, ch_quote_ident(sign), CH_ESCAPED_NAMEDATALEN);
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

		populate_custom_column_entry(entry, relid, attnum,
									 NameStr(attr->attname),
									 fpinfo->ch_table_engine);
	}
	table_close_compat(rel, NoLock);
}

/*
 * Populate one cache entry from pg_attribute + FDW column options.
 * Caller has already inserted the entry under (relid, attnum).
 */
static void
populate_custom_column_entry(CustomColumnInfo * entry, Oid relid,
							 AttrNumber attnum, const char *attname,
							 CHRemoteTableEngine table_engine)
{
	List	   *options;
	ListCell   *lc;

	entry->relid = relid;
	entry->varattno = attnum;
	entry->table_engine = table_engine;
	entry->coltype = CF_USUAL;
	entry->is_AggregateFunction = CF_AGGR_USUAL;
	strlcpy(entry->colname, attname, NAMEDATALEN);

	/* column_name FDW option overrides attname */
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

/*
 * Look up CustomColumnInfo for (relid, varattno), populating on demand.
 * INSERT paths skip GetForeignRelSize, so eager priming via
 * chfdw_apply_custom_table_options does not run, leaving deparse without
 * column_name overrides. Populate lazily so any caller sees them.
 */
CustomColumnInfo *
chfdw_get_custom_column_info(Oid relid, uint16 varattno)
{
	CustomColumnInfo entry_key,
			   *entry;
	bool		found;

	entry_key.relid = relid;
	entry_key.varattno = varattno;

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	entry = hash_search(custom_columns_cache,
						(void *) &entry_key.relid, HASH_ENTER, &found);
	if (!found)
	{
		Relation	rel;
		TupleDesc	tupdesc;

		rel = table_open_compat(relid, NoLock);
		tupdesc = RelationGetDescr(rel);

		if (varattno > 0 && varattno <= tupdesc->natts)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno - 1);

			if (!attr->attisdropped)
				populate_custom_column_entry(entry, relid, varattno,
											 NameStr(attr->attname),
											 CH_DEFAULT);
			else
			{
				hash_search(custom_columns_cache,
							(void *) &entry_key.relid, HASH_REMOVE, NULL);
				entry = NULL;
			}
		}
		else
		{
			hash_search(custom_columns_cache,
						(void *) &entry_key.relid, HASH_REMOVE, NULL);
			entry = NULL;
		}
		table_close_compat(rel, NoLock);
	}

	return entry;
}
