/*-------------------------------------------------------------------------
 *
 * vector_meta.c
 *	  Cache of the ClickHouse vector similarity indexes on foreign tables.
 *
 * ClickHouse is the source of truth for vector indexes, so the planner reads
 * them from system.data_skipping_indices when it plans a vector search and
 * caches the result per backend for pg_clickhouse.vector_metadata_ttl
 * seconds. Nothing is stored in Postgres. The metadata drives only warnings
 * and EXPLAIN output, never query results, so a failed lookup is logged and
 * ignored.
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/vector_meta.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "foreign/foreign.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"

#include "cursor.h"
#include "fdw.h"

typedef struct VectorMetaKey {
    Oid relid;
    Oid userid;
} VectorMetaKey;

typedef struct VectorMetaEntry {
    VectorMetaKey key;
    VectorTableMeta meta;
    TimestampTz fetched_at;
    MemoryContext cxt; /* holds meta */
} VectorMetaEntry;

static HTAB* vector_meta_cache = NULL;
static MemoryContext meta_cxt  = NULL;

PG_FUNCTION_INFO_V1(clickhouse_refresh_metadata);
PG_FUNCTION_INFO_V1(clickhouse_vector_indexes);

/*
 * Drop every cached entry. Called after pg_clickhouse changes an index and by
 * clickhouse_refresh_metadata().
 */
void
chfdw_vector_meta_invalidate_all(void) {
    if (meta_cxt) {
        MemoryContextDelete(meta_cxt);
    }
    meta_cxt          = NULL;
    vector_meta_cache = NULL;
}

/* SQL: clickhouse_refresh_metadata() */
Datum
clickhouse_refresh_metadata(PG_FUNCTION_ARGS pg_attribute_unused()) {
    chfdw_vector_meta_invalidate_all();
    PG_RETURN_VOID();
}

/*
 * Run sql on the server of user and return its rows as a List of char*[]
 * arrays of ncols text values each, NULL for NULL values.
 */
List*
chfdw_run_text_query(UserMapping* user, const char* sql, int ncols) {
    TupleDesc tupdesc     = CreateTemplateTupleDesc(ncols);
    List* retrieved_attrs = NIL;
    List* rows            = NIL;

    for (int i = 1; i <= ncols; i++) {
        TupleDescInitEntry(tupdesc, (AttrNumber)i, "", TEXTOID, -1, 0);
        retrieved_attrs = lappend_int(retrieved_attrs, i);
    }
#if PG_VERSION_NUM >= 190000
    TupleDescFinalize(tupdesc);
#endif

    AttInMetadata* attinmeta = TupleDescGetAttInMetadata(tupdesc);
    ch_scan_connection sconn = chfdw_get_scan_connection(user);
    ch_query query =
        new_query(sql, 0, NULL, tupdesc, retrieved_attrs, sconn.gate.encoding_check);
    Datum* values = palloc(ncols * sizeof(Datum));
    bool* nulls   = palloc0(ncols * sizeof(bool));

    PG_TRY();
    {
        ch_cursor* cursor = sconn.gate.methods->simple_query(sconn.gate.conn, &query);

        for (;;) {
            ChFdwScanRowContext ctx = {
                .tupdesc         = tupdesc,
                .retrieved_attrs = retrieved_attrs,
                .attinmeta       = attinmeta,
                .cursor          = cursor,
                .values          = values,
                .nulls           = nulls,
            };

            if (sconn.gate.methods->fetch_row(&ctx) == NULL) {
                break;
            }

            char** row = palloc(ncols * sizeof(char*));

            for (int i = 0; i < ncols; i++) {
                row[i] = nulls[i] ? NULL : TextDatumGetCString(values[i]);
            }
            rows = lappend(rows, row);
        }
        MemoryContextDelete(cursor->memcxt);
    }
    PG_FINALLY();
    { chfdw_release_scan_connection(sconn); }
    PG_END_TRY();

    return rows;
}

/*
 * Map a ClickHouse distance function name to a vector metric.
 */
chfdw_vector_metric
chfdw_vector_metric_from_ch(const char* name) {
    if (strcmp(name, "L2Distance") == 0) {
        return VECTOR_METRIC_L2;
    } else if (strcmp(name, "cosineDistance") == 0) {
        return VECTOR_METRIC_COSINE;
    } else if (strcmp(name, "dotProduct") == 0) {
        return VECTOR_METRIC_DOT;
    }
    return VECTOR_METRIC_NONE;
}

/*
 * Copy the next argument of a vector_similarity() declaration into out,
 * without quotes, and advance *p past it. Returns false at the end.
 */
static bool
next_index_arg(const char** p, StringInfo out) {
    const char* s = *p;

    resetStringInfo(out);
    while (*s == ' ' || *s == ',') {
        s++;
    }
    if (*s == '\0' || *s == ')') {
        return false;
    }
    if (*s == '\'') {
        for (s++; *s && *s != '\''; s++) {
            if (*s == '\\' && s[1]) {
                s++;
            }
            appendStringInfoChar(out, *s);
        }
        if (*s == '\'') {
            s++;
        }
    } else {
        for (; *s && *s != ',' && *s != ')'; s++) {
            if (*s != ' ') {
                appendStringInfoChar(out, *s);
            }
        }
    }
    *p = s;
    return true;
}

/*
 * Parse type_full, e.g. vector_similarity('hnsw', 'L2Distance', 4, 'bf16',
 * 16, 64), into info. Returns false for an unexpected shape.
 */
static bool
parse_vector_index(const char* type_full, VectorIndexInfo* info) {
    const char* p = strchr(type_full, '(');
    StringInfoData arg;
    int n = 0;

    if (!p) {
        return false;
    }
    p++;
    initStringInfo(&arg);
    while (next_index_arg(&p, &arg)) {
        switch (n++) {
        case 1:
            info->metric = chfdw_vector_metric_from_ch(arg.data);
            break;
        case 2:
            info->dims = atoi(arg.data);
            break;
        case 3:
            info->quantization = pstrdup(arg.data);
            break;
        case 4:
            info->m = atoi(arg.data);
            break;
        case 5:
            info->ef_construction = atoi(arg.data);
            break;
        default:
            break;
        }
    }
    pfree(arg.data);
    return n >= 3 && info->metric != VECTOR_METRIC_NONE;
}

/*
 * Return the dimensions of a vector_similarity() index declaration, or 0.
 */
int
chfdw_vector_index_dims(const char* type_full) {
    VectorIndexInfo info = {};

    return parse_vector_index(type_full, &info) ? info.dims : 0;
}

/*
 * Return a column expression as ClickHouse prints it, with a name that needs
 * quoting unquoted: `Mixed Col` is Mixed Col.
 */
static char*
unquote_column(const char* expr) {
    size_t len = strlen(expr);
    StringInfoData out;

    if (len < 2 || expr[0] != '`' || expr[len - 1] != '`') {
        return pstrdup(expr);
    }
    initStringInfo(&out);
    for (size_t i = 1; i < len - 1; i++) {
        if (expr[i] == '\\' && i + 2 < len) {
            i++;
        }
        appendStringInfoChar(&out, expr[i]);
    }
    return out.data;
}

/*
 * Query ClickHouse for the vector indexes of the remote table and the
 * effective max_limit_for_vector_search_queries, which system.settings
 * reports for the settings this query carries.
 */
static void
fetch_meta(
    UserMapping* user,
    const char* db,
    const char* table,
    VectorTableMeta* meta,
    MemoryContext cxt
) {
    StringInfoData sql;

    initStringInfo(&sql);
    appendStringInfoString(
        &sql,
        "SELECT name, expr, type_full, toString((SELECT value FROM system.settings "
        "WHERE name = 'max_limit_for_vector_search_queries')) "
        "FROM system.data_skipping_indices WHERE type = 'vector_similarity' "
        "AND database = "
    );
    ch_append_literal(&sql, db, strlen(db));
    appendStringInfoString(&sql, " AND table = ");
    ch_append_literal(&sql, table, strlen(table));
    appendStringInfoString(&sql, " ORDER BY name");

    ListCell* lc;

    List* rows = chfdw_run_text_query(user, sql.data, 4);

    /* Keep only the parsed indexes in the entry's long-lived context */
    MemoryContext old = MemoryContextSwitchTo(cxt);

    foreach (lc, rows) {
        char** row             = lfirst(lc);
        VectorIndexInfo* index = palloc0(sizeof(VectorIndexInfo));

        if (!row[0] || !row[1] || !row[2] || !parse_vector_index(row[2], index)) {
            pfree(index);
            continue;
        }
        index->name   = pstrdup(row[0]);
        index->column = unquote_column(row[1]);
        meta->indexes = lappend(meta->indexes, index);
        if (row[3]) {
            meta->limit_cap = strtoll(row[3], NULL, 10);
        }
    }
    MemoryContextSwitchTo(old);
}

/*
 * Remove the entries other than key fetched more than ttl milliseconds before
 * now, such as those of dropped tables, so that the cache holds only tables
 * planned within the time to live.
 */
static void
evict_stale(const VectorMetaKey* key, TimestampTz now, int ttl) {
    HASH_SEQ_STATUS scan;
    VectorMetaEntry* entry;

    hash_seq_init(&scan, vector_meta_cache);
    while ((entry = hash_seq_search(&scan)) != NULL) {
        if (memcmp(&entry->key, key, sizeof(*key)) != 0 &&
            TimestampDifferenceExceeds(entry->fetched_at, now, ttl)) {
            MemoryContextDelete(entry->cxt);
            hash_search(vector_meta_cache, &entry->key, HASH_REMOVE, NULL);
        }
    }
}

/*
 * Return the vector index metadata for foreign table relid as seen by user,
 * from the cache when fresh. Returns NULL when the lookup fails.
 */
const VectorTableMeta*
chfdw_vector_table_meta(Oid relid, UserMapping* user) {
    VectorMetaKey key = { relid, user->userid };
    TimestampTz now   = GetCurrentTimestamp();
    bool found;

    if (!vector_meta_cache) {
        HASHCTL ctl = {
            .keysize   = sizeof(VectorMetaKey),
            .entrysize = sizeof(VectorMetaEntry),
        };

        meta_cxt = AllocSetContextCreate(
            CacheMemoryContext, "pg_clickhouse vector metadata", ALLOCSET_SMALL_SIZES
        );
        ctl.hcxt          = meta_cxt;
        vector_meta_cache = hash_create(
            "pg_clickhouse vector metadata",
            16,
            &ctl,
            HASH_ELEM | HASH_BLOBS | HASH_CONTEXT
        );
    }

    VectorMetaEntry* entry = hash_search(vector_meta_cache, &key, HASH_ENTER, &found);

    /* Milliseconds, capped so that a TTL of many days does not overflow */
    int ttl = Min(chfdw_vector_metadata_ttl(), INT_MAX / 1000);

    if (found && ttl > 0 &&
        !TimestampDifferenceExceeds(entry->fetched_at, now, ttl * 1000)) {
        return &entry->meta;
    }
    evict_stale(&key, now, ttl * 1000);

    char* db;
    char* table;
    MemoryContext caller = CurrentMemoryContext;
    ResourceOwner owner  = CurrentResourceOwner;
    VectorTableMeta meta = { .limit_cap = -1 };
    MemoryContext cxt    = AllocSetContextCreate(
        meta_cxt, "pg_clickhouse vector table", ALLOCSET_SMALL_SIZES
    );

    chfdw_remote_table_name(relid, &db, &table);

    /* A failed lookup must not fail the query, so run it in a subtransaction */
    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(caller);
    PG_TRY();
    {
        fetch_meta(user, db, table, &meta, cxt);
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(caller);
        CurrentResourceOwner = owner;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(caller);
        ErrorData* edata = CopyErrorData();

        FlushErrorState();
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(caller);
        CurrentResourceOwner = owner;
        MemoryContextDelete(cxt);
        if (found) {
            MemoryContextDelete(entry->cxt);
        }
        hash_search(vector_meta_cache, &key, HASH_REMOVE, NULL);
        elog(
            DEBUG1,
            "pg_clickhouse: could not read vector indexes of %s.%s: %s",
            db,
            table,
            edata->message
        );
        return NULL;
    }
    PG_END_TRY();

    /* Free the indexes of the previous fetch */
    if (found) {
        MemoryContextDelete(entry->cxt);
    }
    entry->cxt        = cxt;
    entry->meta       = meta;
    entry->fetched_at = now;
    return &entry->meta;
}

/*
 * SQL: clickhouse_vector_indexes(foreign_table regclass) returns the vector
 * similarity indexes ClickHouse reports for the table, with their size and
 * the build mutations not yet done.
 */
Datum
clickhouse_vector_indexes(PG_FUNCTION_ARGS) {
    Oid relid             = PG_GETARG_OID(0);
    ReturnSetInfo* rsinfo = (ReturnSetInfo*)fcinfo->resultinfo;
    TupleDesc tupdesc;
    char* db;
    char* table;
    StringInfoData sql;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        elog(ERROR, "return type must be a row type");
    }
    if (!chfdw_is_clickhouse_table(relid)) {
        ereport(
            ERROR,
            errcode(ERRCODE_WRONG_OBJECT_TYPE),
            errmsg("\"%s\" is not a ClickHouse foreign table", get_rel_name(relid))
        );
    }
    chfdw_remote_table_name(relid, &db, &table);

    StringInfoData db_lit;
    StringInfoData table_lit;

    initStringInfo(&db_lit);
    initStringInfo(&table_lit);
    ch_append_literal(&db_lit, db, strlen(db));
    ch_append_literal(&table_lit, table, strlen(table));
    initStringInfo(&sql);
    appendStringInfo(
        &sql,
        "SELECT name, expr, type_full, toString(data_compressed_bytes), "
        "toString(arrayCount(c -> position(c, concat('INDEX ', name, ')')) > 0 OR "
        "position(c, concat('INDEX ', name, ' ')) > 0 OR "
        "position(c, concat('INDEX `', name, '`')) > 0, "
        "(SELECT groupArray(command) FROM system.mutations WHERE database = %s "
        "AND table = %s AND NOT is_done))) FROM system.data_skipping_indices "
        "WHERE database = %s AND table = %s AND type = 'vector_similarity' ORDER BY "
        "name",
        db_lit.data,
        table_lit.data,
        db_lit.data,
        table_lit.data
    );

    List* rows = chfdw_run_text_query(
        GetUserMapping(GetUserId(), GetForeignTable(relid)->serverid), sql.data, 5
    );
    MemoryContext old = MemoryContextSwitchTo(rsinfo->econtext->ecxt_per_query_memory);
    Tuplestorestate* store   = tuplestore_begin_heap(true, false, work_mem);
    AttInMetadata* attinmeta = TupleDescGetAttInMetadata(CreateTupleDescCopy(tupdesc));
    ListCell* lc;

    foreach (lc, rows) {
        tuplestore_puttuple(store, BuildTupleFromCStrings(attinmeta, lfirst(lc)));
    }
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = store;
    rsinfo->setDesc    = attinmeta->tupdesc;
    MemoryContextSwitchTo(old);
    return (Datum)0;
}
