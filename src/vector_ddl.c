/*-------------------------------------------------------------------------
 *
 * vector_ddl.c
 *	  CREATE, DROP, and REINDEX of ClickHouse vector indexes from Postgres.
 *
 * Postgres cannot index a foreign table, so a utility hook turns
 * CREATE INDEX ... USING clickhouse on a pg_clickhouse foreign table into a
 * ClickHouse vector similarity index, and DROP INDEX and REINDEX of such an
 * index into their ClickHouse counterparts. Postgres has no catalog entry for
 * these indexes, so DROP INDEX and REINDEX INDEX look for a name Postgres
 * doesn't know among the vector indexes ClickHouse reports for the foreign
 * tables on the search path. ClickHouse DDL is not transactional: rolling
 * back a transaction does not undo it.
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/vector_ddl.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "cursor.h"
#include "fdw.h"

static ProcessUtility_hook_type prev_utility_hook = NULL;

/* Return true if relid is a foreign table of a clickhouse_fdw server */
static bool
is_clickhouse_table(Oid relid) {
    if (!OidIsValid(relid) || get_rel_relkind(relid) != RELKIND_FOREIGN_TABLE) {
        return false;
    }

    ForeignServer* server = GetForeignServer(GetForeignTable(relid)->serverid);
    char* handler = get_func_name(GetForeignDataWrapper(server->fdwid)->fdwhandler);

    return handler && strcmp(handler, "clickhouse_fdw_handler") == 0;
}

static void
check_owner(Oid relid) {
#if PG_VERSION_NUM >= 160000
    if (!object_ownercheck(RelationRelationId, relid, GetUserId()))
#else
    if (!pg_class_ownercheck(relid, GetUserId()))
#endif
    {
        aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_FOREIGN_TABLE, get_rel_name(relid));
    }
}

static UserMapping*
table_user(Oid relid) {
    return GetUserMapping(GetUserId(), GetForeignTable(relid)->serverid);
}

/* Quote an identifier for ClickHouse, which escapes backslashes in quotes too */
static const char*
ch_ident(const char* name) {
    StringInfoData buf;

    initStringInfo(&buf);
    appendStringInfoChar(&buf, '"');
    for (; *name; name++) {
        if (*name == '"' || *name == '\\') {
            appendStringInfoChar(&buf, *name == '"' ? '"' : '\\');
        }
        appendStringInfoChar(&buf, *name);
    }
    appendStringInfoChar(&buf, '"');
    return buf.data;
}

/* Run ALTER TABLE <remote table> <action> on the foreign table's server */
static void
alter_remote_table(Oid relid, const char* action) {
    UserMapping* user        = table_user(relid);
    ch_scan_connection sconn = chfdw_get_scan_connection(user);
    char* db;
    char* table;

    chfdw_remote_table_name(relid, &db, &table);

    ch_query query = new_query(
        psprintf("ALTER TABLE %s.%s %s", ch_ident(db), ch_ident(table), action),
        0,
        NULL,
        NULL,
        NULL,
        sconn.gate.encoding_check
    );

    /* Release the connection on error too, or later scans could not use it */
    PG_TRY();
    {
        MemoryContextDelete(
            sconn.gate.methods->simple_query(sconn.gate.conn, &query)->memcxt
        );
    }
    PG_FINALLY();
    { chfdw_release_scan_connection(sconn); }
    PG_END_TRY();
    chfdw_vector_meta_invalidate_all();
}

static void
materialize(Oid relid, const char* index, bool concurrently) {
    alter_remote_table(
        relid,
        psprintf(
            "MATERIALIZE INDEX %s SETTINGS mutations_sync = %d",
            ch_ident(index),
            concurrently ? 0 : 2
        )
    );
}

/* Return true if the current user may connect to relid's server */
static bool
has_user_mapping(Oid relid) {
    Oid serverid = GetForeignTable(relid)->serverid;

    return SearchSysCacheExists2(
               USERMAPPINGUSERSERVER,
               ObjectIdGetDatum(GetUserId()),
               ObjectIdGetDatum(serverid)
           ) ||
           SearchSysCacheExists2(
               USERMAPPINGUSERSERVER,
               ObjectIdGetDatum(InvalidOid),
               ObjectIdGetDatum(serverid)
           );
}

/* Return true if ClickHouse reports a vector index of relid named name */
static bool
has_index(Oid relid, const char* name) {
    const VectorTableMeta* meta = chfdw_vector_table_meta(relid, table_user(relid));
    ListCell* lc;

    foreach (lc, meta ? meta->indexes : NIL) {
        if (strcmp(((VectorIndexInfo*)lfirst(lc))->name, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Return true if two foreign tables are the same table on the same server */
static bool
same_remote_table(Oid a, Oid b) {
    char *db_a, *table_a, *db_b, *table_b;

    chfdw_remote_table_name(a, &db_a, &table_a);
    chfdw_remote_table_name(b, &db_b, &table_b);
    return GetForeignTable(a)->serverid == GetForeignTable(b)->serverid &&
           strcmp(db_a, db_b) == 0 && strcmp(table_a, table_b) == 0;
}

/*
 * Return the foreign table with the ClickHouse vector index named by rv,
 * searching the pg_clickhouse foreign tables in its schema or on the search
 * path. Returns InvalidOid when Postgres knows the name or no table has it.
 */
static Oid
lookup_index(RangeVar* rv) {
    if (OidIsValid(RangeVarGetRelid(rv, NoLock, true))) {
        return InvalidOid;
    }

    /* A missing schema is for Postgres to report, or skip with IF EXISTS */
    List* schemas = rv->schemaname
                        ? list_make1_oid(get_namespace_oid(rv->schemaname, true))
                        : fetch_search_path(false);
    Oid found     = InvalidOid;
    Oid type      = OIDOID;
    ListCell* lc;

    SPI_connect();
    foreach (lc, schemas) {
        Datum nsp = ObjectIdGetDatum(lfirst_oid(lc));

        SPI_execute_with_args(
            "SELECT oid FROM pg_catalog.pg_class WHERE relkind = 'f' AND relnamespace "
            "= $1 ORDER BY oid",
            1,
            &type,
            &nsp,
            NULL,
            true,
            0
        );
        for (uint64 i = 0; i < SPI_processed; i++) {
            bool isnull;
            Oid relid = DatumGetObjectId(
                SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull)
            );

            /* Lock before reading the table, which a concurrent DROP may remove */
            LockRelationOid(relid, AccessShareLock);
            if (SearchSysCacheExists1(RELOID, ObjectIdGetDatum(relid)) &&
                is_clickhouse_table(relid) && has_user_mapping(relid) &&
                has_index(relid, rv->relname)) {
                if (OidIsValid(found) && same_remote_table(found, relid)) {
                    /* The same index through another foreign table */
                    UnlockRelationOid(relid, AccessShareLock);
                    continue;
                }
                if (OidIsValid(found)) {
                    ereport(
                        ERROR,
                        errcode(ERRCODE_AMBIGUOUS_PARAMETER),
                        errmsg(
                            "pg_clickhouse: index \"%s\" exists on foreign tables "
                            "\"%s\" and \"%s\"",
                            rv->relname,
                            get_rel_name(found),
                            get_rel_name(relid)
                        ),
                        errhint("Give the indexes distinct names.")
                    );
                }
                found = relid;
            } else {
                UnlockRelationOid(relid, AccessShareLock);
            }
        }
        if (OidIsValid(found)) {
            break;
        }
    }
    SPI_finish();
    return found;
}

/* Map a pgvector operator class name to a ClickHouse distance function */
static const char*
opclass_metric(List* opclass) {
    const char* name = opclass ? strVal(llast(opclass)) : "";

    if (strcmp(name, "vector_l2_ops") == 0) {
        return "L2Distance";
    } else if (strcmp(name, "vector_cosine_ops") == 0) {
        return "cosineDistance";
    } else if (strcmp(name, "vector_ip_ops") == 0) {
        return "dotProduct";
    }
    return NULL;
}

static void
create_index(IndexStmt* stmt, Oid relid) {
    if (strcmp(stmt->accessMethod, "clickhouse") != 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_WRONG_OBJECT_TYPE),
            errmsg(
                "pg_clickhouse: cannot create a %s index on foreign table \"%s\"",
                stmt->accessMethod,
                get_rel_name(relid)
            ),
            errhint("Use CREATE INDEX ... USING clickhouse (column vector_cosine_ops).")
        );
    }
    check_owner(relid);

    /* Index the vector column; ClickHouse filters on any other column */
    IndexElem* vec     = NULL;
    const char* metric = NULL;
    ListCell* lc;

    foreach (lc, stmt->indexParams) {
        IndexElem* elem = lfirst(lc);

        if (!opclass_metric(elem->opclass)) {
            ereport(NOTICE, errmsg("pg_clickhouse: ignoring non-vector index column"));
            continue;
        }
        if (vec || !elem->name || stmt->unique || stmt->whereClause) {
            ereport(
                ERROR,
                errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg(
                    "pg_clickhouse: a vector index takes one vector column and no "
                    "UNIQUE or WHERE"
                )
            );
        }
        vec    = elem;
        metric = opclass_metric(elem->opclass);
    }
    if (!vec) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "pg_clickhouse: a vector index needs a column with vector_l2_ops, "
                "vector_cosine_ops, or vector_ip_ops"
            )
        );
    }
    if (!chfdw_version_ge(chfdw_get_server_version(table_user(relid)), 25, 8)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("pg_clickhouse: vector similarity indexes require ClickHouse 25.8")
        );
    }

    AttrNumber attnum = get_attnum(relid, vec->name);

    if (attnum == InvalidAttrNumber) {
        ereport(
            ERROR,
            errcode(ERRCODE_UNDEFINED_COLUMN),
            errmsg(
                "column \"%s\" of relation \"%s\" does not exist",
                vec->name,
                get_rel_name(relid)
            )
        );
    }
    Oid atttype;
    int32 dims;
    Oid collation;

    get_atttypetypmodcoll(relid, attnum, &atttype, &dims, &collation);

    const char* quant   = NULL;
    int m               = 32;
    int ef              = 128;
    int64 granularity   = 0;
    bool do_materialize = true;

    foreach (lc, stmt->options) {
        DefElem* def = lfirst(lc);

        if (strcmp(def->defname, "quantization") == 0) {
            quant = defGetString(def);
        } else if (strcmp(def->defname, "m") == 0) {
            m = defGetInt32(def);
        } else if (strcmp(def->defname, "ef_construction") == 0) {
            ef = defGetInt32(def);
        } else if (strcmp(def->defname, "dimensions") == 0) {
            dims = defGetInt32(def);
        } else if (strcmp(def->defname, "granularity") == 0) {
            granularity = defGetInt64(def);
        } else if (strcmp(def->defname, "materialize") == 0) {
            do_materialize = defGetBoolean(def);
        } else {
            ereport(
                ERROR,
                errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg(
                    "pg_clickhouse: unrecognized vector index option \"%s\"",
                    def->defname
                )
            );
        }
    }
    if (dims <= 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_INVALID_PARAMETER_VALUE),
            errmsg("pg_clickhouse: vector index on \"%s\" needs dimensions", vec->name),
            errhint("Declare the column as vector(n) or set WITH (dimensions = n).")
        );
    }

    const char* name = stmt->idxname
                           ? stmt->idxname
                           : psprintf("%s_%s_idx", get_rel_name(relid), vec->name);

    if (stmt->if_not_exists && has_index(relid, name)) {
        ereport(
            NOTICE, errmsg("pg_clickhouse: index \"%s\" already exists, skipping", name)
        );
        return;
    }

    StringInfoData action;

    initStringInfo(&action);
    appendStringInfo(
        &action,
        "ADD INDEX %s %s TYPE vector_similarity('hnsw', '%s', %d",
        ch_ident(name),
        ch_ident(chfdw_get_custom_column_info(relid, attnum)->colname),
        metric,
        dims
    );
    if (quant) {
        appendStringInfo(&action, ", %s, %d, %d", ch_quote_literal(quant), m, ef);
    }
    appendStringInfoChar(&action, ')');
    if (granularity > 0) {
        appendStringInfo(&action, " GRANULARITY " INT64_FORMAT, granularity);
    }
    alter_remote_table(relid, action.data);
    if (do_materialize) {
        materialize(relid, name, stmt->concurrent);
    }
}

/* Return true if the statement was a ClickHouse vector index command */
static bool
vector_index_utility(Node* parsetree) {
    if (IsA(parsetree, IndexStmt)) {
        IndexStmt* stmt = (IndexStmt*)parsetree;
        Oid relid = RangeVarGetRelid(stmt->relation, ShareUpdateExclusiveLock, true);

        if (!is_clickhouse_table(relid)) {
            return false;
        }
        create_index(stmt, relid);
        return true;
    }

    if (IsA(parsetree, DropStmt) &&
        ((DropStmt*)parsetree)->removeType == OBJECT_INDEX) {
        List* objects = ((DropStmt*)parsetree)->objects;
        List* tables  = NIL;
        bool remote   = false;
        ListCell* lc;
        ListCell* tc;

        foreach (lc, objects) {
            Oid relid = lookup_index(makeRangeVarFromNameList(lfirst(lc)));

            tables = lappend_oid(tables, relid);
            remote |= OidIsValid(relid);
        }
        if (!remote) {
            return false;
        }
        foreach (tc, tables) {
            if (!OidIsValid(lfirst_oid(tc))) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg(
                        "pg_clickhouse: cannot drop ClickHouse and other indexes "
                        "together"
                    )
                );
            }
            check_owner(lfirst_oid(tc));
        }
        forboth(lc, objects, tc, tables) {
            alter_remote_table(
                lfirst_oid(tc),
                psprintf(
                    "DROP INDEX %s",
                    ch_ident(makeRangeVarFromNameList(lfirst(lc))->relname)
                )
            );
        }
        return true;
    }

    if (IsA(parsetree, ReindexStmt)) {
        ReindexStmt* stmt = (ReindexStmt*)parsetree;
        bool concurrently = false;
        Oid relid;
        ListCell* lc;

        foreach (lc, stmt->params) {
            if (strcmp(((DefElem*)lfirst(lc))->defname, "concurrently") == 0) {
                concurrently = defGetBoolean(lfirst(lc));
            }
        }
        if (stmt->kind == REINDEX_OBJECT_INDEX &&
            OidIsValid(relid = lookup_index(stmt->relation))) {
            check_owner(relid);
            materialize(relid, stmt->relation->relname, concurrently);
            return true;
        }
        if (stmt->kind == REINDEX_OBJECT_TABLE &&
            is_clickhouse_table(
                relid = RangeVarGetRelid(stmt->relation, NoLock, true)
            )) {
            check_owner(relid);
            chfdw_vector_meta_invalidate_all();

            const VectorTableMeta* meta =
                chfdw_vector_table_meta(relid, table_user(relid));

            /* Materializing invalidates the cache that owns meta, so copy the names */
            List* names = NIL;

            foreach (lc, meta ? meta->indexes : NIL) {
                names = lappend(names, pstrdup(((VectorIndexInfo*)lfirst(lc))->name));
            }
            foreach (lc, names) {
                materialize(relid, lfirst(lc), concurrently);
            }
            return true;
        }
    }
    return false;
}

static void
vector_utility_hook(
    PlannedStmt* pstmt,
    const char* queryString,
    bool readOnlyTree,
    ProcessUtilityContext context,
    ParamListInfo params,
    QueryEnvironment* queryEnv,
    DestReceiver* dest,
    QueryCompletion* qc
) {
    if (vector_index_utility(pstmt->utilityStmt)) {
        return;
    }
    (prev_utility_hook ? prev_utility_hook : standard_ProcessUtility)(
        pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc
    );
}

void
chfdw_install_vector_ddl_hook(void) {
    prev_utility_hook   = ProcessUtility_hook;
    ProcessUtility_hook = vector_utility_hook;
}
