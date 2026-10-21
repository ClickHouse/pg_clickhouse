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
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/timestamp.h"

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

PG_FUNCTION_INFO_V1(clickhouse_refresh_vector_metadata);

/*
 * Drop every cached entry. Called after pg_clickhouse changes an index and by
 * clickhouse_refresh_vector_metadata().
 */
void
chfdw_vector_meta_invalidate_all(void) {
    if (meta_cxt) {
        MemoryContextDelete(meta_cxt);
    }
    meta_cxt          = NULL;
    vector_meta_cache = NULL;
}

/* SQL: clickhouse_refresh_vector_metadata() */
Datum
clickhouse_refresh_vector_metadata(PG_FUNCTION_ARGS pg_attribute_unused()) {
    chfdw_vector_meta_invalidate_all();
    PG_RETURN_VOID();
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
 * Query ClickHouse for the vector indexes of the remote table and the
 * value of effective max_limit_for_vector_search_queries, which system.settings
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
        "SELECT name, "
        /* ClickHouse backtick-quotes a column name that needs it: drop them */
        "replaceRegexpAll(expr, '^`|`$|\\\\\\\\(.)', '\\\\1'), "
        /* Distance function, second argument of vector_similarity() */
        "extract(type_full, ', *''([^'']+)'''), "
        "toString((SELECT value FROM system.settings "
        "WHERE name = 'max_limit_for_vector_search_queries')) "
        "FROM system.data_skipping_indices WHERE type = 'vector_similarity' "
        "AND database = "
    );
    ch_append_literal(&sql, db, strlen(db));
    appendStringInfoString(&sql, " AND table = ");
    ch_append_literal(&sql, table, strlen(table));
    appendStringInfoString(&sql, " ORDER BY name");

    List* rows = chfdw_run_text_query(user, sql.data, 4);

    /* Keep only the parsed indexes in the entry's long-lived context */
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ListCell* lc;

    foreach (lc, rows) {
        char** row                 = lfirst(lc);
        chfdw_vector_metric metric = chfdw_vector_metric_from_ch(row[2]);

        if (metric == VECTOR_METRIC_NONE) {
            continue;
        }

        VectorIndexInfo* index = palloc(sizeof(VectorIndexInfo));

        index->metric = metric;
        index->name   = pstrdup(row[0]);
        index->column = pstrdup(row[1]);
        meta->indexes = lappend(meta->indexes, index);
        if (row[3] && *row[3]) {
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
