#include "postgres.h"

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
#include "strings.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "fdw.h"

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
/* array_reverse, array_sort, reverse(bytea) added in Postgres 18 */
#if PG_VERSION_NUM < 180000
#define F_ARRAY_REVERSE 6381
#define F_ARRAY_SORT_ANYARRAY 6388
#define F_ARRAY_SORT_ANYARRAY_BOOL 6389
#define F_ARRAY_SORT_ANYARRAY_BOOL_BOOL 6390
/* before PG 18 reverse(text) was unique so macro was F_REVERSE; reverse(bytea) didn't
 * exist */
#define F_REVERSE_TEXT F_REVERSE
#define F_REVERSE_BYTEA 6382
#endif

#define STR_STARTS_WITH(str, sub) strncmp(str, sub, strlen(sub)) == 0
#define STR_EQUAL(a, b) strcmp(a, b) == 0

static HTAB* custom_objects_cache = NULL;
static HTAB* custom_columns_cache = NULL;

static HTAB*
create_custom_objects_cache(void) {
    HASHCTL ctl;

    ctl.keysize   = sizeof(Oid);
    ctl.entrysize = sizeof(CustomObjectDef);

    return hash_create(
        "pg_clickhouse custom functions", 20, &ctl, HASH_ELEM | HASH_BLOBS
    );
}

static void
invalidate_custom_columns_cache(Datum arg, int cacheid, uint32 hashvalue) {
    HASH_SEQ_STATUS status;
    CustomColumnInfo* entry;

    hash_seq_init(&status, custom_columns_cache);
    while ((entry = (CustomColumnInfo*)hash_seq_search(&status)) != NULL) {
        if (hash_search(
                custom_columns_cache, (void*)&entry->relid, HASH_REMOVE, NULL
            ) == NULL) {
            elog(ERROR, "hash table corrupted");
        }
    }
}

static HTAB*
create_custom_columns_cache(void) {
    HASHCTL ctl;

    ctl.keysize   = sizeof(Oid) + sizeof(int);
    ctl.entrysize = sizeof(CustomColumnInfo);

    CacheRegisterSyscacheCallback(ATTNUM, invalidate_custom_columns_cache, (Datum)0);

    return hash_create(
        "pg_clickhouse custom functions", 20, &ctl, HASH_ELEM | HASH_BLOBS
    );
}

inline static void
init_custom_entry(CustomObjectDef* entry) {
    entry->cf_type        = CF_USUAL;
    entry->custom_name[0] = '\0';
    entry->cf_context     = NULL;
    entry->paren_count    = 1;
}

/*
 * Return true if ordered aggregate funcid maps to a parametric ClickHouse
 * aggregate function.
 */
inline bool
chfdw_check_for_ordered_aggregate(Aggref* agg) {
    switch (agg->aggfnoid) {
    case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
    case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
    case F_PERCENTILE_DISC_FLOAT8_ANYELEMENT:
    case F_PERCENTILE_DISC__FLOAT8_ANYELEMENT:
    case F_PERCENTILE_CONT__FLOAT8_FLOAT8:
    case F_PERCENTILE_CONT__FLOAT8_INTERVAL:
        /* Ordered aggregates that map to ClickHouse functions. */
        return true;
    }

    /* Accept all ordered aggregates defined by pg_clickhouse. */
    Oid extoid = getExtensionOfObject(ProcedureRelationId, agg->aggfnoid);
    return get_extension_oid("pg_clickhouse", true) == extoid;
}

/*
 * Map sans-prefix pg_re2 function names to ClickHouse
 * case-sensitive names. Must be kept in lexicographic order.
 */
static char* re2_func_map[][2] = {
    { "countmatches",                "countMatches"                },
    { "countmatchescaseinsensitive", "countMatchesCaseInsensitive" },
    { "extractall",                  "extractAll"                  },
    { "extractallgroupshorizontal",  "extractAllGroupsHorizontal"  },
    { "extractallgroupsvertical",    "extractAllGroupsVertical"    },
    { "extractgroups",               "extractGroups"               },
    { "multimatchallindices",        "multiMatchAllIndices"        },
    { "multimatchany",               "multiMatchAny"               },
    { "multimatchanyindex",          "multiMatchAnyIndex"          },
    { "regexpextract",               "regexpExtract"               },
    { "regexpquotemeta",             "regexpQuoteMeta"             },
    { "replaceregexpall",            "replaceRegexpAll"            },
    { "replaceregexpone",            "replaceRegexpOne"            },
    { "splitbyregexp",               "splitByRegexp"               },
    { NULL,                          NULL                          },
};

inline static char*
re2_func_name(char* proname) {
    Assert(strncmp(proname, "re2", 3) == 0);
    char* stripped = proname + 3;
    size_t i       = 0;

    while (re2_func_map[i][0] != NULL) {
        if (STR_EQUAL(re2_func_map[i][0], stripped)) {
            return re2_func_map[i][1];
        }
        i++;
    }
    return stripped;
}

/*
 * Map pg_clickhouse pushdown function names to ClickHouse case-sensitive
 * names. Must be kept in lexicographic order.
 */
static char* ch_func_map[][2] = {
    { "argmax",         "argMax"         },
    { "argmin",         "argMin"         },
    { "dictget",        "dictGet"        },
    { "quantileexact",  "quantileExact"  },
    { "touint128",      "toUInt128"      },
    { "touint16",       "toUInt16"       },
    { "touint32",       "toUInt32"       },
    { "touint64",       "toUInt64"       },
    { "touint8",        "toUInt8"        },
    { "uniqcombined",   "uniqCombined"   },
    { "uniqcombined64", "uniqCombined64" },
    { "uniqexact",      "uniqExact"      },
    { "uniqhll12",      "uniqHLL12"      },
    { "uniqtheta",      "uniqTheta"      },
    { NULL,             NULL             },
};

inline static char*
ch_func_name(char* proname) {
    size_t i = 0;

    while (ch_func_map[i][0] != NULL) {
        if (STR_EQUAL(ch_func_map[i][0], proname)) {
            return ch_func_map[i][1];
        }
        i++;
    }
    return proname;
}

/*
 * Describes how a recognized builtin maps to ClickHouse.
 *   ch_name = literal: deparse emits ch_name
 *   ch_name = NULL:    deparse emits PG name
 *   ch_name = "\1":    specialized cf_type handler
 */
typedef struct {
    custom_object_type cf_type;
    const char* ch_name;
    int paren_count;
} builtin_func_def;

/*
 * Classify a builtin function and, when we rewrite it, describe the
 * ClickHouse form. Returns false for builtins we do not handle.
 *
 * ch_name "\1" means deparse.c emits the name itself; leaving a field
 * untouched keeps the caller-supplied default (PG name, CF_USUAL, 1 paren).
 */
static bool
lookup_builtin_func(Oid funcid, builtin_func_def* def) {
    switch (funcid) {
    case F_DATE_TRUNC_TEXT_TIMESTAMPTZ:
    case F_DATE_TRUNC_TEXT_TIMESTAMP:
        def->cf_type = CF_DATE_TRUNC;
        def->ch_name = "\1";
        return true;
    case F_DATE_PART_TEXT_TIMESTAMPTZ:
    case F_DATE_PART_TEXT_TIMESTAMP:
    case F_DATE_PART_TEXT_DATE:
    case F_EXTRACT_TEXT_TIMESTAMP:
    case F_EXTRACT_TEXT_TIMESTAMPTZ:
    case F_EXTRACT_TEXT_DATE:
        def->cf_type = CF_DATE_PART;
        def->ch_name = "\1";
        return true;
    case F_TIMEZONE_TEXT_TIMESTAMP:
    case F_TIMEZONE_TEXT_TIMESTAMPTZ:
        def->cf_type = CF_TIMEZONE;
        def->ch_name = "toTimeZone";
        return true;
    case F_ARRAY_POSITION_ANYCOMPATIBLEARRAY_ANYCOMPATIBLE:
    case F_ARRAY_POSITION_ANYCOMPATIBLEARRAY_ANYCOMPATIBLE_INT4:
        def->cf_type = CF_ARRAY_POSITION;
        def->ch_name = "\1";
        return true;
    case F_BTRIM_TEXT_TEXT:
    case F_BTRIM_TEXT:
        def->ch_name = "trimBoth";
        return true;
    case F_STRPOS:
        def->ch_name = "positionUTF8";
        return true;
        /* PG strpos counts code points, CH position counts bytes */
    case F_LOWER_TEXT:
        def->ch_name = "lowerUTF8";
        return true;
        /* PG lower(text) is locale-aware on code points */
    case F_UPPER_TEXT:
        def->ch_name = "upperUTF8";
        return true;
        /* PG upper(text) is locale-aware on code points */
    case F_SUBSTR_TEXT_INT4_INT4:
    case F_SUBSTR_TEXT_INT4:
    case F_SUBSTRING_TEXT_INT4_INT4:
    case F_SUBSTRING_TEXT_INT4:
        def->ch_name = "substringUTF8";
        return true;
        /* PG substring(text, ...) counts code points */
    case F_SUBSTR_BYTEA_INT4_INT4:
    case F_SUBSTR_BYTEA_INT4:
    case F_SUBSTRING_BYTEA_INT4_INT4:
    case F_SUBSTRING_BYTEA_INT4:
        def->ch_name = "substring";
        return true;
        /* bytea variant is byte-based; CH substring matches */
    case F_REGEXP_LIKE_TEXT_TEXT:
    case F_REGEXP_LIKE_TEXT_TEXT_TEXT:
        def->cf_type = CF_MATCH;
        def->ch_name = "match";
        return true;
    case F_REGEXP_MATCH_TEXT_TEXT:
    case F_REGEXP_MATCH_TEXT_TEXT_TEXT:
        def->cf_type = CF_REGEX_PG_MATCH;
        def->ch_name = "\1";
        return true;
    case F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT:
    case F_REGEXP_SPLIT_TO_ARRAY_TEXT_TEXT_TEXT:
        def->cf_type = CF_SPLIT_BY_REGEX;
        def->ch_name = "splitByRegexp";
        return true;
    case F_REGEXP_REPLACE_TEXT_TEXT_TEXT:
    case F_REGEXP_REPLACE_TEXT_TEXT_TEXT_TEXT:
        def->cf_type = CF_REPLACE_REGEX;
        def->ch_name = "\1";
        return true;
    case F_PERCENTILE_CONT_FLOAT8_FLOAT8:
    case F_PERCENTILE_CONT_FLOAT8_INTERVAL:
        def->ch_name = "quantile";
        return true;
    case F_PERCENTILE_CONT__FLOAT8_FLOAT8:
    case F_PERCENTILE_CONT__FLOAT8_INTERVAL:
        def->cf_type = CF_PARAM_LIST_AGG;
        def->ch_name = "quantiles";
        return true;
    case F_PERCENTILE_DISC_FLOAT8_ANYELEMENT:
        def->ch_name = "quantileExactLow";
        return true;
    case F_PERCENTILE_DISC__FLOAT8_ANYELEMENT:
        def->cf_type = CF_PARAM_LIST_AGG;
        def->ch_name = "quantilesExactLow";
        return true;
    case F_ARRAY_AGG_ANYNONARRAY:
        def->ch_name = "groupArray";
        return true;
    case F_MD5_BYTEA:
    case F_MD5_TEXT:
        def->ch_name     = "lower(hex(MD5";
        def->paren_count = 3;
        return true;
        /* Special hashing function returns lowercase hex. */
    case F_ENCODE:
        def->cf_type = CF_ENCODE;
        def->ch_name = "\1";
        return true;
    case F_REVERSE_TEXT:
        def->ch_name = "reverseUTF8";
        return true;
        /* reverse code points, not bytes */
    case F_LENGTH_TEXT:
        def->ch_name = "lengthUTF8";
        return true;
        /* PG length(text) counts code points, CH length() counts */
        /* bytes */
    case F_OCTET_LENGTH_TEXT:
    case F_OCTET_LENGTH_BPCHAR:
    case F_OCTET_LENGTH_BYTEA:
        def->ch_name = "length";
        return true;
    case F_BIT_AND_INT2:
    case F_BIT_AND_INT4:
    case F_BIT_AND_INT8:
        def->ch_name = "groupBitAnd";
        return true;
    case F_BIT_OR_INT2:
    case F_BIT_OR_INT4:
    case F_BIT_OR_INT8:
        def->ch_name = "groupBitOr";
        return true;
    case F_BIT_XOR_INT2:
    case F_BIT_XOR_INT4:
    case F_BIT_XOR_INT8:
        def->ch_name = "groupBitXor";
        return true;
    case F_BIT_COUNT_BYTEA:
        def->ch_name = "bitCount";
        return true;
    case F_MOD_INT2_INT2:
    case F_MOD_INT4_INT4:
    case F_MOD_INT8_INT8:
    case F_MOD_NUMERIC_NUMERIC:
        def->ch_name = "modulo";
        return true;
    case F_POW_FLOAT8_FLOAT8:
    case F_POWER_FLOAT8_FLOAT8:
        def->ch_name = "pow";
        return true;
        /* CH lacks "power", maps to pow */
    case F_TO_TIMESTAMP_FLOAT8:
        def->cf_type = CF_TO_TIMESTAMP;
        def->ch_name = "\1";
        return true;
    case F_TO_CHAR_TIMESTAMP_TEXT:
    case F_TO_CHAR_TIMESTAMPTZ_TEXT:
        def->cf_type = CF_TO_CHAR;
        def->ch_name = "formatDateTime";
        return true;
    case F_JSONB_EXTRACT_PATH_TEXT:
    case F_JSON_EXTRACT_PATH_TEXT:
        def->cf_type = CF_JSON_EXTRACT_PATH_TEXT;
        def->ch_name = "\1";
        return true;
    case F_JSONB_EXTRACT_PATH:
    case F_JSON_EXTRACT_PATH:
        def->cf_type = CF_JSON_EXTRACT_PATH;
        def->ch_name = "\1";
        return true;
    case F_NOW:
        def->ch_name = "now64";
        return true;
        /* Postgres NOW() produces subsecond precision, to map to */
        /* now64() */
    case F_STATEMENT_TIMESTAMP:
    case F_TRANSACTION_TIMESTAMP:
    case F_CLOCK_TIMESTAMP:
        def->cf_type = CF_CLOCK_TIMESTAMP;
        def->ch_name = "\1";
        return true;
    case F_CURRENT_SCHEMA:
        def->cf_type = CF_CURRENT_SCHEMA;
        def->ch_name = "\1";
        return true;
    case F_CURRENT_DATABASE:
        def->cf_type = CF_CURRENT_DATABASE;
        def->ch_name = "\1";
        return true;
        /* array functions: simple mappings */
    case F_ARRAY_CAT:
        def->ch_name = "arrayConcat";
        return true;
    case F_ARRAY_APPEND:
        def->ch_name = "arrayPushBack";
        return true;
    case F_ARRAY_REMOVE:
        def->ch_name = "arrayRemove";
        return true;
    case F_ARRAY_TO_STRING_ANYARRAY_TEXT:
        def->ch_name = "arrayStringConcat";
        return true;
    case F_ARRAY_LENGTH:
        def->cf_type = CF_ARRAY_LENGTH;
        def->ch_name = "\1";
        return true;
    case F_CARDINALITY:
        def->ch_name = "length";
        return true;
    case F_ARRAY_REVERSE:
        def->ch_name = "arrayReverse";
        return true;
    case F_ARRAY_SORT_ANYARRAY:
        def->ch_name = "arraySort";
        return true;
    case F_ARRAY_SHUFFLE:
        def->ch_name = "arrayShuffle";
        return true;
    case F_ARRAY_SAMPLE:
        def->ch_name = "arrayRandomSample";
        return true;
    case F_ARRAY_PREPEND:
        def->cf_type = CF_ARRAY_PREPEND;
        def->ch_name = "arrayPushFront";
        return true;
    case F_STRING_TO_ARRAY_TEXT_TEXT:
        def->cf_type = CF_STRING_TO_ARRAY;
        def->ch_name = "splitByString";
        return true;
    case F_SPLIT_PART:
        def->cf_type = CF_STRING_TO_ARRAY_PART;
        def->ch_name = "splitByString";
        return true;
    case F_TRIM_ARRAY:
        def->cf_type = CF_TRIM_ARRAY;
        def->ch_name = "arrayResize";
        return true;
    case F_ARRAY_FILL_ANYELEMENT__INT4:
        def->cf_type = CF_ARRAY_FILL;
        def->ch_name = "arrayWithConstant";
        return true;
    case F_ARRAY_SORT_ANYARRAY_BOOL:
        def->cf_type = CF_ARRAY_SORT_DESC;
        def->ch_name = "\1";
        return true;

    case F_STDDEV_INT2:
    case F_STDDEV_INT4:
    case F_STDDEV_INT8:
    case F_STDDEV_FLOAT4:
    case F_STDDEV_FLOAT8:
    case F_STDDEV_NUMERIC:
        def->ch_name = "stddev_samp";
        return true;

    case F_VARIANCE_INT2:
    case F_VARIANCE_INT4:
    case F_VARIANCE_INT8:
    case F_VARIANCE_FLOAT4:
    case F_VARIANCE_FLOAT8:
    case F_VARIANCE_NUMERIC:
        def->ch_name = "var_samp";
        return true;

#if PG_VERSION_NUM >= 160000
    case F_ANY_VALUE:
        def->ch_name = "any";
        return true;
#endif

        /* 1:1 pass-through: PG and CH agree on name and semantics */
    case F_ARRAY_AGG_ANYARRAY:
    case F_AVG_INT8:
    case F_AVG_INT4:
    case F_AVG_INT2:
    case F_AVG_NUMERIC:
    case F_AVG_FLOAT4:
    case F_AVG_FLOAT8:
    case F_AVG_INTERVAL:
    case F_SUM_INT8:
    case F_SUM_INT4:
    case F_SUM_INT2:
    case F_SUM_FLOAT4:
    case F_SUM_FLOAT8:
    case F_SUM_INTERVAL:
    case F_SUM_NUMERIC:
    case F_COUNT_ANY:
    case F_COUNT_:
    case F_MIN_INT8:
    case F_MIN_INT4:
    case F_MIN_INT2:
    case F_MIN_FLOAT4:
    case F_MIN_FLOAT8:
    case F_MIN_DATE:
    case F_MIN_TIMESTAMP:
    case F_MIN_TIMESTAMPTZ:
    case F_MIN_INTERVAL:
    case F_MIN_TEXT:
    case F_MIN_NUMERIC:
    case F_MIN_BPCHAR:
    case F_MAX_INT8:
    case F_MAX_INT4:
    case F_MAX_INT2:
    case F_MAX_FLOAT4:
    case F_MAX_FLOAT8:
    case F_MAX_DATE:
    case F_MAX_TIMESTAMP:
    case F_MAX_TIMESTAMPTZ:
    case F_MAX_INTERVAL:
    case F_MAX_TEXT:
    case F_MAX_NUMERIC:
    case F_MAX_BPCHAR:
    case F_BOOL_AND:
    case F_BOOL_OR:
    case F_EVERY:
    case F_STRING_AGG_TEXT_TEXT:
    case F_CORR:
    case F_COVAR_POP:
    case F_COVAR_SAMP:
    case F_STDDEV_SAMP_INT2:
    case F_STDDEV_SAMP_INT4:
    case F_STDDEV_SAMP_INT8:
    case F_STDDEV_SAMP_FLOAT4:
    case F_STDDEV_SAMP_FLOAT8:
    case F_STDDEV_SAMP_NUMERIC:
    case F_STDDEV_POP_INT2:
    case F_STDDEV_POP_INT4:
    case F_STDDEV_POP_INT8:
    case F_STDDEV_POP_FLOAT4:
    case F_STDDEV_POP_FLOAT8:
    case F_STDDEV_POP_NUMERIC:
    case F_VAR_POP_INT2:
    case F_VAR_POP_INT4:
    case F_VAR_POP_INT8:
    case F_VAR_POP_FLOAT4:
    case F_VAR_POP_FLOAT8:
    case F_VAR_POP_NUMERIC:
    case F_VAR_SAMP_INT2:
    case F_VAR_SAMP_INT4:
    case F_VAR_SAMP_INT8:
    case F_VAR_SAMP_FLOAT4:
    case F_VAR_SAMP_FLOAT8:
    case F_VAR_SAMP_NUMERIC:

    /* window functions sharing PG and CH names */
    case F_ROW_NUMBER:
    case F_RANK_:
    case F_DENSE_RANK_:
    case F_PERCENT_RANK_:
    case F_CUME_DIST_:
    case F_NTILE:
        /* trig: PG and CH agree on all finite inputs. Skipping */
        /* asin/acos/atanh/acosh because PG errors on out-of-range */
        /* input where CH returns NaN; sin/cos/tan share the same */
        /* error-vs-NaN divergence only at infinity. */
    case F_SIN:
    case F_COS:
    case F_TAN:
    case F_ATAN:
    case F_ATAN2:
    case F_SINH:
    case F_COSH:
    case F_TANH:
    case F_ASINH:
    case F_DEGREES:
    case F_RADIANS:
    case F_PI:
    case F_REVERSE_BYTEA:
        /* numeric scalar functions, names match ClickHouse */
    case F_ABS_INT2:
    case F_ABS_INT4:
    case F_ABS_INT8:
    case F_ABS_FLOAT4:
    case F_ABS_FLOAT8:
    case F_ABS_NUMERIC:
    case F_ROUND_FLOAT8:
    case F_ROUND_NUMERIC:
    case F_ROUND_NUMERIC_INT4:
    case F_FACTORIAL:
        /* string functions: CH ltrim/rtrim/concat_ws are aliases */
    case F_LTRIM_TEXT:
    case F_LTRIM_TEXT_TEXT:
    case F_RTRIM_TEXT:
    case F_RTRIM_TEXT_TEXT:
    case F_CONCAT_WS:
    case F_LENGTH_BYTEA:
        /* date(ts), date(tstz) deparse as CH date() (alias toDate) */
    case F_DATE_TIMESTAMP:
    case F_DATE_TIMESTAMPTZ:
        /* window functions: lead/lag share PG and CH names */
    case F_LEAD_ANYELEMENT:
    case F_LEAD_ANYELEMENT_INT4:
    case F_LEAD_ANYCOMPATIBLE_INT4_ANYCOMPATIBLE:
    case F_LAG_ANYELEMENT:
    case F_LAG_ANYELEMENT_INT4:
    case F_LAG_ANYCOMPATIBLE_INT4_ANYCOMPATIBLE:
        return true;
    default:
        return false;
    }
}

CustomObjectDef*
chfdw_check_for_custom_function(Oid funcid) {
    CustomObjectDef* entry;
    builtin_func_def def = { .paren_count = 1 };
    bool is_builtin      = chfdw_is_builtin(funcid);

    if (is_builtin && !lookup_builtin_func(funcid, &def)) {
        return NULL;
    }

    if (!custom_objects_cache) {
        custom_objects_cache = create_custom_objects_cache();
    }

    entry = hash_search(custom_objects_cache, (void*)&funcid, HASH_FIND, NULL);
    if (!entry) {
        Oid extoid;
        char* extname;

        entry = hash_search(custom_objects_cache, (void*)&funcid, HASH_ENTER, NULL);
        entry->cf_oid = funcid;
        init_custom_entry(entry);

        if (is_builtin) {
            entry->cf_type     = def.cf_type;
            entry->paren_count = def.paren_count;
            if (def.ch_name) {
                strlcpy(entry->custom_name, def.ch_name, NAMEDATALEN);
            }
            return entry;
        }

        extoid  = getExtensionOfObject(ProcedureRelationId, funcid);
        extname = get_extension_name(extoid);
        if (extname) {
            HeapTuple proctup;
            Form_pg_proc procform;
            char* proname;

            proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
            if (!HeapTupleIsValid(proctup)) {
                elog(ERROR, "cache lookup failed for function %u", funcid);
            }

            procform = (Form_pg_proc)GETSTRUCT(proctup);
            proname  = NameStr(procform->proname);

            if (STR_EQUAL(extname, "intarray")) {
                if (STR_EQUAL(proname, "idx")) {
                    entry->cf_type = CF_INTARRAY_IDX;
                    strcpy(entry->custom_name, "indexOf");
                }
            } else if (STR_EQUAL(extname, "re2")) {
                /* pg_re2: 1:1 pushdown to ClickHouse RE2 functions. */
                entry->cf_type = CF_CH_FUNCTION;
                strlcpy(entry->custom_name, re2_func_name(proname), NAMEDATALEN);
            } else if (STR_EQUAL(extname, "fuzzystrmatch")) {
                if (STR_EQUAL(proname, "levenshtein") && procform->pronargs == 2) {
                    strcpy(entry->custom_name, "editDistanceUTF8");
                } else if (!(STR_EQUAL(proname, "soundex"))) {
                    ReleaseSysCache(proctup);
                    pfree(extname);
                    hash_search(
                        custom_objects_cache, (void*)&funcid, HASH_REMOVE, NULL
                    );
                    return NULL;
                }
            } else if (STR_EQUAL(extname, "pg_clickhouse")) {
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

CustomObjectDef*
chfdw_check_for_custom_type(Oid typeoid) {
    CustomObjectDef* entry;

    if (!custom_objects_cache) {
        custom_objects_cache = create_custom_objects_cache();
    }

    if (chfdw_is_builtin(typeoid)) {
        return NULL;
    }

    entry = hash_search(custom_objects_cache, (void*)&typeoid, HASH_FIND, NULL);
    if (!entry) {
        entry = hash_search(custom_objects_cache, (void*)&typeoid, HASH_ENTER, NULL);
        init_custom_entry(entry);
    }

    return entry;
}

/* pg_operator_d.h only has oid_symbol for some operators */
#define OID_TEXT_REGEX_NE_OP 642
#define OID_TEXT_IREGEX_NE_OP 1229
#define OID_JSONB_FETCHVAL_OP 3211
#define OID_JSONB_FETCHVAL_TEXT_OP 3477
#define OID_JSON_FETCHVAL_OP 3962
#define OID_JSON_FETCHVAL_TEXT_OP 3963
#define OID_DATE_PL_INTERVAL_OP 1076
#define OID_DATE_MI_INTERVAL_OP 1077
#define OID_TIMESTAMPTZ_PL_INTERVAL_OP 1327
#define OID_TIMESTAMPTZ_MI_INTERVAL_OP 1329
#define OID_TIMESTAMP_PL_INTERVAL_OP 2066
#define OID_TIMESTAMP_MI_INTERVAL_OP 2068

/*
 * Map a builtin operator OID to its custom_object_type. Returns CF_USUAL
 * when the operator needs no special handling.
 */
static custom_object_type
classify_builtin_operator(Oid opoid) {
    switch (opoid) {
    case OID_TEXT_REGEXEQ_OP:
        return CF_REGEX_MATCH;
    case OID_TEXT_REGEX_NE_OP:
        return CF_REGEX_NO_MATCH;
    case OID_TEXT_ICREGEXEQ_OP:
        return CF_REGEX_ICASE_MATCH;
    case OID_TEXT_IREGEX_NE_OP:
        return CF_REGEX_ICASE_NO_MATCH;
    case OID_JSONB_FETCHVAL_OP:
    case OID_JSON_FETCHVAL_OP:
        return CF_JSON_FETCHVAL;
    case OID_JSONB_FETCHVAL_TEXT_OP:
    case OID_JSON_FETCHVAL_TEXT_OP:
        return CF_JSON_FETCHVAL_TEXT;
    case OID_ARRAY_CONTAINS_OP:
        return CF_ARRAY_CONTAINS;
    case OID_ARRAY_CONTAINED_OP:
        return CF_ARRAY_CONTAINED_BY;
    case OID_ARRAY_OVERLAP_OP:
        return CF_ARRAY_OVERLAP;
    case OID_DATE_PL_INTERVAL_OP:
    case OID_TIMESTAMPTZ_PL_INTERVAL_OP:
    case OID_TIMESTAMP_PL_INTERVAL_OP:
        return CF_DATETIME_PL_INTERVAL;
    case OID_DATE_MI_INTERVAL_OP:
    case OID_TIMESTAMPTZ_MI_INTERVAL_OP:
    case OID_TIMESTAMP_MI_INTERVAL_OP:
        return CF_DATETIME_MI_INTERVAL;
    default:
        return CF_USUAL;
    }
}

CustomObjectDef*
chfdw_check_for_custom_operator(Oid opoid, Form_pg_operator form) {
    HeapTuple tuple = NULL;
    custom_object_type ctype;

    CustomObjectDef* entry;

    if (!custom_objects_cache) {
        custom_objects_cache = create_custom_objects_cache();
    }

    if (chfdw_is_builtin(opoid)) {
        ctype = classify_builtin_operator(opoid);
        if (ctype == CF_USUAL) {
            return NULL;
        }
    }

    if (!form) {
        tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(opoid));
        if (!HeapTupleIsValid(tuple)) {
            elog(ERROR, "cache lookup failed for operator %u", opoid);
        }
        form = (Form_pg_operator)GETSTRUCT(tuple);
    }

    entry = hash_search(custom_objects_cache, (void*)&opoid, HASH_FIND, NULL);
    if (!entry) {
        entry = hash_search(custom_objects_cache, (void*)&opoid, HASH_ENTER, NULL);
        init_custom_entry(entry);

        ctype = classify_builtin_operator(opoid);
        if (ctype != CF_USUAL) {
            entry->cf_type = ctype;
        } else {
            Oid extoid    = getExtensionOfObject(OperatorRelationId, opoid);
            char* extname = get_extension_name(extoid);

            if (extname) {
                if (STR_EQUAL(extname, "hstore")) {
                    if (form && strcmp(NameStr(form->oprname), "->") == 0) {
                        entry->cf_type = CF_HSTORE_FETCHVAL;
                    }
                } else if (STR_EQUAL(extname, "re2")) {
                    if (form && strcmp(NameStr(form->oprname), "@~") == 0) {
                        entry->cf_type = CF_RE2_MATCH;
                    }
                }
                pfree(extname);
            }
        }
    }

    if (tuple) {
        ReleaseSysCache(tuple);
    }

    return entry;
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
static void
populate_custom_column_entry(
    CustomColumnInfo* entry,
    Oid relid,
    AttrNumber attnum,
    const char* attname,
    CHRemoteTableEngine table_engine
);

void
chfdw_apply_custom_table_options(CHFdwRelationInfo* fpinfo, Oid relid) {
    ListCell* lc;
    TupleDesc tupdesc;
    int attnum;
    Relation rel;

    foreach (lc, fpinfo->table->options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (STR_EQUAL(def->defname, "engine")) {
            static char *collapsing_text  = "collapsingmergetree",
                        *aggregating_text = "aggregatingmergetree";

            char* val = defGetString(def);

            if (strncasecmp(val, collapsing_text, strlen(collapsing_text)) == 0) {
                char *start = index(val, '('), *end = rindex(val, ')');
                char sign[CH_ESCAPED_NAMEDATALEN];
                Size sign_len;

                if (start == NULL || end == NULL || end <= start + 1) {
                    ereport(
                        ERROR,
                        errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                        errmsg("pg_clickhouse: invalid engine parameter")
                    );
                }

                sign_len = end - start - 1;
                if (sign_len > CH_ESCAPED_NAMEDATALEN - 1) {
                    ereport(
                        ERROR,
                        errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
                        errmsg("pg_clickhouse: invalid engine parameter")
                    );
                }

                fpinfo->ch_table_engine = CH_COLLAPSING_MERGE_TREE;
                memcpy(sign, start + 1, sign_len);
                sign[sign_len] = '\0';
                strlcpy(
                    fpinfo->ch_table_sign_field,
                    ch_quote_ident(sign),
                    CH_ESCAPED_NAMEDATALEN
                );
            } else if (
                strncasecmp(val, aggregating_text, strlen(aggregating_text)) == 0
            ) {
                fpinfo->ch_table_engine = CH_AGGREGATING_MERGE_TREE;
            }
        }
    }

    if (custom_columns_cache == NULL) {
        custom_columns_cache = create_custom_columns_cache();
    }

    rel     = table_open(relid, NoLock);
    tupdesc = RelationGetDescr(rel);

    for (attnum = 1; attnum <= tupdesc->natts; attnum++) {
        bool found;
        CustomColumnInfo entry_key, *entry;
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

        entry_key.relid    = relid;
        entry_key.varattno = attnum;

        entry = hash_search(
            custom_columns_cache, (void*)&entry_key.relid, HASH_ENTER, &found
        );
        if (found) {
            continue;
        }

        populate_custom_column_entry(
            entry, relid, attnum, NameStr(attr->attname), fpinfo->ch_table_engine
        );
    }
    table_close(rel, NoLock);
}

/*
 * Populate one cache entry from pg_attribute + FDW column options.
 * Caller has already inserted the entry under (relid, attnum).
 */
static void
populate_custom_column_entry(
    CustomColumnInfo* entry,
    Oid relid,
    AttrNumber attnum,
    const char* attname,
    CHRemoteTableEngine table_engine
) {
    List* options;
    ListCell* lc;

    entry->relid                = relid;
    entry->varattno             = attnum;
    entry->table_engine         = table_engine;
    entry->coltype              = CF_USUAL;
    entry->is_AggregateFunction = CF_AGGR_USUAL;
    strlcpy(entry->colname, attname, NAMEDATALEN);

    /* column_name FDW option overrides attname */
    options = GetForeignColumnOptions(relid, attnum);
    foreach (lc, options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (STR_EQUAL(def->defname, "column_name")) {
            strncpy(entry->colname, defGetString(def), NAMEDATALEN);
            entry->colname[NAMEDATALEN - 1] = '\0';
        } else if (STR_EQUAL(def->defname, "aggregatefunction")) {
            entry->is_AggregateFunction = CF_AGGR_FUNC;
        } else if (STR_EQUAL(def->defname, "simpleaggregatefunction")) {
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
CustomColumnInfo*
chfdw_get_custom_column_info(Oid relid, uint16 varattno) {
    CustomColumnInfo entry_key, *entry;
    bool found;

    entry_key.relid    = relid;
    entry_key.varattno = varattno;

    if (custom_columns_cache == NULL) {
        custom_columns_cache = create_custom_columns_cache();
    }

    entry =
        hash_search(custom_columns_cache, (void*)&entry_key.relid, HASH_ENTER, &found);
    if (!found) {
        Relation rel;
        TupleDesc tupdesc;

        rel     = table_open(relid, NoLock);
        tupdesc = RelationGetDescr(rel);

        if (varattno > 0 && varattno <= tupdesc->natts) {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, varattno - 1);

            if (!attr->attisdropped) {
                populate_custom_column_entry(
                    entry, relid, varattno, NameStr(attr->attname), CH_DEFAULT
                );
            } else {
                hash_search(
                    custom_columns_cache, (void*)&entry_key.relid, HASH_REMOVE, NULL
                );
                entry = NULL;
            }
        } else {
            hash_search(
                custom_columns_cache, (void*)&entry_key.relid, HASH_REMOVE, NULL
            );
            entry = NULL;
        }
        table_close(rel, NoLock);
    }

    return entry;
}
