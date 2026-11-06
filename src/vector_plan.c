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
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#endif
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"

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
    case CF_VECTOR_NEG_IP:
        /* Ascending <#> deparses to dotProduct DESC */
        *metric = VECTOR_METRIC_DOT;
        break;
    case CF_VECTOR_DOT:
        *metric    = VECTOR_METRIC_DOT;
        *index_asc = false;
        break;
    case CF_OPERATOR_FUNC:
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
 * Return why query shape prevents vector index use, or NULL when structural
 * checks pass.
 */
static const char*
shape_reason(
    PlannerInfo* root,
    RelOptInfo* rel,
    ForeignPath* path,
    bool has_limit,
    bool index_asc
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)rel->fdw_private;
    PathKey* first            = linitial(root->query_pathkeys);
#if PG_VERSION_NUM >= 180000
    bool asc = first->pk_cmptype == BTLessStrategyNumber;
#else
    bool asc = first->pk_strategy == BTLessStrategyNumber;
#endif

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
    return NULL;
}

/*
 * Return the reason why ClickHouse cannot serve vector search from an index,
 * or NULL when it can, in which case it also sets *index_name to the index it
 * would use.
 */
static const char*
index_reason(
    const VectorTableMeta* meta,
    const char* func,
    const char* column,
    int64 limit,
    const char** index_name
) {
    const VectorIndexInfo* other = NULL;
    ListCell* lc;

    foreach (lc, meta->indexes) {
        const VectorIndexInfo* index = lfirst(lc);

        if (strcmp(index->column, column) != 0) {
            continue;
        }
        if (strcmp(metrics[index->metric].func, func) == 0) {
            *index_name = index->name;
            if (meta->limit_cap >= 0 && limit > meta->limit_cap) {
                return psprintf(
                    "LIMIT " INT64_FORMAT
                    " exceeds max_limit_for_vector_search_queries (" INT64_FORMAT ")",
                    limit,
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
            func
        );
    }
    return psprintf("column \"%s\" has no vector index", column);
}

/*
 * Classify the vector search for a foreign scan, if any, and warn when its
 * shape keeps ClickHouse from serving it from a vector index. Returns what
 * EXPLAIN needs as a List for the scan's fdw_private: the distance function,
 * column, fallback reason, whether the sort is pushed down, and LIMIT plus
 * OFFSET or -1. Returns NIL when the scan does not sort by a vector distance.
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

    Oid relid               = planner_rt_fetch(rel->relid, root)->relid;
    CustomColumnInfo* cinfo = chfdw_get_custom_column_info(relid, var->varattno);
    const char* column      = cinfo && cinfo->colname
                                  ? cinfo->colname
                                  : get_attname(relid, var->varattno, false);
    bool pushed             = path->path.pathkeys != NIL;
    const char* reason =
        pushed ? shape_reason(root, rel, path, has_limit, index_asc)
               : "the ORDER BY cannot be pushed down, so Postgres sorts every row";

    if (reason && chfdw_vector_planner_warnings()) {
        ereport(
            WARNING,
            errmsg(
                "pg_clickhouse: vector search on \"%s\" will not use a ClickHouse "
                "vector index",
                get_rel_name(relid)
            ),
            errdetail_internal("%s", reason),
            errhint(
                "set pg_clickhouse.vector_planner_warnings = off to hide this warning."
            )
        );
    }

    int64 limit  = const_count(root->parse->limitCount);
    int64 offset = const_count(root->parse->limitOffset);

    return list_make5(
        makeString(pstrdup(metrics[metric].func)),
        makeString(pstrdup(column)),
        makeString(pstrdup(reason ? reason : "")),
        makeInteger(pushed),
        makeString(
            psprintf(INT64_FORMAT, limit >= 0 && offset >= 0 ? limit + offset : -1)
        )
    );
}

/*
 * Add vector search and index properties to EXPLAIN, reading the indexes of
 * foreign table relid from ClickHouse when the query shape allows an index.
 * `info` must be a list generated by `chfdw_vector_plan_info()` (or NIL).
 */
void
chfdw_explain_vector_info(
    List* info,
    Oid relid,
    UserMapping* user,
    struct ExplainState* es
) {
    if (info == NIL) {
        /* Not a vector search. */
        return;
    }

    /* Read data generated by chfdw_vector_plan_info(). */
    const char* func   = strVal(linitial(info));
    const char* column = strVal(lsecond(info));
    const char* reason = strVal(lthird(info));
    const char* index  = NULL;
    VectorTableMeta meta;

    /* Fourth item of info indicates whether the `ORDER BY` was pushed down. */
    if (intVal(lfourth(info)) && chfdw_vector_table_meta(relid, user, &meta)) {
        if (meta.old_server) {
            reason = "vector similarity indexes require ClickHouse 25.8 or later";
        } else if (!*reason) {
            int64 limit = strtoll(strVal(list_nth(info, 4)), NULL, 10);

            reason = index_reason(&meta, func, column, limit, &index);
        }
    }

    ExplainPropertyText("Vector Search", psprintf("%s on %s", func, column), es);
    ExplainPropertyText("Vector Index", index ? index : "none", es);
    if (reason && *reason) {
        ExplainPropertyText("Vector Index Not Used", reason, es);
    }
}
