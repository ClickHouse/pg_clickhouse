/*-------------------------------------------------------------------------
 *
 * vector_meta.c
 *	  Read the ClickHouse vector similarity indexes for foreign tables.
 *
 * ClickHouse is the source of truth for vector indexes, so EXPLAIN reads them
 * from its system.data_skipping_indices catalog. We don't store this data in
 * Postgres. The metadata drives only EXPLAIN output, never query results, so
 * a failed lookup is logged and ignored.
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
#include "utils/resowner.h"

#include "cursor.h"
#include "fdw.h"

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
    VectorTableMeta* meta
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
    ListCell* lc;

    foreach (lc, rows) {
        char** row                 = lfirst(lc);
        chfdw_vector_metric metric = chfdw_vector_metric_from_ch(row[2]);

        if (metric == VECTOR_METRIC_NONE) {
            continue;
        }

        VectorIndexInfo* index = palloc(sizeof(VectorIndexInfo));

        index->metric = metric;
        index->name   = row[0];
        index->column = row[1];
        meta->indexes = lappend(meta->indexes, index);
        if (row[3] && *row[3]) {
            meta->limit_cap = strtoll(row[3], NULL, 10);
        }
    }
}

/*
 * Read the vector index metadata for foreign table relid as userid sees it on
 * server serverid into meta. Returns false when the lookup fails.
 */
bool
chfdw_vector_table_meta(Oid relid, UserMapping* user, VectorTableMeta* meta) {
    char* db;
    char* table;
    MemoryContext caller = CurrentMemoryContext;
    ResourceOwner owner  = CurrentResourceOwner;

    *meta = (VectorTableMeta){ .limit_cap = -1 };
    chfdw_remote_table_name(relid, &db, &table);

    /*
     * A failed lookup must not fail the context in which it runs, EXPLAIN, so
     * run it in a subtransaction.
     */
    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(caller);
    PG_TRY();
    {
        meta->old_server = !chfdw_version_ge(chfdw_get_server_version(user), 25, 8);
        if (!meta->old_server) {
            fetch_meta(user, db, table, meta);
        }
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
        elog(
            DEBUG1,
            "pg_clickhouse: could not read vector indexes of %s.%s: %s",
            db,
            table,
            edata->message
        );
        return false;
    }
    PG_END_TRY();
    return true;
}
