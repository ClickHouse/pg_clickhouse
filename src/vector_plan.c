/*-------------------------------------------------------------------------
 *
 * vector_plan.c
 *	  Classify vector searches on foreign tables and explain index use.
 *
 * When a query orders a foreign table by a pgvector distance, decide whether
 * ClickHouse can serve it from a vector similarity index. ClickHouse falls
 * back to an exact search whenever it cannot, so results never depend on
 * this; the classification only drives planner warnings and EXPLAIN output.
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/vector_plan.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/stratnum.h"
#include "commands/explain.h"
#include "miscadmin.h"
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#endif
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#include "fdw.h"

/* ClickHouse function and pgvector operator of each metric */
static const struct {
    const char* func;
    const char* op;
} metrics[] = {
    [VECTOR_METRIC_L2]     = { "L2Distance",     "<->" },
    [VECTOR_METRIC_COSINE] = { "cosineDistance", "<=>" },
    [VECTOR_METRIC_DOT]    = { "dotProduct",     "<#>" },
    [VECTOR_METRIC_L1]     = { "L1Distance",     "<+>" },
};

/* Servers already warned about lacking vector indexes */
static List* warned_old_servers = NIL;

/*
 * Return true if args are a column of rel and an expression free of rel's
 * columns, in either order, setting *var to the column.
 */
static bool
column_and_query_vector(List* args, Index relid, Var** var) {
    Node* a = strip_implicit_coercions(linitial(args));
    Node* b = strip_implicit_coercions(lsecond(args));

    if (IsA(b, Var)) {
        Node* t = a;

        a = b;
        b = t;
    }
    *var = (Var*)a;
    return IsA(a, Var) && (Index)(*var)->varno == relid && (*var)->varlevelsup == 0 &&
           !bms_is_member(relid, pull_varnos(NULL, b));
}

/*
 * Return true if expr is a pgvector distance between a column of rel and a
 * query vector, setting its metric and column. Sets *index_asc to the sort
 * direction a vector index serves.
 */
static bool
vector_distance(
    Expr* expr,
    Index relid,
    chfdw_vector_metric* metric,
    bool* index_asc,
    Var** var
) {
    CustomObjectDef* cdef = NULL;
    List* args            = NIL;

    expr       = (Expr*)strip_implicit_coercions((Node*)expr);
    *index_asc = true;
    if (IsA(expr, OpExpr)) {
        cdef = chfdw_check_for_custom_operator(((OpExpr*)expr)->opno, NULL);
        args = ((OpExpr*)expr)->args;
    } else if (IsA(expr, FuncExpr)) {
        cdef = chfdw_check_for_custom_function(((FuncExpr*)expr)->funcid);
        args = ((FuncExpr*)expr)->args;
    }
    if (!cdef || list_length(args) != 2) {
        return false;
    }

    switch (cdef->cf_type) {
    case CF_VECTOR_L2:
        *metric = VECTOR_METRIC_L2;
        break;
    case CF_VECTOR_COSINE:
        *metric = VECTOR_METRIC_COSINE;
        break;
    case CF_VECTOR_NEG_IP:
        /* Ascending <#> deparses to dotProduct DESC */
        *metric = VECTOR_METRIC_DOT;
        break;
    case CF_VECTOR_L1:
        *metric = VECTOR_METRIC_L1;
        break;
    case CF_VECTOR_DOT:
        *metric    = VECTOR_METRIC_DOT;
        *index_asc = false;
        break;
    case CF_CH_FUNCTION:
        *metric = strcmp(cdef->custom_name, "L1Distance") == 0
                      ? VECTOR_METRIC_L1
                      : chfdw_vector_metric_from_ch(cdef->custom_name);
        if (*metric == VECTOR_METRIC_NONE) {
            return false;
        }
        break;
    default:
        return false;
    }
    return column_and_query_vector(args, relid, var);
}

/* Return the constant value of a LIMIT or OFFSET expression, or -1 */
static int64
const_count(Node* node) {
    if (node && IsA(node, Const) && !((Const*)node)->constisnull) {
        return DatumGetInt64(((Const*)node)->constvalue);
    }
    return node ? -1 : 0;
}

/*
 * Return why ClickHouse cannot serve the vector search of a scan from an
 * index, or NULL when it can. Sets *index_name to the index it would use, and
 * clears *warn for a reason already reported once per backend.
 */
static const char*
fallback_reason(
    PlannerInfo* root,
    RelOptInfo* rel,
    ForeignPath* path,
    bool has_limit,
    Oid relid,
    const char* column,
    bool qbit,
    chfdw_vector_metric metric,
    bool index_asc,
    const char** index_name,
    bool* warn
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)rel->fdw_private;
    UserMapping* user         = chfdw_rel_user_mapping(root, rel);
    PathKey* first            = linitial(root->query_pathkeys);
#if PG_VERSION_NUM >= 180000
    bool asc = first->pk_cmptype == BTLessStrategyNumber;
#else
    bool asc = first->pk_strategy == BTLessStrategyNumber;
#endif

    if (path->path.pathkeys == NIL) {
        return "the ORDER BY cannot be pushed down, so Postgres sorts every row";
    }
    if (qbit) {
        return "QBit columns cannot have vector indexes";
    }
    if (!user) {
        return NULL;
    }

    /* Look up the indexes first: that survives an unreachable server */
    const VectorTableMeta* meta = chfdw_vector_table_meta(relid, user);

    if (!meta) {
        return NULL;
    }
    if (!chfdw_version_ge(chfdw_get_server_version(user), 25, 8)) {
        *warn = !list_member_oid(warned_old_servers, user->serverid);
        if (*warn) {
            MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

            warned_old_servers = lappend_oid(warned_old_servers, user->serverid);
            MemoryContextSwitchTo(old);
        }
        return "vector similarity indexes require ClickHouse 25.8 or later";
    }
    if (!has_limit) {
        return fpinfo->local_conds
                   ? "a WHERE condition cannot be pushed down, so neither can LIMIT"
                   : "the query has no LIMIT";
    }
    if (list_length(path->path.pathkeys) > 1) {
        return "the ORDER BY has more than one sort key";
    }
    if (asc != index_asc) {
        return "the ORDER BY sorts farthest first";
    }

    const VectorIndexInfo* other = NULL;
    ListCell* lc;
    foreach (lc, meta->indexes) {
        const VectorIndexInfo* index = lfirst(lc);

        if (strcmp(index->column, column) != 0) {
            continue;
        }
        if (index->metric == metric) {
            *index_name  = index->name;
            int64 limit  = const_count(root->parse->limitCount);
            int64 offset = const_count(root->parse->limitOffset);

            if (meta->limit_cap >= 0 && limit >= 0 && offset >= 0 &&
                limit + offset > meta->limit_cap) {
                return psprintf(
                    "LIMIT " INT64_FORMAT
                    " exceeds max_limit_for_vector_search_queries (" INT64_FORMAT ")",
                    limit + offset,
                    meta->limit_cap
                );
            }
            return NULL;
        }
        other = other ? other : index;
    }
    if (other) {
        return psprintf(
            "index \"%s\" uses %s (%s), not %s",
            other->name,
            metrics[other->metric].func,
            metrics[other->metric].op,
            metrics[metric].func
        );
    }
    return psprintf("column \"%s\" has no vector index", column);
}

/*
 * Classify the vector search of a foreign scan, if any, and warn when
 * ClickHouse cannot serve it from a vector index. Returns what EXPLAIN shows
 * as a List of String nodes for the scan's fdw_private: the distance
 * function, column, index, and fallback reason. Returns NIL when the scan
 * does not sort by a vector distance.
 */
List*
chfdw_vector_plan_info(
    PlannerInfo* root,
    RelOptInfo* rel,
    ForeignPath* path,
    bool has_limit
) {
    chfdw_vector_metric metric;
    bool index_asc;
    Var* var;

    if (!IS_SIMPLE_REL(rel) || root->query_pathkeys == NIL) {
        return NIL;
    }

    PathKey* first = linitial(root->query_pathkeys);
    Expr* expr     = chfdw_find_em_expr_for_rel(first->pk_eclass, rel);

    if (!expr || !vector_distance(expr, rel->relid, &metric, &index_asc, &var)) {
        return NIL;
    }

    Oid relid          = planner_rt_fetch(rel->relid, root)->relid;
    const char* column = chfdw_get_custom_column_info(relid, var->varattno)->colname;
    const char* index  = "";
    bool warn          = true;
    bool dot;
    bool qbit          = chfdw_qbit_bits(root, var, &dot) > 0;
    const char* reason = fallback_reason(
        root,
        rel,
        path,
        has_limit,
        relid,
        column,
        qbit,
        metric,
        index_asc,
        &index,
        &warn
    );

    /* A pushed down distance on a QBit column uses the transposed function */
    const char* func = metrics[metric].func;

    if (qbit && path->path.pathkeys != NIL) {
        func = psprintf("%sTransposed", func);
    }

    if (reason && warn && chfdw_vector_planner_warnings()) {
        ereport(
            WARNING,
            errmsg(
                "pg_clickhouse: vector search on \"%s\" will not use a ClickHouse "
                "vector index",
                get_rel_name(relid)
            ),
            errdetail_internal(reason),
            errhint(
                "set pg_clickhouse.vector_planner_warnings = off to hide this warning."
            )
        );
    }

    return list_make4(
        makeString(pstrdup(func)),
        makeString(pstrdup(column)),
        makeString(pstrdup(index)),
        makeString(pstrdup(reason ? reason : ""))
    );
}

/*
 * Show the vector search classification of a scan in EXPLAIN.
 */
void
chfdw_explain_vector_info(List* info, struct ExplainState* es) {
    if (info == NIL) {
        return;
    }

    const char* index  = strVal(lthird(info));
    const char* reason = strVal(lfourth(info));

    ExplainPropertyText(
        "Vector Search",
        psprintf("%s on %s", strVal(linitial(info)), strVal(lsecond(info))),
        es
    );
    ExplainPropertyText("Vector Index", *index ? index : "none", es);
    if (*reason) {
        ExplainPropertyText("Vector Fallback", reason, es);
    }
}

/*
 * Show ClickHouse's EXPLAIN indexes = 1 for the remote query of a scan, which
 * reports whether a vector index serves it. Parameterized queries cannot be
 * explained without their values.
 */
void
chfdw_explain_remote_plan(ForeignScan* plan, const char* sql, struct ExplainState* es) {
    if (plan->fdw_exprs) {
        ExplainPropertyText("Remote Plan", "unavailable for parameterized queries", es);
        return;
    }

    UserMapping* user = GetUserMapping(GetUserId(), plan->fs_server);
    List* rows = chfdw_run_text_query(user, psprintf("EXPLAIN indexes = 1 %s", sql), 1);
    List* lines = NIL;
    ListCell* lc;

    foreach (lc, rows) {
        lines = lappend(lines, ((char**)lfirst(lc))[0]);
    }
    if (es->format != EXPLAIN_FORMAT_TEXT) {
        ExplainPropertyList("Remote Plan", lines, es);
        return;
    }
    /* ExplainIndentText() is not exported before Postgres 18 */
    appendStringInfoSpaces(es->str, es->indent * 2);
    appendStringInfoString(es->str, "Remote Plan:\n");
    es->indent++;
    foreach (lc, lines) {
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfo(es->str, "%s\n", (char*)lfirst(lc));
    }
    es->indent--;
}
