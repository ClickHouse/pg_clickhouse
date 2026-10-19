#include "postgres.h"

#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/dependency.h"
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
static bool custom_objects_stale  = false;

/*
 * Note a change to a function, type, or operator, so that the transaction's
 * end drops the cached definitions. Dropping them now could free one in use.
 */
static void
invalidate_custom_objects_cache(Datum arg, int cacheid, uint32 hashvalue) {
    custom_objects_stale = true;
}

/*
 * Drop the cached definitions after a change to the objects they describe.
 * Without it, each DROP and CREATE of an extension left entries for objects
 * that no longer exist, and a reused OID could find another object's entry.
 */
static void
drop_stale_custom_objects(XactEvent event, void* arg) {
    if (custom_objects_stale && custom_objects_cache &&
        (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT)) {
        hash_destroy(custom_objects_cache);
        custom_objects_cache = NULL;
        custom_objects_stale = false;
    }
}

static HTAB*
create_custom_objects_cache(void) {
    static bool registered = false;
    HASHCTL ctl;

    if (!registered) {
        CacheRegisterSyscacheCallback(
            PROCOID, invalidate_custom_objects_cache, (Datum)0
        );
        CacheRegisterSyscacheCallback(
            TYPEOID, invalidate_custom_objects_cache, (Datum)0
        );
        CacheRegisterSyscacheCallback(
            OPEROID, invalidate_custom_objects_cache, (Datum)0
        );
        RegisterXactCallback(drop_stale_custom_objects, NULL);
        registered = true;
    }
    custom_objects_stale = false;
    ctl.keysize          = sizeof(Oid);
    ctl.entrysize        = sizeof(CustomObjectDef);

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
        if (entry->colname) {
            pfree(entry->colname);
        }
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
 * names. Must be kept in lexicographic order and in sync with functions we
 * define to be pushed down.
 */
static char* ch_func_map[][2] = {
    { "argmax",                   "argMax"                   },
    { "argmin",                   "argMin"                   },
    { "cosinedistancetransposed", "cosineDistanceTransposed" },
    { "dictget",                  "dictGet"                  },
    { "dotproducttransposed",     "dotProductTransposed"     },
    { "l2distancetransposed",     "L2DistanceTransposed"     },
    { "quantile",                 "quantile"                 },
    { "quantileexact",            "quantileExact"            },
    { "touint128",                "toUInt128"                },
    { "touint16",                 "toUInt16"                 },
    { "touint32",                 "toUInt32"                 },
    { "touint64",                 "toUInt64"                 },
    { "touint8",                  "toUInt8"                  },
    { "uniq",                     "uniq"                     },
    { "uniqcombined",             "uniqCombined"             },
    { "uniqcombined64",           "uniqCombined64"           },
    { "uniqexact",                "uniqExact"                },
    { "uniqhll12",                "uniqHLL12"                },
    { "uniqtheta",                "uniqTheta"                },
    { NULL,                       NULL                       },
};

/*
 * Returns the canonical, case-sensitive name of proname if it's is one of the
 * custom functions pg_clickhouse provides to push down to ClickHouse.
 * Otherwise return NULL.
 */
inline static char*
ch_func_name(char* proname) {
    size_t i = 0;

    while (ch_func_map[i][0] != NULL) {
        if (STR_EQUAL(ch_func_map[i][0], proname)) {
            return ch_func_map[i][1];
        }
        i++;
    }
    return NULL;
}

/* Map pgcrypto digest algorithms to ClickHouse function names. */
static const char* const digest_func_map[][2] = {
    { "md5",    "MD5"    },
    { "sha1",   "SHA1"   },
    { "sha224", "SHA224" },
    { "sha256", "SHA256" },
    { "sha384", "SHA384" },
    { "sha512", "SHA512" },
    { NULL,     NULL     },
};

const char*
chfdw_digest_func_name(const char* algorithm) {
    for (size_t i = 0; digest_func_map[i][0] != NULL; i++) {
        if (pg_strcasecmp(algorithm, digest_func_map[i][0]) == 0) {
            return digest_func_map[i][1];
        }
    }

    return NULL;
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
        def->ch_name = "toDateTime64";
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
    case F_SHA224:
        def->ch_name = "SHA224";
        return true;
    case F_SHA256:
        def->ch_name = "SHA256";
        return true;
    case F_SHA384:
        def->ch_name = "SHA384";
        return true;
    case F_SHA512:
        def->ch_name = "SHA512";
        return true;
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

        /* 1:1 pass-through: PG and CH 26.9+ agree on name and semantics */
    case F_REGR_AVGX:
    case F_REGR_AVGY:
    case F_REGR_COUNT:
    case F_REGR_INTERCEPT:
    case F_REGR_R2:
    case F_REGR_SLOPE:
    case F_REGR_SXX:
    case F_REGR_SXY:
    case F_REGR_SYY:
        def->cf_type = CF_REGR_AGG;
        return true;

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
        /* date(ts), date(tstz) deparse as toDate32 in TimeZone */
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

/*
 * Map a pgvector function on vector arguments to its ClickHouse function.
 * Returns false for functions without an equivalent, and for the halfvec and
 * sparsevec functions that share their names.
 */
static bool
pgvector_function(Form_pg_proc procform, CustomObjectDef* entry) {
    const char* proname = NameStr(procform->proname);
    const char* ch_name = NULL;

    for (int i = 0; i < procform->pronargs; i++) {
        if (!chfdw_is_pgvector_type(procform->proargtypes.values[i])) {
            return false;
        }
    }

    entry->cf_type = CF_CH_FUNCTION;
    if (procform->pronargs == 2) {
        if (STR_EQUAL(proname, "l2_distance")) {
            ch_name = "L2Distance";
        } else if (STR_EQUAL(proname, "cosine_distance")) {
            ch_name = "cosineDistance";
        } else if (STR_EQUAL(proname, "l1_distance")) {
            ch_name = "L1Distance";
        } else if (STR_EQUAL(proname, "inner_product")) {
            /* Requires a ClickHouse version whose dotProduct takes arrays */
            entry->cf_type = CF_VECTOR_DOT;
            ch_name        = "dotProduct";
        }
    } else if (procform->pronargs == 1 && STR_EQUAL(proname, "vector_dims")) {
        ch_name = "length";
    }

    if (!ch_name) {
        return false;
    }
    strlcpy(entry->custom_name, ch_name, NAMEDATALEN);
    return true;
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

            if (STR_EQUAL(extname, "intarray") && STR_EQUAL(proname, "idx")) {
                entry->cf_type = CF_INTARRAY_IDX;
                strcpy(entry->custom_name, "indexOf");
            } else if (STR_EQUAL(extname, "re2")) {
                /* pg_re2: 1:1 pushdown to ClickHouse RE2 functions. */
                entry->cf_type = CF_CH_FUNCTION;
                strlcpy(entry->custom_name, re2_func_name(proname), NAMEDATALEN);
            } else if (STR_EQUAL(extname, "vector")) {
                if (!pgvector_function(procform, entry)) {
                    ReleaseSysCache(proctup);
                    pfree(extname);
                    hash_search(
                        custom_objects_cache, (void*)&funcid, HASH_REMOVE, NULL
                    );
                    return NULL;
                }
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
            } else if (
                STR_EQUAL(extname, "pgcrypto") && STR_EQUAL(proname, "digest") &&
                procform->pronargs == 2 &&
                (procform->proargtypes.values[0] == TEXTOID ||
                 procform->proargtypes.values[0] == BYTEAOID) &&
                procform->proargtypes.values[1] == TEXTOID &&
                procform->prorettype == BYTEAOID
            ) {
                entry->cf_type = CF_DIGEST;
                strcpy(entry->custom_name, "\1");
            } else if (
                STR_EQUAL(extname, "pg_clickhouse") && (proname = ch_func_name(proname))
            ) {
                /* pg_clickhouse custom functions. */
                entry->cf_type = CF_CH_FUNCTION;
                strlcpy(entry->custom_name, proname, NAMEDATALEN);
            } else {
                /* Unknown extension & function. */
                ReleaseSysCache(proctup);
                pfree(extname);
                hash_search(custom_objects_cache, (void*)&funcid, HASH_REMOVE, NULL);
                return NULL;
            }
            ReleaseSysCache(proctup);
            pfree(extname);
        } else {
            /* We current do not support custom extension functions. */
            hash_search(custom_objects_cache, (void*)&funcid, HASH_REMOVE, NULL);
            return NULL;
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
        /* Use elemoid as only PG17 began recording array types as extension members */
        Oid elemoid = get_element_type(typeoid);
        Oid extoid  = getExtensionOfObject(
            TypeRelationId, OidIsValid(elemoid) ? elemoid : typeoid
        );
        char* extname           = get_extension_name(extoid);
        custom_object_type type = CF_USUAL;

        if (extname) {
            bool known = STR_EQUAL(extname, "hstore");

            if (pgch_is_pgvector_type(typeoid)) {
                known = true;
                type  = CF_PGVECTOR;
            }

            pfree(extname);
            if (!known) {
                return NULL;
            }
        }

        entry = hash_search(custom_objects_cache, (void*)&typeoid, HASH_ENTER, NULL);
        init_custom_entry(entry);
        entry->cf_type = type;
    }

    return entry;
}

/*
 * Map a builtin operator OID to its custom_object_type, and set ch_name for
 * operators deparsed as ClickHouse functions. Returns CF_USUAL when the
 * operator needs no special handling.
 */
static custom_object_type
classify_builtin_operator(Oid opoid, const char** ch_name) {
    switch (get_opcode(opoid)) {
    case F_TEXTREGEXEQ:
        return CF_REGEX_MATCH;
    case F_TEXTREGEXNE:
        return CF_REGEX_NO_MATCH;
    case F_TEXTICREGEXEQ:
        return CF_REGEX_ICASE_MATCH;
    case F_TEXTICREGEXNE:
        return CF_REGEX_ICASE_NO_MATCH;
    case F_JSONB_OBJECT_FIELD:
    case F_JSON_OBJECT_FIELD:
        return CF_JSON_FETCHVAL;
    case F_JSONB_OBJECT_FIELD_TEXT:
    case F_JSON_OBJECT_FIELD_TEXT:
        return CF_JSON_FETCHVAL_TEXT;
    case F_ARRAYCONTAINS:
        *ch_name = "hasAll";
        break;
    case F_ARRAYCONTAINED:
        return CF_ARRAY_CONTAINED_BY;
    case F_ARRAYOVERLAP:
        *ch_name = "hasAny";
        break;
    case F_DATE_PL_INTERVAL:
    case F_TIMESTAMPTZ_PL_INTERVAL:
    case F_TIMESTAMP_PL_INTERVAL:
        return CF_DATETIME_PL_INTERVAL;
    case F_DATE_MI_INTERVAL:
    case F_TIMESTAMPTZ_MI_INTERVAL:
    case F_TIMESTAMP_MI_INTERVAL:
        return CF_DATETIME_MI_INTERVAL;
    case F_DPOW:
        *ch_name = "pow";
        break;
    case F_DSQRT:
        *ch_name = "sqrt";
        break;
    case F_DCBRT:
        *ch_name = "cbrt";
        break;
    case F_INT2ABS:
    case F_INT4ABS:
    case F_INT8ABS:
    case F_FLOAT4ABS:
    case F_FLOAT8ABS:
    case F_NUMERIC_ABS:
        *ch_name = "abs";
        break;
    case F_INT2DIV:
    case F_INT4DIV:
    case F_INT8DIV:
    case F_INT24DIV:
    case F_INT42DIV:
    case F_INT28DIV:
    case F_INT82DIV:
    case F_INT48DIV:
    case F_INT84DIV:
        *ch_name = "intDiv";
        break;
    case F_INT2AND:
    case F_INT4AND:
    case F_INT8AND:
        *ch_name = "bitAnd";
        break;
    case F_INT2OR:
    case F_INT4OR:
    case F_INT8OR:
        *ch_name = "bitOr";
        break;
    case F_INT2XOR:
    case F_INT4XOR:
    case F_INT8XOR:
        *ch_name = "bitXor";
        break;
    case F_INT2NOT:
    case F_INT4NOT:
    case F_INT8NOT:
        *ch_name = "bitNot";
        break;
    case F_INT2SHL:
    case F_INT4SHL:
    case F_INT8SHL:
        *ch_name = "bitShiftLeft";
        break;
    case F_INT2SHR:
    case F_INT4SHR:
    case F_INT8SHR:
        *ch_name = "bitShiftRight";
        break;
    default:
        return CF_USUAL;
    }
    return CF_OPERATOR_FUNC;
}

/*
 * Map a pgvector operator name to its custom_object_type. Returns false for
 * operators without a ClickHouse equivalent.
 */
static bool
pgvector_operator_type(
    const char* oprname,
    custom_object_type* ctype,
    const char** ch_name
) {
    if (STR_EQUAL(oprname, "=") || STR_EQUAL(oprname, "<>")) {
        /* ClickHouse compares Array(Float32) values element-wise. */
        *ctype = CF_USUAL;
        return true;
    }
    if (STR_EQUAL(oprname, "<#>")) {
        *ctype = CF_VECTOR_NEG_IP;
        return true;
    }
    if (STR_EQUAL(oprname, "<->")) {
        *ch_name = "L2Distance";
    } else if (STR_EQUAL(oprname, "<=>")) {
        *ch_name = "cosineDistance";
    } else if (STR_EQUAL(oprname, "<+>")) {
        *ch_name = "L1Distance";
    } else {
        return false;
    }
    *ctype = CF_OPERATOR_FUNC;
    return true;
}

CustomObjectDef*
chfdw_check_for_custom_operator(Oid opoid, Form_pg_operator form) {
    HeapTuple tuple          = NULL;
    const char* ch_name      = NULL;
    custom_object_type ctype = CF_USUAL;

    CustomObjectDef* entry;

    if (!custom_objects_cache) {
        custom_objects_cache = create_custom_objects_cache();
    }

    if (chfdw_is_builtin(opoid)) {
        ctype = classify_builtin_operator(opoid, &ch_name);
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

        if (ctype != CF_USUAL) {
            entry->cf_type = ctype;
        } else {
            Oid extoid          = getExtensionOfObject(OperatorRelationId, opoid);
            char* extname       = get_extension_name(extoid);
            const char* oprname = NameStr(form->oprname);

            if (extname && STR_EQUAL(extname, "hstore") && STR_EQUAL(oprname, "->")) {
                entry->cf_type = CF_HSTORE_FETCHVAL;
            } else if (
                extname && STR_EQUAL(extname, "re2") && STR_EQUAL(oprname, "@~")
            ) {
                entry->cf_type = CF_OPERATOR_FUNC;
                ch_name        = "match";
            } else if (
                extname && STR_EQUAL(extname, "vector") &&
                pgvector_operator_type(oprname, &ctype, &ch_name) &&
                chfdw_is_pgvector_type(form->oprleft) &&
                chfdw_is_pgvector_type(form->oprright)
            ) {
                entry->cf_type = ctype;
            } else {
                /* Unknown or user defined operator */
                hash_search(custom_objects_cache, (void*)&opoid, HASH_REMOVE, NULL);
                entry = NULL;
            }
            if (extname) {
                pfree(extname);
            }
        }
        if (entry && ch_name) {
            strlcpy(entry->custom_name, ch_name, NAMEDATALEN);
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

        entry_key.relid    = relid;
        entry_key.varattno = attnum;

        entry = hash_search(
            custom_columns_cache, (void*)&entry_key.relid, HASH_ENTER, &found
        );
        if (found) {
            continue;
        }

        populate_custom_column_entry(entry, relid, attnum, fpinfo->ch_table_engine);
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
    CHRemoteTableEngine table_engine
) {
    List* options;
    ListCell* lc;

    entry->relid                = relid;
    entry->varattno             = attnum;
    entry->table_engine         = table_engine;
    entry->coltype              = CF_USUAL;
    entry->is_AggregateFunction = CF_AGGR_USUAL;
    entry->colname              = NULL;

    options = GetForeignColumnOptions(relid, attnum);
    foreach (lc, options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (STR_EQUAL(def->defname, "column_name")) {
            entry->colname = MemoryContextStrdup(CacheMemoryContext, defGetString(def));
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
                populate_custom_column_entry(entry, relid, varattno, CH_DEFAULT);
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
