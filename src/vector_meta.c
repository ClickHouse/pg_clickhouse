/*-------------------------------------------------------------------------
 *
 * vector_meta.c
 *	  Cache of the ClickHouse vector similarity indexes and QBit columns on
 *	  foreign tables.
 *
 * ClickHouse is the source of truth for vector indexes, so the planner reads
 * them from system.data_skipping_indices when it plans a vector search and
 * caches the result per backend for pg_clickhouse.vector_metadata_ttl
 * seconds. Nothing is stored in Postgres. The indexes drive only warnings and
 * EXPLAIN output, never query results, so a failed lookup is logged and
 * ignored. The QBit columns, from system.columns, decide how to push down a
 * vector distance; after a failed lookup, it pushes down as on an Array
 * column.
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
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "parser/parsetree.h"
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

PG_FUNCTION_INFO_V1(clickhouse_refresh_vector_metadata);
PG_FUNCTION_INFO_V1(clickhouse_vector_indexes);

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
 * reports for the settings this query carries. Rows for the QBit columns of
 * the table follow, with whether ClickHouse has dotProductTransposed().
 */
static void
fetch_meta(
    UserMapping* user,
    const char* db,
    const char* table,
    VectorTableMeta* meta,
    MemoryContext cxt
) {
    StringInfoData db_lit;
    StringInfoData table_lit;

    initStringInfo(&db_lit);
    initStringInfo(&table_lit);
    ch_append_literal(&db_lit, db, strlen(db));
    ch_append_literal(&table_lit, table, strlen(table));

    char* sql = psprintf(
        "SELECT * FROM (SELECT name, "
        /* ClickHouse backtick-quotes a column name that needs it: drop them */
        "replaceRegexpAll(expr, '^`|`$|\\\\\\\\(.)', '\\\\1') AS expr, "
        /* Distance function, second argument of vector_similarity() */
        "extract(type_full, ', *''([^'']+)''') AS dist, '0' AS bits, "
        "toString((SELECT value FROM system.settings "
        "WHERE name = 'max_limit_for_vector_search_queries')) AS extra "
        "FROM system.data_skipping_indices WHERE type = 'vector_similarity' "
        "AND database = %s AND table = %s "
        /* QBit columns: the bits of each element, and dotProductTransposed() */
        "UNION ALL SELECT name, '' AS expr, '' AS dist, "
        "toString(multiIf(startsWith(type, 'QBit(BFloat16,'), 16, "
        "startsWith(type, 'QBit(Float32,'), 32, "
        "startsWith(type, 'QBit(Float64,'), 64, 0)) AS bits, "
        "toString((SELECT count() FROM system.functions "
        "WHERE name = 'dotProductTransposed')) AS extra "
        "FROM system.columns WHERE database = %s AND table = %s "
        "AND startsWith(type, 'QBit(')) ORDER BY name",
        db_lit.data,
        table_lit.data,
        db_lit.data,
        table_lit.data
    );
    List* rows = chfdw_run_text_query(user, sql, 5);

    /* Keep only the parsed indexes in the entry's long-lived context */
    MemoryContext old = MemoryContextSwitchTo(cxt);
    ListCell* lc;

    foreach (lc, rows) {
        char** row = lfirst(lc);
        int bits   = row[3] ? pg_strtoint32(row[3]) : 0;

        if (bits) {
            VectorQBitInfo* qbit = palloc(sizeof(VectorQBitInfo));

            qbit->column         = pstrdup(row[0]);
            qbit->bits           = bits;
            meta->qbits          = lappend(meta->qbits, qbit);
            meta->dot_transposed = row[4] && strcmp(row[4], "0") != 0;
            continue;
        }

        chfdw_vector_metric metric = chfdw_vector_metric_from_ch(row[2]);

        if (metric == VECTOR_METRIC_NONE) {
            continue;
        }

        VectorIndexInfo* index = palloc(sizeof(VectorIndexInfo));

        index->metric = metric;
        index->name   = pstrdup(row[0]);
        index->column = pstrdup(row[1]);
        meta->indexes = lappend(meta->indexes, index);
        if (row[4] && *row[4]) {
            meta->limit_cap = strtoll(row[4], NULL, 10);
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
 * Return the element bits of var when it's a QBit column of a foreign table
 * planned in root, otherwise 0, and set *dot to whether ClickHouse has
 * dotProductTransposed(). Reads the metadata once per planning of the table,
 * so that the planner and the deparser agree. A failed lookup reports no QBit
 * columns.
 */
int
chfdw_qbit_bits(PlannerInfo* root, Var* var, bool* dot) {
    int varno = (int)var->varno; /* unsigned in Postgres 14 */

    *dot = false;
    if (var->varlevelsup != 0 || var->varattno <= 0 || varno <= 0 ||
        varno >= root->simple_rel_array_size) {
        return 0;
    }

    RelOptInfo* rel = root->simple_rel_array[varno];
    Oid relid       = planner_rt_fetch(varno, root)->relid;

    if (!rel || !IS_SIMPLE_REL(rel) || !rel->fdw_private ||
        var->varattno > rel->max_attr || !chfdw_is_clickhouse_table(relid)) {
        return 0;
    }

    CHFdwRelationInfo* fpinfo = rel->fdw_private;

    if (!fpinfo->qbit_bits) {
        UserMapping* user = chfdw_rel_user_mapping(root, rel);
        const VectorTableMeta* meta =
            user ? chfdw_vector_table_meta(relid, user) : NULL;
        int* bits = MemoryContextAllocZero(
            GetMemoryChunkContext(fpinfo), (rel->max_attr + 1) * sizeof(int)
        );

        for (int i = 1; meta && meta->qbits && i <= rel->max_attr; i++) {
            CustomColumnInfo* col = chfdw_get_custom_column_info(relid, i);
            const char* name =
                col && col->colname ? col->colname : get_attname(relid, i, true);
            ListCell* lc;

            foreach (lc, meta->qbits) {
                VectorQBitInfo* qbit = lfirst(lc);

                if (name && strcmp(name, qbit->column) == 0) {
                    bits[i] = qbit->bits;
                }
            }
        }
        fpinfo->qbit_dot  = meta && meta->dot_transposed;
        fpinfo->qbit_bits = bits;
    }
    *dot = fpinfo->qbit_dot;
    return fpinfo->qbit_bits[var->varattno];
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
