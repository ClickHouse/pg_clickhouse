/*-------------------------------------------------------------------------
 *
 * setop.c
 *		  Planner helpers for set-operation pushdown
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/setop.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "setop.h"

#include "nodes/nodeFuncs.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"

typedef struct ChFdwSetopPathState {
    ChFdwSetopPathValidator validator;
    void* validator_arg;
    ChFdwSetopPathInfo path_info;
} ChFdwSetopPathState;

static bool
chfdw_setop_extract_path_member(Path* path, ChFdwSetopPathState* state);
static bool
chfdw_setop_extract_plan_member(
    Plan* plan,
    List* expected_tlist,
    Oid expected_serverid,
    List** foreign_scans
);

static bool
chfdw_setop_path_has_no_parameters(const Path* path) {
    return path->param_info == NULL && !path->parallel_aware;
}

static bool
chfdw_setop_plan_has_no_parameters(const Plan* plan) {
    return plan->initPlan == NIL && bms_is_empty(plan->extParam) &&
           bms_is_empty(plan->allParam) && !plan->parallel_aware;
}

static bool
chfdw_setop_path_targets_equal(const PathTarget* left, const PathTarget* right) {
    return left != NULL && right != NULL && equal(left->exprs, right->exprs);
}

static bool
chfdw_setop_expr_signatures_equal(const Node* left, const Node* right) {
    return exprType(left) == exprType(right) && exprTypmod(left) == exprTypmod(right) &&
           exprCollation(left) == exprCollation(right);
}

static bool
chfdw_setop_path_signatures_equal(const Path* left, const Path* right) {
    ListCell* left_lc;
    ListCell* right_lc;

    if (left->pathtarget == NULL || right->pathtarget == NULL ||
        list_length(left->pathtarget->exprs) != list_length(right->pathtarget->exprs)) {
        return false;
    }

    forboth(left_lc, left->pathtarget->exprs, right_lc, right->pathtarget->exprs) {
        if (!chfdw_setop_expr_signatures_equal(
                (Node*)lfirst(left_lc), (Node*)lfirst(right_lc)
            )) {
            return false;
        }
    }

    return true;
}

static bool
chfdw_setop_tlist_signatures_equal(const List* left, const List* right) {
    ListCell* left_lc;
    ListCell* right_lc;
    List* left_exprs  = NIL;
    List* right_exprs = NIL;

    foreach (left_lc, left) {
        TargetEntry* tle = lfirst_node(TargetEntry, left_lc);

        if (!tle->resjunk) {
            left_exprs = lappend(left_exprs, tle->expr);
        }
    }

    foreach (right_lc, right) {
        TargetEntry* tle = lfirst_node(TargetEntry, right_lc);

        if (!tle->resjunk) {
            right_exprs = lappend(right_exprs, tle->expr);
        }
    }

    if (list_length(left_exprs) != list_length(right_exprs)) {
        return false;
    }

    forboth(left_lc, left_exprs, right_lc, right_exprs) {
        if (!chfdw_setop_expr_signatures_equal(
                (Node*)lfirst(left_lc), (Node*)lfirst(right_lc)
            )) {
            return false;
        }
    }

    return true;
}

static bool
chfdw_setop_tlists_equal(const List* left, const List* right) {
    ListCell* left_lc;
    ListCell* right_lc;

    if (list_length(left) != list_length(right)) {
        return false;
    }

    forboth(left_lc, left, right_lc, right) {
        TargetEntry* left_tle  = lfirst_node(TargetEntry, left_lc);
        TargetEntry* right_tle = lfirst_node(TargetEntry, right_lc);

        if (left_tle->resjunk != right_tle->resjunk ||
            !equal(left_tle->expr, right_tle->expr)) {
            return false;
        }
    }

    return true;
}

static bool
chfdw_setop_subquery_path_is_trivial(const SubqueryScanPath* path) {
    RelOptInfo* rel = path->path.parent;
    ListCell* outer_lc;
    bool identity     = true;
    int output_column = 0;

    if (rel == NULL ||
        (rel->reloptkind != RELOPT_BASEREL &&
         rel->reloptkind != RELOPT_OTHER_MEMBER_REL) ||
        rel->rtekind != RTE_SUBQUERY || rel->baserestrictinfo != NIL ||
        !bms_is_empty(rel->lateral_relids) || rel->subplan_params != NIL ||
        path->subpath == NULL ||
        list_length(path->path.pathtarget->exprs) !=
            list_length(path->subpath->pathtarget->exprs)) {
        return false;
    }

    foreach (outer_lc, path->path.pathtarget->exprs) {
        Node* outer_expr = (Node*)lfirst(outer_lc);
        Node* inner_expr;
        Var* var;

        output_column++;
        if (!IsA(outer_expr, Var)) {
            return false;
        }

        var = (Var*)outer_expr;
        if ((int64)var->varno != (int64)rel->relid || var->varattno <= 0 ||
            var->varattno > list_length(path->subpath->pathtarget->exprs) ||
            var->varlevelsup != 0) {
            return false;
        }
        inner_expr =
            (Node*)list_nth(path->subpath->pathtarget->exprs, var->varattno - 1);
        if (!chfdw_setop_expr_signatures_equal(outer_expr, inner_expr)) {
            return false;
        }

        if (var->varattno != output_column) {
            identity = false;
        }
    }

    return identity || IsA(path->subpath, ForeignPath);
}

static bool
chfdw_setop_subquery_plan_is_trivial(const SubqueryScan* scan) {
    ListCell* outer_lc;
    List* outer_tlist;
    List* inner_tlist;

    if (scan->subplan == NULL || scan->scan.plan.qual != NIL ||
        !chfdw_setop_plan_has_no_parameters(&scan->scan.plan)) {
        return false;
    }

    outer_tlist = scan->scan.plan.targetlist;
    inner_tlist = scan->subplan->targetlist;

    if (list_length(outer_tlist) != list_length(inner_tlist)) {
        return false;
    }

    foreach (outer_lc, outer_tlist) {
        TargetEntry* outer_tle = lfirst_node(TargetEntry, outer_lc);
        TargetEntry* inner_tle;
        Var* var;

        if (!IsA(outer_tle->expr, Var)) {
            return false;
        }

        var = (Var*)outer_tle->expr;
        if ((int64)var->varno != (int64)scan->scan.scanrelid || var->varattno <= 0 ||
            var->varlevelsup != 0) {
            return false;
        }
        inner_tle = get_tle_by_resno(inner_tlist, var->varattno);
        if (inner_tle == NULL || outer_tle->resjunk != inner_tle->resjunk ||
            var->varlevelsup != 0 ||
            !chfdw_setop_expr_signatures_equal(
                (Node*)outer_tle->expr, (Node*)inner_tle->expr
            )) {
            return false;
        }
    }

    return true;
}

static bool
chfdw_setop_fdw_routines_equal(const FdwRoutine* left, const FdwRoutine* right) {
    return left == right || (left != NULL && right != NULL &&
                             left->GetForeignPlan == right->GetForeignPlan &&
                             left->BeginForeignScan == right->BeginForeignScan &&
                             left->IterateForeignScan == right->IterateForeignScan);
}

AppendPath*
chfdw_setop_find_append_path(Path* path, bool* is_distinct) {
    Path* subpath;

    if (is_distinct == NULL || path == NULL ||
        !chfdw_setop_path_has_no_parameters(path)) {
        return NULL;
    }

    *is_distinct = false;

    if (IsA(path, AppendPath)) {
        return (AppendPath*)path;
    }

    if (IsA(path, AggPath)) {
        AggPath* agg_path = (AggPath*)path;

        if (agg_path->aggstrategy != AGG_HASHED ||
            agg_path->aggsplit != AGGSPLIT_SIMPLE || agg_path->qual != NIL ||
            list_length(agg_path->groupClause) !=
                list_length(path->pathtarget->exprs)) {
            return NULL;
        }

        subpath = agg_path->subpath;
    }
#if PG_VERSION_NUM >= 190000
    else if (IsA(path, UniquePath)) {
        UniquePath* unique_path = (UniquePath*)path;

        if (unique_path->numkeys != list_length(path->pathkeys)) {
            return NULL;
        }

        subpath = unique_path->subpath;
#else
    else if (IsA(path, UpperUniquePath)) {
        UpperUniquePath* unique_path = (UpperUniquePath*)path;

        if (unique_path->numkeys != list_length(path->pathkeys)) {
            return NULL;
        }

        subpath = unique_path->subpath;
#endif
    } else {
        return NULL;
    }

    if (IsA(subpath, SortPath)) {
        SortPath* sort_path = (SortPath*)subpath;

        if (!chfdw_setop_path_has_no_parameters(subpath) ||
            !chfdw_setop_path_targets_equal(
                subpath->pathtarget, sort_path->subpath->pathtarget
            )) {
            return NULL;
        }

        subpath = sort_path->subpath;
    }

    if (!IsA(subpath, AppendPath)) {
        return NULL;
    }

    *is_distinct = true;
    return (AppendPath*)subpath;
}

static bool
chfdw_setop_extract_append_path(AppendPath* append_path, ChFdwSetopPathState* state) {
    ListCell* lc;
    int path_count;

    path_count = list_length(append_path->subpaths);
    if (path_count < 2 || IS_PARTITIONED_REL(append_path->path.parent) ||
        append_path->first_partial_path != path_count ||
        append_path->limit_tuples >= 0 ||
        !chfdw_setop_path_has_no_parameters(&append_path->path)) {
        return false;
    }

    foreach (lc, append_path->subpaths) {
        Path* subpath = (Path*)lfirst(lc);

        if (!chfdw_setop_path_signatures_equal(&append_path->path, subpath) ||
            !chfdw_setop_extract_path_member(subpath, state)) {
            return false;
        }
    }

    return true;
}

static bool
chfdw_setop_extract_foreign_path(
    ForeignPath* foreign_path,
    ChFdwSetopPathState* state
) {
    RelOptInfo* rel               = foreign_path->path.parent;
    ChFdwSetopPathInfo* path_info = &state->path_info;

    if (rel == NULL || rel->fdwroutine == NULL || !OidIsValid(rel->serverid) ||
        !chfdw_setop_path_has_no_parameters(&foreign_path->path)
#if PG_VERSION_NUM >= 170000
        || foreign_path->fdw_restrictinfo != NIL
#endif
        || !state->validator(foreign_path, state->validator_arg)) {
        return false;
    }

    if (path_info->foreign_paths == NIL) {
        path_info->fdwroutine      = rel->fdwroutine;
        path_info->serverid        = rel->serverid;
        path_info->userid          = rel->userid;
        path_info->useridiscurrent = rel->useridiscurrent;
    } else if (
        path_info->serverid != rel->serverid || path_info->userid != rel->userid ||
        path_info->useridiscurrent != rel->useridiscurrent ||
        !chfdw_setop_fdw_routines_equal(path_info->fdwroutine, rel->fdwroutine)
    ) {
        return false;
    }

    path_info->foreign_paths = lappend(path_info->foreign_paths, foreign_path);
    return true;
}

static bool
chfdw_setop_extract_path_member(Path* path, ChFdwSetopPathState* state) {
    if (path == NULL || !chfdw_setop_path_has_no_parameters(path)) {
        return false;
    }

    if (IsA(path, AppendPath)) {
        return chfdw_setop_extract_append_path((AppendPath*)path, state);
    }

    if (IsA(path, MaterialPath)) {
        MaterialPath* material_path = (MaterialPath*)path;

        return material_path->subpath != NULL &&
               chfdw_setop_path_targets_equal(
                   path->pathtarget, material_path->subpath->pathtarget
               ) &&
               chfdw_setop_extract_path_member(material_path->subpath, state);
    }

    if (IsA(path, ProjectionPath)) {
        ProjectionPath* projection_path = (ProjectionPath*)path;

        return projection_path->subpath != NULL &&
               chfdw_setop_path_targets_equal(
                   path->pathtarget, projection_path->subpath->pathtarget
               ) &&
               chfdw_setop_extract_path_member(projection_path->subpath, state);
    }

    if (IsA(path, SubqueryScanPath)) {
        SubqueryScanPath* subquery_path = (SubqueryScanPath*)path;

        return chfdw_setop_subquery_path_is_trivial(subquery_path) &&
               chfdw_setop_extract_path_member(subquery_path->subpath, state);
    }

    if (IsA(path, ForeignPath)) {
        return chfdw_setop_extract_foreign_path((ForeignPath*)path, state);
    }

    return false;
}

bool
chfdw_setop_extract_foreign_paths(
    AppendPath* append_path,
    ChFdwSetopPathValidator validator,
    void* validator_arg,
    ChFdwSetopPathInfo* path_info
) {
    ChFdwSetopPathState state;

    if (append_path == NULL || validator == NULL || path_info == NULL) {
        return false;
    }

    memset(&state, 0, sizeof(state));
    state.validator     = validator;
    state.validator_arg = validator_arg;

    if (!chfdw_setop_extract_append_path(append_path, &state) ||
        list_length(state.path_info.foreign_paths) < 2) {
        return false;
    }

    *path_info = state.path_info;
    return true;
}

static bool
chfdw_setop_extract_append_plan(
    Append* append,
    Oid expected_serverid,
    List** foreign_scans
) {
    ListCell* lc;

    if (
        list_length(append->appendplans) < 2 ||
        append->first_partial_plan != list_length(append->appendplans) ||
        append->plan.qual != NIL || append->plan.lefttree != NULL ||
        append->plan.righttree != NULL ||
        !chfdw_setop_plan_has_no_parameters(&append->plan)
#if PG_VERSION_NUM >= 180000
        || append->part_prune_index >= 0
#else
        || append->part_prune_info != NULL
#endif
    ) {
        return false;
    }

    foreach (lc, append->appendplans) {
        Plan* subplan = lfirst_node(Plan, lc);

        if (!chfdw_setop_tlist_signatures_equal(
                append->plan.targetlist, subplan->targetlist
            ) ||
            !chfdw_setop_extract_plan_member(
                subplan, append->plan.targetlist, expected_serverid, foreign_scans
            )) {
            return false;
        }
    }

    return true;
}

static bool
chfdw_setop_extract_foreign_scan(
    ForeignScan* foreign_scan,
    Oid expected_serverid,
    List** foreign_scans
) {
    Plan* plan = &foreign_scan->scan.plan;

    if (foreign_scan->operation != CMD_SELECT ||
        foreign_scan->fs_server != expected_serverid || plan->qual != NIL ||
        plan->lefttree != NULL || plan->righttree != NULL ||
        !chfdw_setop_plan_has_no_parameters(plan) || foreign_scan->fdw_private == NIL) {
        return false;
    }

    *foreign_scans = lappend(*foreign_scans, foreign_scan);
    return true;
}

static bool
chfdw_setop_extract_plan_member(
    Plan* plan,
    List* expected_tlist,
    Oid expected_serverid,
    List** foreign_scans
) {
    if (plan == NULL || !chfdw_setop_plan_has_no_parameters(plan) ||
        !chfdw_setop_tlist_signatures_equal(expected_tlist, plan->targetlist)) {
        return false;
    }

    if (IsA(plan, Append)) {
        return chfdw_setop_extract_append_plan(
            (Append*)plan, expected_serverid, foreign_scans
        );
    }

    if (IsA(plan, Material)) {
        return plan->qual == NIL && plan->lefttree != NULL && plan->righttree == NULL &&
               chfdw_setop_tlists_equal(plan->targetlist, plan->lefttree->targetlist) &&
               chfdw_setop_extract_plan_member(
                   plan->lefttree, plan->targetlist, expected_serverid, foreign_scans
               );
    }

    if (IsA(plan, Result)) {
        Result* result = (Result*)plan;

        return result->resconstantqual == NULL && plan->qual == NIL &&
               plan->lefttree != NULL && plan->righttree == NULL &&
               chfdw_setop_tlists_equal(plan->targetlist, plan->lefttree->targetlist) &&
               chfdw_setop_extract_plan_member(
                   plan->lefttree, plan->targetlist, expected_serverid, foreign_scans
               );
    }

    if (IsA(plan, SubqueryScan)) {
        SubqueryScan* subquery_scan = (SubqueryScan*)plan;
        List* projected_tlist       = NIL;
        ListCell* lc;

        if (!chfdw_setop_subquery_plan_is_trivial(subquery_scan)) {
            return false;
        }
        foreach (lc, subquery_scan->scan.plan.targetlist) {
            TargetEntry* outer_tle = lfirst_node(TargetEntry, lc);
            Var* var               = castNode(Var, outer_tle->expr);
            TargetEntry* inner_tle =
                get_tle_by_resno(subquery_scan->subplan->targetlist, var->varattno);
            TargetEntry* projected_tle = copyObject(inner_tle);

            projected_tle->resno = list_length(projected_tlist) + 1;
            projected_tle->resname =
                outer_tle->resname ? pstrdup(outer_tle->resname) : NULL;
            projected_tle->resjunk = outer_tle->resjunk;
            projected_tlist        = lappend(projected_tlist, projected_tle);
        }
        /*
         * This is the private outer plan built only for this ForeignPath.
         * The combined ForeignScan replaces it, so normalize the throwaway
         * child target list in place to retain the SubqueryScan projection.
         */
        subquery_scan->subplan->targetlist = projected_tlist;
        return chfdw_setop_extract_plan_member(
            subquery_scan->subplan, projected_tlist, expected_serverid, foreign_scans
        );
    }

    if (IsA(plan, ForeignScan)) {
        return chfdw_setop_extract_foreign_scan(
            (ForeignScan*)plan, expected_serverid, foreign_scans
        );
    }

    return false;
}

bool
chfdw_setop_extract_foreign_scans(
    Plan* plan,
    Oid expected_serverid,
    List** foreign_scans
) {
    List* scans = NIL;

    if (plan == NULL || !OidIsValid(expected_serverid) || foreign_scans == NULL ||
        !IsA(plan, Append) ||
        !chfdw_setop_extract_append_plan((Append*)plan, expected_serverid, &scans) ||
        list_length(scans) < 2) {
        return false;
    }

    *foreign_scans = scans;
    return true;
}
