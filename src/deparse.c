/*-------------------------------------------------------------------------
 *
 * deparse.c
 *		  Query deparser for pg_clickhouse
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/deparse.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/primnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/tlist.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "regex/regex.h"
#include "utils/array.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include "fdw.h"

/*
 * Prior to PostgreSQL 14 these fmgroids had different names or were
 * not generated.  Window functions used F_WINDOW_ prefix; aggregate
 * OIDs were not emitted at all so must use raw values.
 */
#if PG_VERSION_NUM < 140000
#define F_ROW_NUMBER F_WINDOW_ROW_NUMBER
#define F_RANK_ F_WINDOW_RANK
#define F_DENSE_RANK_ F_WINDOW_DENSE_RANK
#define F_PERCENT_RANK_ F_WINDOW_PERCENT_RANK
#define F_CUME_DIST_ F_WINDOW_CUME_DIST
#define F_NTILE F_WINDOW_NTILE
#define F_BOOL_AND 2517
#define F_BOOL_OR 2518
#define F_EVERY 2519
#define F_STRING_AGG_TEXT_TEXT 3538
#define F_STRING_AGG_BYTEA_BYTEA 3545
#define F_REGR_COUNT 2818
#define F_REGR_SXX 2819
#define F_REGR_SYY 2820
#define F_REGR_SXY 2821
#define F_REGR_AVGX 2822
#define F_REGR_AVGY 2823
#define F_REGR_R2 2824
#define F_REGR_SLOPE 2825
#define F_REGR_INTERCEPT 2826
#endif

#ifndef MAXINT8LEN
#define MAXINT8LEN 25
#endif

/* Aggregate OIDs absent from fmgroids.h on all PG versions. */
#define F_STRING_AGG_TEXT_TEXT 3538

/* Oft-used syntax to quote quote the session time zone literal string. */
#define QUOTED_TZ ch_quote_literal(pg_get_timezone_name(session_timezone))

/* variable counter */
static uint32 var_counter = 0;

/*
 * Global context for foreign_expr_walker's search of an expression tree.
 */
typedef struct foreign_glob_cxt {
    PlannerInfo* root;      /* global planner state */
    RelOptInfo* foreignrel; /* the foreign relation we are planning for */
    Relids relids;          /* relids of base relations in the underlying
                             * scan */
    bool subquery_scope;    /* true while walking the interior of a
                             * SubPlan's Query; relaxes upper-rel-only
                             * checks (e.g. bare aggregates are legal in a
                             * scalar subquery) and forbids nesting */
} foreign_glob_cxt;

/*
 * Context for deparseExpr
 */
typedef struct deparse_expr_cxt {
    PlannerInfo* root;         /* global planner state */
    RelOptInfo* foreignrel;    /* the foreign relation we are planning for */
    RelOptInfo* scanrel;       /* the underlying scan relation. Same as
                                * foreignrel, when that represents a join or
                                * a base relation. */
    StringInfo buf;            /* output buffer to append to */
    List** params_list;        /* exprs that will become remote Params */
    CustomObjectDef* func;     /* custom function deparse */
    CHFdwRelationInfo* fpinfo; /* fdw relation info */
    bool interval_op;
    bool array_as_tuple; /* determines array output format */
    bool no_sort_parens; /* determines sort group clause format */

    /*
     * True when the statement being deparsed inlines at least one SubPlan.
     * Computed once up front (a contain_subplans scan over the tlist/quals)
     * before any emission begins, and never propagated between contexts.
     * Forces r{N} aliasing even for single-table scans: an unqualified outer
     * column inlined into a subquery would be captured by the subquery's own
     * tables (innermost-scope resolution), silently changing the semantics.
     */
    bool has_inlined_subplan;

    /*
     * SubPlan scope. While deparsing the body of a pushed-down SubPlan,
     * subplan points at it and root points at the SubPlan's own PlannerInfo
     * (so planner_rt_fetch resolves varnos against the subquery's rtable).
     * parent_ctx chains to the enclosing scope so correlation Params can be
     * deparsed with the outer query's aliases. Both are NULL at top level.
     */
    SubPlan* subplan;
    struct deparse_expr_cxt* parent_ctx;
} deparse_expr_cxt;

#define REL_ALIAS_PREFIX "r"
/* Handy macro to add relation name qualification */
#define ADD_REL_QUALIFIER(buf, varno)                                                  \
    appendStringInfo((buf), "%s%d.", REL_ALIAS_PREFIX, (varno))
#define SUBQUERY_REL_ALIAS_PREFIX "s"
#define SUBQUERY_COL_ALIAS_PREFIX "c"

/*
 * SubPlan-interior aliases. plan_id is unique across PlannedStmt.subplans,
 * so q{plan_id}_{varno} can never collide with the outer query's r{N}/s{N}
 * aliases, nor with a sibling SubPlan's tables. See deparseSubPlanQuery.
 *
 * INVARIANT (scope discipline): every alias qualifier emitted anywhere in
 * this file must come from exactly one of these namespaces, chosen by the
 * deparse_expr_cxt that is current at the emit site:
 *   context->subplan == NULL  ->  r{N} / s{N}.c{M}   (outer scope)
 *   context->subplan != NULL  ->  q{plan_id}_{N}     (SubPlan scope)
 * Emit sites assert this so a scope leak fails loudly in test builds.
 */
#define SUBPLAN_REL_ALIAS_PREFIX "q"
#define ADD_SUBPLAN_REL_QUALIFIER(buf, plan_id, varno)                                 \
    appendStringInfo((buf), "%s%d_%d.", SUBPLAN_REL_ALIAS_PREFIX, (plan_id), (varno))

#define CSTRING_TOLOWER(str)                                                           \
    do {                                                                               \
        for (int i = 0; str[i]; i++) {                                                 \
            str[i] = tolower((unsigned char)str[i]);                                   \
        }                                                                              \
    } while (0)

/*
 * How an expression's result is consumed, for the IN-family NULL-semantics
 * rules (see the block comment for deparseScalarArrayOpExpr).
 *
 * EXPR_CTX_TRUTH    only qualifies rows: NULL and FALSE are interchangeable.
 * EXPR_CTX_NEGATED  the direct operand of one NOT whose own result is in a
 *                   truth context. Only a SubPlan consumes this state (it
 *                   admits the guarded NOT IN deparse); every other node
 *                   degrades it to EXPR_CTX_EXACT for its children, so the
 *                   walker and the deparse peek in deparseBoolExpr agree on
 *                   exactly which SubPlans get the guarded form.
 * EXPR_CTX_EXACT    the value itself is observable: exact three-valued
 *                   equivalence required.
 *
 * NOTE: deparseExpr's own switch on nodeTag degrades its analogous
 * deparse_expr_cxt.truth_ctx to false before dispatch for every node type
 * except T_BoolExpr/T_CaseExpr/T_RelabelType/T_ScalarArrayOpExpr -- the same
 * types this walker does something other than force EXPR_CTX_EXACT for. If
 * you add a type here that preserves ctx for its children, add it there too.
 */
typedef enum ExprTruthCtx {
    EXPR_CTX_TRUTH,
    EXPR_CTX_NEGATED,
    EXPR_CTX_EXACT,
} ExprTruthCtx;

typedef enum JsonbDocumentKind {
    JSONB_DOCUMENT_UNSUPPORTED,
    JSONB_DOCUMENT_NATIVE,
    JSONB_DOCUMENT_STRING,
} JsonbDocumentKind;

/*
 * Functions to determine whether an expression can be evaluated safely on
 * remote server.
 */
static bool
foreign_expr_walker(Node* node, foreign_glob_cxt* glob_cxt, ExprTruthCtx ctx);
static bool
is_shippable_subplan(SubPlan* subplan, foreign_glob_cxt* glob_cxt, ExprTruthCtx ctx);
static char*
deparse_type_name(Oid type_oid, int32 typemod);

/*
 * Functions to construct string representation of a node tree.
 */
static void
deparseTargetList(
    StringInfo buf,
    RangeTblEntry* rte,
    Index rtindex,
    Relation rel,
    Bitmapset* attrs_used,
    bool qualify_col,
    List** retrieved_attrs
);
static void
deparseExplicitTargetList(
    List* tlist,
    List** retrieved_attrs,
    deparse_expr_cxt* context
);
static void
deparseSubqueryTargetList(deparse_expr_cxt* context);
static void
deparseColumnRef(
    StringInfo buf,
    CustomObjectDef* cdef,
    int varno,
    int varattno,
    RangeTblEntry* rte,
    bool qualify_col
);
static void
deparseRelation(StringInfo buf, Relation rel);
static void
deparseExpr(Expr* expr, deparse_expr_cxt* context);
static void
deparseVar(Var* node, deparse_expr_cxt* context);
static void
deparseConst(Const* node, deparse_expr_cxt* context, int showtype);
static void
deparseParam(Param* node, deparse_expr_cxt* context);
static void
deparseSubscriptingRef(SubscriptingRef* node, deparse_expr_cxt* context);
static void
deparseFuncExpr(FuncExpr* node, deparse_expr_cxt* context);
static void
deparseSQLValueFunction(SQLValueFunction* node, deparse_expr_cxt* context);
static void
deparseOpExpr(OpExpr* node, deparse_expr_cxt* context);
static void
deparseJsonbExists(
    Expr* document,
    Expr* key,
    JsonbDocumentKind document_kind,
    deparse_expr_cxt* context
);
static void
deparseOperatorName(StringInfo buf, Form_pg_operator opform);
static void
deparseDistinctExpr(DistinctExpr* node, deparse_expr_cxt* context);
static void
deparseScalarArrayOpExpr(ScalarArrayOpExpr* node, deparse_expr_cxt* context);
static void
deparseCaseExpr(CaseExpr* node, deparse_expr_cxt* context);
static void
deparseCaseWhen(CaseWhen* node, deparse_expr_cxt* context);
static void
deparseRelabelType(RelabelType* node, deparse_expr_cxt* context);
static void
deparseBoolExpr(BoolExpr* node, deparse_expr_cxt* context);
static void
deparseNullTest(NullTest* node, deparse_expr_cxt* context);
static void
deparseArrayExpr(ArrayExpr* node, deparse_expr_cxt* context);
static void
deparseArrayList(ArrayExpr* node, deparse_expr_cxt* context);
static void
printRemoteParam(
    int paramindex,
    Oid paramtype,
    int32 paramtypmod,
    deparse_expr_cxt* context
);
static void
printRemotePlaceholder(Oid paramtype, int32 paramtypmod, deparse_expr_cxt* context);
static void
deparseSelectSql(
    List* tlist,
    bool is_subquery,
    List** retrieved_attrs,
    deparse_expr_cxt* context
);
static void
appendOrderByClause(List* pathkeys, bool has_final_sort, deparse_expr_cxt* context);
static void
appendLimitClause(deparse_expr_cxt* context);
static void
appendConditions(List* exprs, deparse_expr_cxt* context);
static void
deparseFromExprForRel(
    StringInfo buf,
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    bool use_alias,
    Index ignore_rel,
    List** ignore_conds,
    List** params_list
);
static void
deparseFromExpr(List* quals, deparse_expr_cxt* context);
static void
deparseRangeTblRef(
    StringInfo buf,
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    bool make_subquery,
    Index ignore_rel,
    List** ignore_conds,
    List** params_list
);
static void
deparseAggref(Aggref* node, deparse_expr_cxt* context);
static void
deparseWindowFunc(WindowFunc* node, deparse_expr_cxt* context);
static void
appendGroupByClause(List* tlist, deparse_expr_cxt* context);
static CustomObjectDef*
appendFunctionName(Oid funcid, deparse_expr_cxt* context);
static Node*
deparseSortGroupClause(
    Index ref,
    List* tlist,
    bool force_colno,
    deparse_expr_cxt* context
);
static void
deparseCoerceViaIO(CoerceViaIO* node, deparse_expr_cxt* context);
static void
deparseCoalesceExpr(CoalesceExpr* node, deparse_expr_cxt* context);
static void
deparseMinMaxExpr(MinMaxExpr* node, deparse_expr_cxt* context);
static void
deparseRowExpr(RowExpr* node, deparse_expr_cxt* context);
static void
deparseNullIfExpr(NullIfExpr* node, deparse_expr_cxt* context);
static void
appendRegex(List* args, deparse_expr_cxt* context);
/*
 * Which rendering of a SubPlan's subquery body deparseSubPlanQuery emits.
 */
typedef enum SubPlanBodyForm {
    SUBPLAN_BODY_QUERY,      /* the subquery as written */
    SUBPLAN_BODY_NULL_PROBE, /* SELECT 1 .. AND (output IS NULL): the poison
                              * guard of a guarded NOT IN */
} SubPlanBodyForm;

static void
deparseSubPlan(SubPlan* node, deparse_expr_cxt* context);
static void
deparseGuardedNotIn(SubPlan* subplan, deparse_expr_cxt* context);
static void
deparseSubPlanQuery(SubPlan* subplan, deparse_expr_cxt* context, SubPlanBodyForm form);
static void
deparseSubPlanQuals(Node* quals, deparse_expr_cxt* context);
static void
deparseSubPlanTargetList(deparse_expr_cxt* context);
static void
deparseSubPlanFrom(deparse_expr_cxt* context);

/*
 * Helper functions
 */
static bool
is_subquery_var(Var* node, RelOptInfo* foreignrel, int* relno, int* colno);
static void
get_relation_column_alias_ids(
    Var* node,
    RelOptInfo* foreignrel,
    int* relno,
    int* colno
);
static JsonbDocumentKind
classifyJsonbDocument(Expr* expr, Expr** document);

/*
 * Examine each qual clause in input_conds, and classify them into two groups,
 * which are returned as two lists:
 *	- remote_conds contains expressions that can be evaluated remotely
 *	- local_conds contains expressions that can't be evaluated remotely
 */
void
chfdw_classify_conditions(
    PlannerInfo* root,
    RelOptInfo* baserel,
    List* input_conds,
    List** remote_conds,
    List** local_conds
) {
    ListCell* lc;

    *remote_conds = NIL;
    *local_conds  = NIL;

    foreach (lc, input_conds) {
        RestrictInfo* ri = lfirst_node(RestrictInfo, lc);

        if (chfdw_is_foreign_expr(root, baserel, ri->clause, false)) {
            *remote_conds = lappend(*remote_conds, ri);
        } else {
            *local_conds = lappend(*local_conds, ri);
        }
    }
}

/*
 * Returns true if given expr is safe to evaluate on the foreign server.
 *
 * exact_ctx determines how the expression's result is consumed: false for a
 * WHERE/JOIN/HAVING condition, where a NULL qualifies a row exactly like
 * FALSE; true anywhere the computed value itself is observable (target
 * lists, grouping and ordering expressions). The distinction feeds the
 * IN-family NULL-semantics rules; see foreign_expr_walker. Internally the
 * walker refines this into ExprTruthCtx's three states; the third
 * (EXPR_CTX_NEGATED) is derivable only from a NOT encountered during the
 * walk, never assertable by a caller, so the entry point speaks the
 * two-valued form.
 */
bool
chfdw_is_foreign_expr(
    PlannerInfo* root,
    RelOptInfo* baserel,
    Expr* expr,
    bool exact_ctx
) {
    foreign_glob_cxt glob_cxt;
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)(baserel->fdw_private);

    /*
     * Check that the expression consists of nodes that are safe to execute
     * remotely.
     */
    glob_cxt.root           = root;
    glob_cxt.foreignrel     = baserel;
    glob_cxt.subquery_scope = false;

    /*
     * For an upper relation, use relids from its underneath scan relation,
     * because the upperrel's own relids currently aren't set to anything
     * meaningful by the core code. For other relation, use their own relids.
     */
    if (IS_UPPER_REL(baserel)) {
        glob_cxt.relids = fpinfo->outerrel->relids;
    } else {
        glob_cxt.relids = baserel->relids;
    }

    if (!foreign_expr_walker(
            (Node*)expr, &glob_cxt, exact_ctx ? EXPR_CTX_EXACT : EXPR_CTX_TRUTH
        )) {
        return false;
    }

    /* OK to evaluate on the remote server */
    return true;
}

CHEqualOp
chfdw_is_equal_op(Oid opno) {
    Form_pg_operator operform;
    HeapTuple opertup;
    CHEqualOp res = CH_OP_NONE;

    opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
    if (!HeapTupleIsValid(opertup)) {
        elog(ERROR, "cache lookup failed for operator %u", opno);
    }

    operform = (Form_pg_operator)GETSTRUCT(opertup);

    if (NameStr(operform->oprname)[0] == '=' && NameStr(operform->oprname)[1] == '\0') {
        res = CH_OP_EQ;
    } else if (
        NameStr(operform->oprname)[0] == '<' && NameStr(operform->oprname)[1] == '>'
    ) {
        res = CH_OP_NE;
    }

    ReleaseSysCache(opertup);
    return res;
}

/*
 * Classifies how a partial aggregate maps onto ClickHouse.
 *
 * Under partitionwise partial aggregation each partition computes a partial
 * state that PG finalizes above Append. ClickHouse can't emit a PG transition
 * state, so we reconstruct it from scalar components ClickHouse does compute.
 */
typedef enum {
    AGG_PARTIAL_NONE,       /* can't push as partial */
    AGG_PARTIAL_DIRECT,     /* transvalue is final value: no finalfn/serialfn */
    AGG_PARTIAL_AVG_INT,    /* int8[2] {count, sum}: avg(int2/int4) */
    AGG_PARTIAL_STAT_FLOAT, /* float8[3] {N, Sx, Sxx}: avg/var/stddev(float4/8) */
} AggPartialKind;

/*
 * Reconstructs plain (non-INTERNAL) transition array from ClickHouse, keyed on
 * accumulator so layout is known. INTERNAL-state aggregates would need their
 * serialfn and a version-specific struct, so are left to local aggregation.
 */
static AggPartialKind
agg_partial_kind(Aggref* agg) {
    HeapTuple tuple;
    Form_pg_aggregate aggform;
    AggPartialKind kind = AGG_PARTIAL_NONE;

    /* DISTINCT dedups per partition, so partials can't be combined */
    if (agg->aggdistinct) {
        return AGG_PARTIAL_NONE;
    }

    /* Planner already resolved aggfnoid, so the lookup always hits */
    tuple = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(agg->aggfnoid));
    if (!HeapTupleIsValid(tuple)) {
        return AGG_PARTIAL_NONE;
    }
    aggform = (Form_pg_aggregate)GETSTRUCT(tuple);

    if (!OidIsValid(aggform->aggcombinefn)) {
        /* No combinefn means Finalize can't merge per-partition partials */
        kind = AGG_PARTIAL_NONE;
    } else if (!OidIsValid(aggform->aggfinalfn) && !OidIsValid(aggform->aggserialfn)) {
        /* No finalfn: transvalue is the final value (count/sum/min/max) */
        kind = AGG_PARTIAL_DIRECT;
    } else if (
        /* Has a finalfn, but plain SQL transtype (no serialfn): an array we
         * can rebuild, provided no variadic/ORDER BY/ordered-set wrinkle */
        !OidIsValid(aggform->aggserialfn) && !agg->aggvariadic &&
        agg->aggorder == NIL && agg->aggkind == AGGKIND_NORMAL
    ) {
        switch (aggform->aggtransfn) {
        case F_INT2_AVG_ACCUM:
        case F_INT4_AVG_ACCUM:
            kind = AGG_PARTIAL_AVG_INT;
            break;
        case F_FLOAT4_ACCUM:
        case F_FLOAT8_ACCUM:
            kind = AGG_PARTIAL_STAT_FLOAT;
            break;
        default:
            break;
        }
    }

    ReleaseSysCache(tuple);
    return kind;
}

static bool
is_ok_in_expr(Expr* expr);

static bool
op_expr_never_null(OpExpr* op, foreign_glob_cxt* glob_cxt);

/*
 * True if `expr` provably cannot evaluate to NULL: a non-NULL constant, a
 * column of a scanned relation that is declared NOT NULL and cannot have been
 * null-extended by an outer join, or certain arithmetic operators over such
 * inputs (see op_expr_never_null). Anything else is assumed nullable.
 *
 * PG 17+ precomputes the catalog half of this as
 * RelOptInfo->notnullattnums; the syscache lookup below keeps one code path
 * across PG 13-19, so switch when PG 16 support drops. Postgres exports no
 * expression-level equivalent on any version.
 */
static bool
expr_never_null(Expr* expr, foreign_glob_cxt* glob_cxt) {
    while (IsA(expr, RelabelType)) {
        expr = ((RelabelType*)expr)->arg;
    }

    if (IsA(expr, Const)) {
        return !((Const*)expr)->constisnull;
    }

    if (IsA(expr, Var)) {
        Var* var = (Var*)expr;
        RangeTblEntry* rte;
        HeapTuple tp;
        bool notnull;

        /* Only a plain user column that this scan reads is provable */
        if (var->varlevelsup != 0 || var->varattno <= 0 ||
            !bms_is_member(var->varno, glob_cxt->relids)) {
            return false;
        }

#if PG_VERSION_NUM >= 160000
        /* A Var nulled by an outer join is nullable whatever the catalog says */
        if (!bms_is_empty(var->varnullingrels)) {
            return false;
        }
#else
        /* Pre-16 Vars carry no nulling info; give up if any outer join exists */
        {
            ListCell* jlc;

            foreach (jlc, glob_cxt->root->parse->rtable) {
                RangeTblEntry* jrte = (RangeTblEntry*)lfirst(jlc);

                if (jrte->rtekind == RTE_JOIN && jrte->jointype != JOIN_INNER) {
                    return false;
                }
            }
        }
#endif

        rte = planner_rt_fetch(var->varno, glob_cxt->root);
        if (rte == NULL || rte->rtekind != RTE_RELATION) {
            return false;
        }

        tp = SearchSysCache2(
            ATTNUM, ObjectIdGetDatum(rte->relid), Int16GetDatum(var->varattno)
        );
        if (!HeapTupleIsValid(tp)) {
            return false;
        }
        notnull = ((Form_pg_attribute)GETSTRUCT(tp))->attnotnull;
        ReleaseSysCache(tp);

        return notnull;
    }

    if (IsA(expr, OpExpr)) {
        return op_expr_never_null((OpExpr*)expr, glob_cxt);
    }

    return false;
}

/*
 * Operator implementation funcs that cannot return NULL when no input is NULL.
 * Limited to basic arithmetic, which returns a value or raises an error.
 * Being strict alone proves nothing here: a strict function may still compute
 * NULL from non-NULL inputs (json's ->> on a missing key, for one), and no
 * catalog flag distinguishes the two, so every entry below is hand-picked.
 * Extend as pushdown-worthy shapes turn up; the list need not stay sorted here,
 * since `sort_never_null_opfuncs` (below) sorts it in place once, at load time,
 * for consistency across PG versions.
 */
static Oid never_null_opfuncs[] = {
    /* + */
    F_INT2PL,
    F_INT4PL,
    F_INT8PL,
    F_INT24PL,
    F_INT42PL,
    F_INT28PL,
    F_INT82PL,
    F_INT48PL,
    F_INT84PL,
    F_FLOAT4PL,
    F_FLOAT8PL,
    F_FLOAT48PL,
    F_FLOAT84PL,
    F_NUMERIC_ADD,
    /* - */
    F_INT2MI,
    F_INT4MI,
    F_INT8MI,
    F_INT24MI,
    F_INT42MI,
    F_INT28MI,
    F_INT82MI,
    F_INT48MI,
    F_INT84MI,
    F_FLOAT4MI,
    F_FLOAT8MI,
    F_FLOAT48MI,
    F_FLOAT84MI,
    F_NUMERIC_SUB,
    /* * */
    F_INT2MUL,
    F_INT4MUL,
    F_INT8MUL,
    F_INT24MUL,
    F_INT42MUL,
    F_INT28MUL,
    F_INT82MUL,
    F_INT48MUL,
    F_INT84MUL,
    F_FLOAT4MUL,
    F_FLOAT8MUL,
    F_FLOAT48MUL,
    F_FLOAT84MUL,
    F_NUMERIC_MUL,
    /* unary - */
    F_INT2UM,
    F_INT4UM,
    F_INT8UM,
    F_FLOAT4UM,
    F_FLOAT8UM,
    F_NUMERIC_UMINUS,
};

static void __attribute__((constructor))
sort_never_null_opfuncs(void) {
    qsort(never_null_opfuncs, lengthof(never_null_opfuncs), sizeof(Oid), oid_cmp);
}

/*
 * True if `opfuncid` is one of never_null_opfuncs.
 */
static bool
is_non_nullable_op(Oid opfuncid) {
    return bsearch(
               &opfuncid,
               never_null_opfuncs,
               lengthof(never_null_opfuncs),
               sizeof(Oid),
               oid_cmp
           ) != NULL;
}

/*
 * Proves non-nullability of an OpExpr (in some cases). More cases = nicer SQL.
 * (If we fail to prove something is non-nullable, we just emit runtime guards.)
 * Returns true if `op` is a never-null operator (see `is_non_nullable_op`) and
 * every argument is itself provably non-NULL.
 */
static bool
op_expr_never_null(OpExpr* op, foreign_glob_cxt* glob_cxt) {
    ListCell* lc;

    set_opfuncid(op);
    if (!is_non_nullable_op(op->opfuncid)) {
        return false;
    }
    foreach (lc, op->args) {
        if (!expr_never_null((Expr*)lfirst(lc), glob_cxt)) {
            return false;
        }
    }
    return true;
}

/*
 * What is known at plan time about NULL elements in a ScalarArrayOpExpr's
 * right-hand array.
 */
typedef enum SaopArrayNulls {
    SAOP_ARR_EMPTY,     /* provably zero elements */
    SAOP_ARR_NULL_FREE, /* provably contains no NULL element */
    SAOP_ARR_UNKNOWN,   /* may contain a NULL element */
} SaopArrayNulls;

static SaopArrayNulls
saop_array_nulls(Expr* arr, foreign_glob_cxt* glob_cxt) {
    if (IsA(arr, Const)) {
        Const* c = (Const*)arr;
        ArrayType* a;

        if (c->constisnull) {
            return SAOP_ARR_UNKNOWN;
        }
        a = DatumGetArrayTypeP(c->constvalue);
        if (ArrayGetNItems(ARR_NDIM(a), ARR_DIMS(a)) == 0) {
            return SAOP_ARR_EMPTY;
        }
        /* No null bitmap means no NULL element; with one, assume the worst */
        return ARR_HASNULL(a) ? SAOP_ARR_UNKNOWN : SAOP_ARR_NULL_FREE;
    }

    if (IsA(arr, ArrayExpr) && !((ArrayExpr*)arr)->multidims) {
        ArrayExpr* ae = (ArrayExpr*)arr;
        ListCell* lc;

        if (ae->elements == NIL) {
            return SAOP_ARR_EMPTY;
        }
        foreach (lc, ae->elements) {
            if (!expr_never_null((Expr*)lfirst(lc), glob_cxt)) {
                return SAOP_ARR_UNKNOWN;
            }
        }
        return SAOP_ARR_NULL_FREE;
    }

    return SAOP_ARR_UNKNOWN;
}

/*
 * How to ship a Postgres `NOT (x IN (SELECT ..))`.
 */
typedef enum NotInShipForm {
    NOTIN_SHIP_PLAIN,   /* both sides provably non-NULL: native NOT IN is exact */
    NOTIN_SHIP_GUARDED, /* deparseGuardedNotIn's CASE/guard form (truth context) */
    NOTIN_SHIP_NONE,    /* no correct remote form; evaluate locally */
} NotInShipForm;

/*
 * Which sides of an ANY SubPlan's comparison may produce a NULL: the probe
 * (testexpr's LHS, an outer-scope expression) and the subquery's output
 * column. A bit is set for each side that cannot be proven non-NULL, so the
 * guarded NOT IN deparse emits a guard exactly for each bit.
 */
typedef enum NotInNullability {
    NOTIN_NEITHER_NULLABLE = 0,
    NOTIN_PROBE_NULLABLE   = 1 << 0,
    NOTIN_OUTPUT_NULLABLE  = 1 << 1,
} NotInNullability;

/*
 * Prove non-nullness for an ANY SubPlan's two comparison inputs, reporting
 * the sides that cannot be proven. root/relids describe the OUTER scope
 * (the probe's); the subquery scope is derived here. A missing output
 * column reports both sides nullable.
 */
static NotInNullability
notin_prove_nullability(SubPlan* subplan, PlannerInfo* root, Relids relids) {
    OpExpr* op = castNode(OpExpr, subplan->testexpr);
    PlannerInfo* subroot;
    Query* query;
    TargetEntry* out       = NULL;
    NotInNullability nulls = NOTIN_NEITHER_NULLABLE;
    foreign_glob_cxt outer_cxt;
    foreign_glob_cxt sub_cxt;
    ListCell* lc;

    subroot = (PlannerInfo*)list_nth(root->glob->subroots, subplan->plan_id - 1);
    query   = subroot->parse;

    foreach (lc, query->targetList) {
        TargetEntry* tle = lfirst_node(TargetEntry, lc);

        if (!tle->resjunk) {
            out = tle;
            break;
        }
    }
    if (out == NULL) {
        return NOTIN_PROBE_NULLABLE | NOTIN_OUTPUT_NULLABLE;
    }

    memset(&outer_cxt, 0, sizeof(outer_cxt));
    outer_cxt.root   = root;
    outer_cxt.relids = relids;

    memset(&sub_cxt, 0, sizeof(sub_cxt));
    sub_cxt.root = subroot;
    foreach (lc, query->jointree->fromlist) {
        RangeTblRef* rtr = (RangeTblRef*)lfirst(lc);

        sub_cxt.relids = bms_add_member(sub_cxt.relids, rtr->rtindex);
    }

    if (!expr_never_null((Expr*)linitial(op->args), &outer_cxt)) {
        nulls |= NOTIN_PROBE_NULLABLE;
    }
    if (!expr_never_null(out->expr, &sub_cxt)) {
        nulls |= NOTIN_OUTPUT_NULLABLE;
    }
    return nulls;
}

/*
 * Classify a NOT-negated ANY SubPlan. Called with identical inputs by the
 * shippability gate in is_shippable_subplan and by deparseBoolExpr's SubPlan
 * peek, so the walker's decision and the emitted form always agree.
 */
static NotInShipForm
classify_notin_subplan(SubPlan* subplan, PlannerInfo* root, Relids relids) {
    PlannerInfo* subroot;
    Query* query;

    if (notin_prove_nullability(subplan, root, relids) == NOTIN_NEITHER_NULLABLE) {
        return NOTIN_SHIP_PLAIN;
    }

    /*
     * The guarded form's null probe re-runs the subquery body with an extra
     * "output IS NULL" qual, which asks the same question only when the
     * output is a per-row expression. A grouped or aggregated body would
     * need the guard applied above the grouping; not implemented, so those
     * shapes fall back to local evaluation.
     */
    subroot = (PlannerInfo*)list_nth(root->glob->subroots, subplan->plan_id - 1);
    query   = subroot->parse;
    if (query->groupClause == NIL && query->havingQual == NULL && !query->hasAggs) {
        return NOTIN_SHIP_GUARDED;
    }
    return NOTIN_SHIP_NONE;
}

/*
 * Recognize either a native ClickHouse JSON column imported as jsonb or the
 * compatibility-view shape used for a String containing a JSON document:
 *
 *     jsonb_in(foreign_text_column::cstring)
 *
 * Arbitrary jsonb expressions stay local because their remote representation
 * is not known.
 */
static JsonbDocumentKind
classifyJsonbDocument(Expr* expr, Expr** document) {
    while (IsA(expr, RelabelType)) {
        expr = ((RelabelType*)expr)->arg;
    }

    if (IsA(expr, Var) && exprType((Node*)expr) == JSONBOID) {
        *document = expr;
        return JSONB_DOCUMENT_NATIVE;
    }

    /*
     * Check to see if Postgres added a call to jsonb_in() to do a transparent
     * cast. Or even if the user put it in in explicitly. In either case,
     * we'll want to push down its behavior in deparseJsonbExists() to
     * validate the JSON.
     *
     * TODO: Consider adding explicit pushdown support for jsonb_in() (and
     * other _in functions?), rather than special case it here.
     */
    if (IsA(expr, FuncExpr)) {
        FuncExpr* func = (FuncExpr*)expr;

        if (func->funcid == F_JSONB_IN && list_length(func->args) == 1) {
            Expr* input = (Expr*)linitial(func->args);

            while (IsA(input, RelabelType)) {
                input = ((RelabelType*)input)->arg;
            }
            if (IsA(input, CoerceViaIO)) {
                CoerceViaIO* cast = (CoerceViaIO*)input;
                Expr* raw         = cast->arg;

                while (IsA(raw, RelabelType)) {
                    raw = ((RelabelType*)raw)->arg;
                }
                if (cast->resulttype == CSTRINGOID && exprType((Node*)raw) == TEXTOID) {
                    *document = raw;
                    return JSONB_DOCUMENT_STRING;
                }
            }
        }
    }

    *document = NULL;
    return JSONB_DOCUMENT_UNSUPPORTED;
}

/*
 * Check if expression is safe to execute remotely, and return true if so.
 *
 * We must check that the expression contains only node types we can deparse,
 * that all types/functions/operators are safe to send (they are "shippable"),
 * and that all collations used in the expression derive from Vars of the
 * foreign table. Because of the latter, the logic is pretty close to
 * assign_collations_walker() in parse_collate.c, though we can assume here
 * that the given expression is valid. Note function mutability is not
 * currently considered here.
 *
 * ctx tracks whether the expression's value is observable beyond qualifying
 * rows (see ExprTruthCtx and deparseScalarArrayOpExpr's block comment):
 * EXPR_CTX_TRUTH only while every enclosing node treats NULL and FALSE
 * identically.
 */
static bool
foreign_expr_walker(Node* node, foreign_glob_cxt* glob_cxt, ExprTruthCtx ctx) {
    bool check_type = true;
    CHFdwRelationInfo* fpinfo;

    /* Need do nothing for empty subexpressions */
    if (node == NULL) {
        return true;
    }

    /* May need server info from baserel's fdw_private struct */
    fpinfo = (CHFdwRelationInfo*)(glob_cxt->foreignrel->fdw_private);

    switch (nodeTag(node)) {
    case T_Var: {
        Var* var = (Var*)node;

        /*
         * If the Var is from the foreign table, we consider its
         * collation (if any) safe to use. If it is from another
         * table, we treat its collation the same way as we would a
         * Param's collation, ie it's not safe for it to have a
         * non-default collation.
         */
        if (bms_is_member(var->varno, glob_cxt->relids) && var->varlevelsup == 0) {
            /* Var belongs to foreign table */

            /*
             * System columns other than ctid and oid should not be
             * sent to the remote, since we don't make any effort to
             * ensure that local and remote values match (tableoid, in
             * particular, almost certainly doesn't match).
             */
            if (var->varattno < 0) {
                return false;
            }
        }
    } break;
    case T_Const:
        break;
    case T_Param: {
        Param* p = (Param*)node;

        /*
         * If it's a MULTIEXPR Param, punt.  We can't tell from here
         * whether the referenced sublink/subplan contains any remote
         * Vars; if it does, handling that is too complicated to
         * consider supporting at present.  Fortunately, MULTIEXPR
         * Params are not reduced to plain PARAM_EXEC until the end of
         * planning, so we can easily detect this case.  (Normal
         * PARAM_EXEC Params are safe to ship because their values
         * come from somewhere else in the plan tree; but a MULTIEXPR
         * references a sub-select elsewhere in the same targetlist,
         * so we'd be on the hook to evaluate it somehow if we wanted
         * to handle such cases as direct foreign updates.)
         */
        if (p->paramkind == PARAM_MULTIEXPR) {
            return false;
        }
    } break;
    case T_SubscriptingRef: {
        SubscriptingRef* ar = (SubscriptingRef*)node;

        /* Assignment should not be in restrictions. */
        if (ar->refassgnexpr != NULL) {
            return false;
        }

        /*
         * The jsonb subscript syntax column['key'] is not supported
         * on ClickHouse JSON (it requires dot notation). Refuse to
         * push down jsonb subscript expressions for now, so they are
         * evaluated locally instead.
         */
        if (ar->refcontainertype == JSONBOID) {
            return false;
        }

        /* Recurse to remaining subexpressions. */
        if (!foreign_expr_walker(
                (Node*)ar->refupperindexpr, glob_cxt, EXPR_CTX_EXACT
            )) {
            return false;
        }

        if (!foreign_expr_walker(
                (Node*)ar->reflowerindexpr, glob_cxt, EXPR_CTX_EXACT
            )) {
            return false;
        }

        if (!foreign_expr_walker((Node*)ar->refexpr, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_FuncExpr: {
        CustomObjectDef* cdef = NULL;
        FuncExpr* fe          = (FuncExpr*)node;

        /*
         * If function used by the expression is not shippable, it
         * can't be sent to remote because it might have incompatible
         * semantics on remote side.
         */
        if (!chfdw_is_shippable(node, fe->funcid, ProcedureRelationId, fpinfo, &cdef)) {
            return false;
        }

        /*
         * jsonb?_extract_path_text / jsonb?_extract_path are always
         * presented by the planner in variadic form: two args where
         * the second is a text[] constant.  Allow them through the
         * variadic gate; only recurse on the column argument.
         */
        if (cdef && (cdef->cf_type == CF_JSON_EXTRACT_PATH_TEXT ||
                     cdef->cf_type == CF_JSON_EXTRACT_PATH)) {
            if (list_length(fe->args) != 2 || !IsA(lsecond(fe->args), Const) ||
                ((Const*)lsecond(fe->args))->constisnull) {
                return false;
            }

            Const* path = (Const*)lsecond(fe->args);
            if (array_contains_nulls(DatumGetArrayTypeP(path->constvalue))) {
                return false;
            }

            /* Only recurse on the column expression. */
            if (!foreign_expr_walker(
                    (Node*)linitial(fe->args), glob_cxt, EXPR_CTX_EXACT
                )) {
                return false;
            }
            break;
        }

        /*
         * ClickHouse length() only represents the first dimension. A
         * non-empty PostgreSQL array has the same first-dimension length,
         * but empty arrays return NULL rather than zero.
         */
        if (cdef && cdef->cf_type == CF_ARRAY_LENGTH) {
            if (list_length(fe->args) != 2 || !IsA(lsecond(fe->args), Const)) {
                return false;
            }

            Const* dimension = (Const*)lsecond(fe->args);
            if (dimension->constisnull || DatumGetInt32(dimension->constvalue) != 1) {
                return false;
            }
        }

        /*
         * Recurse to input subexpressions.
         */
        if (!foreign_expr_walker((Node*)fe->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_SQLValueFunction:
        /* All handled by deparseSQLValueFunction(). */
        break;
    case T_OpExpr:
    case T_NullIfExpr:
    case T_DistinctExpr: /* struct-equivalent to OpExpr */
    {
        OpExpr* oe = (OpExpr*)node;
        CustomObjectDef* cdef;

        /*
         * Similarly, only shippable operators can be sent to remote.
         * (If the operator is shippable, we assume its underlying
         * function is too.)
         */
        if (!chfdw_is_shippable(node, oe->opno, OperatorRelationId, fpinfo, NULL)) {
            return false;
        }

        cdef = chfdw_check_for_custom_operator(oe->opno, NULL);
        if (cdef && cdef->cf_type == CF_JSON_EXISTS) {
            Expr* document;
            JsonbDocumentKind document_kind =
                classifyJsonbDocument((Expr*)linitial(oe->args), &document);

            if (document_kind == JSONB_DOCUMENT_UNSUPPORTED ||
                !foreign_expr_walker((Node*)document, glob_cxt, EXPR_CTX_EXACT) ||
                !foreign_expr_walker(
                    (Node*)lsecond(oe->args), glob_cxt, EXPR_CTX_EXACT
                )) {
                return false;
            }
            break;
        }

        /*
         * Recurse to input subexpressions.
         */
        if (!foreign_expr_walker((Node*)oe->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_ScalarArrayOpExpr: {
        ScalarArrayOpExpr* oe = (ScalarArrayOpExpr*)node;
        CHEqualOp optype      = chfdw_is_equal_op(oe->opno);
        Expr* arr;

        if (optype == CH_OP_NONE) {
            return false;
        }

        /*
         * ClickHouse doesn't support null arrays, and Postgres's planner won't
         * optimize away `x in NULL::int[]` unless x is also NULL. Postgres will
         * always evaluate this to null, but pushing the whole test down as NULL
         * would remove the SQL in the probe (and any side-effects thereof).
         * This is a pathological case and the most correct form to push would
         * look like `tupleElement((<probe_sql>, NULL), 2)`, which is dumb.
         * Keep the pathological case local. Everything else ships; see
         * deparseScalarArrayOpExpr's block comment for how it preserves
         * Postgres's NULL semantics across the more difficult cases.
         */
        arr = (Expr*)lsecond(oe->args);
        if (IsA(arr, Const) && ((Const*)arr)->constisnull) {
            return false;
        }

        /*
         * Recurse to input subexpressions.
         */
        if (!foreign_expr_walker((Node*)oe->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_RelabelType: {
        RelabelType* r = (RelabelType*)node;

        /*
         * Recurse to input subexpression. A relabel between a NOT and its
         * operand would defeat deparseBoolExpr's SubPlan peek, so degrade
         * NEGATED rather than pass it through.
         */
        if (!foreign_expr_walker(
                (Node*)r->arg, glob_cxt, ctx == EXPR_CTX_NEGATED ? EXPR_CTX_EXACT : ctx
            )) {
            return false;
        }
    } break;
    case T_BoolExpr: {
        BoolExpr* b = (BoolExpr*)node;
        ExprTruthCtx child_ctx;

        /*
         * AND/OR preserve the NULL/FALSE equivalence of a truth context but
         * consume a NEGATED state (their children are not the NOT's direct
         * operand). Under NOT the roles swap — a FALSE-for-NULL substitution
         * below would surface as TRUE-for-NULL above — so the operand must be
         * exact, except that one NOT directly over a truth context grants the
         * operand EXPR_CTX_NEGATED, which a SubPlan consumes via the guarded
         * NOT IN deparse. A second NOT above that degrades to exact (the
         * planner collapses double negation anyway).
         */
        if (b->boolop == NOT_EXPR) {
            child_ctx = ctx == EXPR_CTX_TRUTH ? EXPR_CTX_NEGATED : EXPR_CTX_EXACT;
        } else {
            child_ctx = ctx == EXPR_CTX_NEGATED ? EXPR_CTX_EXACT : ctx;
        }
        if (!foreign_expr_walker((Node*)b->args, glob_cxt, child_ctx)) {
            return false;
        }
    } break;
    case T_NullTest: {
        NullTest* nt = (NullTest*)node;

        /*
         * Recurse to input subexpressions.
         */
        if (!foreign_expr_walker((Node*)nt->arg, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_ArrayExpr: {
        ArrayExpr* a = (ArrayExpr*)node;

        /*
         * Recurse to input subexpressions.
         */
        if (!foreign_expr_walker((Node*)a->elements, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_List: {
        List* l = (List*)node;
        ListCell* lc;

        /*
         * Recurse to component subexpressions.
         */
        foreach (lc, l) {
            if (!foreign_expr_walker((Node*)lfirst(lc), glob_cxt, ctx)) {
                return false;
            }
        }

        /* Don't apply exprType() to the list. */
        check_type = false;
    } break;
    case T_Aggref: {
        Aggref* agg = (Aggref*)node;
        ListCell* lc;

        /*
         * Not safe to pushdown when not in grouping context. Inside a
         * SubPlan's Query a bare aggregate is legal (it's the
         * subquery's own implicit grouping context).
         */
        if (!IS_UPPER_REL(glob_cxt->foreignrel) && !glob_cxt->subquery_scope) {
            return false;
        }

        /* Simple aggregates push down directly */
        if (agg->aggsplit != AGGSPLIT_SIMPLE &&
            agg_partial_kind(agg) == AGG_PARTIAL_NONE) {
            return false;
        }

        /* As usual, it must be shippable. */
        if (!chfdw_is_shippable(
                node, agg->aggfnoid, ProcedureRelationId, fpinfo, NULL
            )) {
            return false;
        }

        /* Features that ClickHouse doesn't support */
        if (AGGKIND_IS_ORDERED_SET(agg->aggkind) &&
            !chfdw_check_for_ordered_aggregate(agg)) {
            return false;
        }

        if (agg->aggdistinct && agg->aggfilter) {
            return false;
        }

        /* groupConcat has no ORDER BY; block ordered string_agg */
        if (agg->aggfnoid == F_STRING_AGG_TEXT_TEXT && agg->aggorder != NIL) {
            return false;
        }

        /* Block aggregates with no ClickHouse equivalent */
        switch (agg->aggfnoid) {
        case F_STRING_AGG_BYTEA_BYTEA:
        case F_REGR_COUNT:
        case F_REGR_SXX:
        case F_REGR_SYY:
        case F_REGR_SXY:
        case F_REGR_AVGX:
        case F_REGR_AVGY:
        case F_REGR_R2:
        case F_REGR_SLOPE:
        case F_REGR_INTERCEPT:
#if PG_VERSION_NUM >= 160000
        case F_JSON_AGG_STRICT:
        case F_JSONB_AGG_STRICT:
#endif
            return false;
        }

        /*
         * Recurse to input args. aggdirectargs, aggorder and
         * aggdistinct are all present in args, so no need to check
         * their shippability explicitly.
         */
        foreach (lc, agg->args) {
            Node* n = (Node*)lfirst(lc);

            /* If TargetEntry, extract the expression from it */
            if (IsA(n, TargetEntry)) {
                TargetEntry* tle = (TargetEntry*)n;

                n = (Node*)tle->expr;
            }

            if (!foreign_expr_walker(n, glob_cxt, EXPR_CTX_EXACT)) {
                return false;
            }
        }

        /* FILTER only qualifies rows, so it keeps NULL/FALSE equivalence */
        if (!foreign_expr_walker((Node*)agg->aggfilter, glob_cxt, EXPR_CTX_TRUTH)) {
            return false;
        }
    } break;
    case T_WindowFunc: {
        WindowFunc* wfunc = (WindowFunc*)node;

        /* Not safe to pushdown when not in upper relation context */
        if (!IS_UPPER_REL(glob_cxt->foreignrel)) {
            return false;
        }

        /* The window function must be shippable */
        if (!chfdw_is_shippable(
                node, wfunc->winfnoid, ProcedureRelationId, fpinfo, NULL
            )) {
            return false;
        }

        /* FILTER is not supported in ClickHouse window functions */
        if (wfunc->aggfilter) {
            return false;
        }

        /* Recurse to input arguments */
        if (!foreign_expr_walker((Node*)wfunc->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_CaseExpr: {
        CaseExpr* caseexpr = (CaseExpr*)node;
        ListCell* lc;

        if (!foreign_expr_walker((Node*)caseexpr->arg, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }

        foreach (lc, caseexpr->args) {
            CaseWhen* when = lfirst_node(CaseWhen, lc);

            /* A WHEN condition takes the same branch for NULL and FALSE */
            if (!foreign_expr_walker((Node*)when->expr, glob_cxt, EXPR_CTX_TRUTH)) {
                return false;
            }
            if (!foreign_expr_walker(
                    (Node*)when->result,
                    glob_cxt,
                    ctx == EXPR_CTX_TRUTH ? EXPR_CTX_TRUTH : EXPR_CTX_EXACT
                )) {
                return false;
            }
        }
        if (!foreign_expr_walker(
                (Node*)caseexpr->defresult,
                glob_cxt,
                ctx == EXPR_CTX_TRUTH ? EXPR_CTX_TRUTH : EXPR_CTX_EXACT
            )) {
            return false;
        }
    } break;
    case T_CoalesceExpr: {
        CoalesceExpr* ce = (CoalesceExpr*)node;

        if (!foreign_expr_walker((Node*)ce->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_MinMaxExpr: {
        MinMaxExpr* me = (MinMaxExpr*)node;

        if (!foreign_expr_walker((Node*)me->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_CoerceViaIO: {
        CoerceViaIO* me = (CoerceViaIO*)node;

        if (!foreign_expr_walker((Node*)me->arg, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_RowExpr: {
        RowExpr* me = (RowExpr*)node;

        if (!foreign_expr_walker((Node*)me->args, glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    } break;
    case T_CaseTestExpr:
        break;
    case T_SubPlan: {
        /*
         * A SubPlan node is the planner's residue of a SubLink that
         * could not be flattened into a join: a correlated or
         * uncorrelated subquery evaluated per-row. We can fold a
         * restricted class of these into the remote SQL as a real SQL
         * subquery; is_shippable_subplan() decides. Nested SubPlans
         * are out of scope: deparseParam resolves correlation through
         * a single parent_ctx link.
         */
        if (glob_cxt->subquery_scope) {
            return false;
        }

        if (!is_shippable_subplan((SubPlan*)node, glob_cxt, ctx)) {
            return false;
        }
    } break;
    default:

        /*
         * If it's anything else, assume it's unsafe. This list can be
         * expanded later, but don't forget to add deparse support below.
         */
        return false;
    }

    /*
     * If result type of given expression is not shippable, it can't be sent
     * to remote because it might have incompatible semantics on remote side.
     */
    if (check_type &&
        !chfdw_is_shippable(node, exprType(node), TypeRelationId, fpinfo, NULL)) {
        return false;
    }

    return true;
}

/*
 * Decide whether a SubPlan can be folded into the remote SQL as a real SQL
 * subquery.
 *
 * Supported SubLinkTypes:
 *   EXPR_SUBLINK    (SELECT agg(..) ..)        scalar; aggregate-only, see
 *                                              the single-row note below
 *   EXISTS_SUBLINK  EXISTS (SELECT ..)
 *   ANY_SUBLINK     x IN (SELECT ..)           single-column equality only
 *
 * Unsupported (and why):
 *   ALL_SUBLINK         ClickHouse lacks a direct ALL; NOT IN arrives as
 *                       NOT(ANY) and is handled by the BoolExpr case. A
 *                       genuine ALL could be deparsed in the future: `<> ALL`
 *                       is NOT IN, and an inequality ALL reduces to a compare
 *                       against min()/max(). Not yet implemented.
 *   ROWCOMPARE_SUBLINK  multi-column compares not currently deparsed;
 *                       deparsable in principle as a tuple compare. Future
 *                       work.
 *   MULTIEXPR_SUBLINK   UPDATE-only; also blocked by the PARAM_MULTIEXPR
 *                       guard in the walker.
 *   ARRAY_SUBLINK       ARRAY() construction differs on ClickHouse.
 *   CTE_SUBLINK         WITH-clause references; ClickHouse does support CTEs,
 *                       so this is deparsable in principle but not yet wired
 *                       up here. Future work.
 *
 * The subquery body itself must be a plain comma-joined SELECT over foreign
 * tables that live on the same server as the outer scan: no set ops, CTEs,
 * window functions, DISTINCT, ORDER BY, LIMIT, grouping sets, SRFs, nested
 * SubPlans, or InitPlans. Correlation arrives as PARAM_EXEC Params (the
 * planner has already replaced outer Vars via SS_replace_correlation_vars);
 * each correlation arg expression is itself checked for shippability in the
 * OUTER scope, since deparseParam will inline it with outer aliases.
 *
 * Single-row note (semantic wrinkle, deliberately fenced off): a scalar
 * subquery is required to return at most one row. When it returns ZERO rows,
 * Postgres substitutes NULL, but ClickHouse's scalar-subquery semantics
 * differ (it can raise or yield an empty result rather than NULL), so a naive
 * push would change query results. We sidestep this entirely by only pushing
 * EXPR subqueries whose top level is a bare aggregate with no GROUP BY: such a
 * query returns exactly one row on both systems by construction, so the
 * zero-row divergence cannot arise. A non-aggregate or grouped scalar
 * subquery is left for local execution (see the guard below, and the
 * negative case in test/sql/subplan_pushdown.sql). This could be relaxed in
 * the future by wrapping the pushed subquery so an empty result yields NULL
 * (e.g. deparsing it under an any()/singleValueOrNull() aggregate); not yet
 * implemented.
 */
/*
 * Resolve the user mapping the executor will use to scan foreignrel, for the
 * plan-time server-version probe below. Mirrors the executor's own lookup
 * (see clickhouseBeginForeignScan): the RTE's checkAsUser — set when the rel
 * is accessed on behalf of another user, e.g. through a view — wins over the
 * invoking user. Returns NULL instead of erroring when no mapping exists, so
 * the version gate degrades to "no pushdown" rather than failing a query
 * (or a bare EXPLAIN) at plan time.
 */
static UserMapping*
subplan_gate_user_mapping(PlannerInfo* root, RelOptInfo* foreignrel, Oid serverid) {
    Oid userid  = InvalidOid;
    int rtindex = -1;

    /*
     * Find a representative base-rel RTE; any member gives the same answer
     * for the rels this walker runs against. An upper rel may have empty
     * relids, so descend to its input rel first. On PG16+ relids may also
     * carry outer-join indexes whose RTEs are not relations; skip those.
     */
    if (bms_is_empty(foreignrel->relids)) {
        CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;

        if (fpinfo != NULL && fpinfo->outerrel != NULL) {
            foreignrel = fpinfo->outerrel;
        }
    }
    while ((rtindex = bms_next_member(foreignrel->relids, rtindex)) >= 0) {
        RangeTblEntry* rte;

        if (rtindex <= 0 || rtindex > list_length(root->parse->rtable)) {
            continue;
        }
        rte = planner_rt_fetch(rtindex, root);
        if (rte == NULL || rte->rtekind != RTE_RELATION) {
            continue;
        }
#if PG_VERSION_NUM >= 160000
        if (rte->perminfoindex != 0) {
            userid = getRTEPermissionInfo(root->parse->rteperminfos, rte)->checkAsUser;
        }
#else
        userid = rte->checkAsUser;
#endif
        break;
    }
    if (!OidIsValid(userid)) {
        userid = GetUserId();
    }

    /*
     * GetUserMapping ereports when neither a per-user nor a PUBLIC mapping
     * exists; probe the syscache first so the gate can refuse quietly.
     */
    if (!SearchSysCacheExists2(
            USERMAPPINGUSERSERVER, ObjectIdGetDatum(userid), ObjectIdGetDatum(serverid)
        ) &&
        !SearchSysCacheExists2(
            USERMAPPINGUSERSERVER,
            ObjectIdGetDatum(InvalidOid),
            ObjectIdGetDatum(serverid)
        )) {
        return NULL;
    }
    return GetUserMapping(userid, serverid);
}

static bool
is_shippable_subplan(SubPlan* subplan, foreign_glob_cxt* glob_cxt, ExprTruthCtx ctx) {
    PlannerInfo* subroot;
    Query* query;
    foreign_glob_cxt sub_cxt;
    CHFdwRelationInfo* fpinfo;
    ListCell* lc;

    if (subplan->subLinkType != EXPR_SUBLINK &&
        subplan->subLinkType != EXISTS_SUBLINK && subplan->subLinkType != ANY_SUBLINK) {
        return false;
    }

    /*
     * plan_id is 1-based and indexes both glob->subplans and glob->subroots
     * in parallel (see pathnodes.h).
     */
    if (subplan->plan_id <= 0 ||
        subplan->plan_id > list_length(glob_cxt->root->glob->subroots)) {
        return false;
    }

    subroot =
        (PlannerInfo*)list_nth(glob_cxt->root->glob->subroots, subplan->plan_id - 1);
    query  = subroot->parse;
    fpinfo = (CHFdwRelationInfo*)(glob_cxt->foreignrel->fdw_private);

    /*
     * fdw_private and ->server are populated by clickhouseGetForeignRelSize /
     * foreign_join_ok for any relation we plan, so for the rels this walker
     * runs against they are normally set. Guard defensively anyway: we
     * dereference fpinfo->server->serverid below to require the subquery's
     * tables live on the same server, and a NULL here simply means "can't
     * prove same-server" -> refuse the pushdown rather than risk a crash.
     */
    if (fpinfo == NULL || fpinfo->server == NULL) {
        return false;
    }

    /* Structural features we do not deparse */
    if (query->setOperations || query->cteList || query->windowClause ||
        query->distinctClause || query->sortClause || query->limitOffset ||
        query->limitCount || query->groupingSets || query->hasTargetSRFs ||
        query->jointree == NULL || query->jointree->fromlist == NIL) {
        return false;
    }

    /* No InitPlans hanging off the subquery, no nested SubPlans */
    if (subroot->init_plans != NIL) {
        return false;
    }
    if (contain_subplans((Node*)query->targetList) ||
        contain_subplans((Node*)query->jointree->quals) ||
        contain_subplans(query->havingQual)) {
        return false;
    }

    /* Scalar subqueries must be single-row by construction (see note) */
    if (subplan->subLinkType == EXPR_SUBLINK &&
        (!query->hasAggs || query->groupClause != NIL)) {
        return false;
    }

    /*
     * FROM must be plain comma-joined foreign tables on the same server as
     * the outer scan.
     */
    foreach (lc, query->jointree->fromlist) {
        RangeTblRef* rtr;
        RangeTblEntry* rte;

        if (!IsA(lfirst(lc), RangeTblRef)) {
            return false;
        }
        rtr = (RangeTblRef*)lfirst(lc);
        rte = rt_fetch(rtr->rtindex, query->rtable);

        if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_FOREIGN_TABLE ||
            rte->securityQuals != NIL) {
            return false;
        }

        if (GetForeignTable(rte->relid)->serverid != fpinfo->server->serverid) {
            return false;
        }
    }

    /*
     * ANY: only single-column equality (deparsed as IN). testexpr compares an
     * outer-scope LHS against the subquery's output Param. An inequality ANY
     * (e.g. x < ANY (SELECT ..)) could be supported in the future by
     * rewriting to a compare against min()/max(); not yet implemented.
     */
    if (subplan->subLinkType == ANY_SUBLINK) {
        OpExpr* op;

        if (subplan->testexpr == NULL || !IsA(subplan->testexpr, OpExpr)) {
            return false;
        }
        op = (OpExpr*)subplan->testexpr;
        if (list_length(op->args) != 2) {
            return false;
        }
        if (chfdw_is_equal_op(op->opno) != CH_OP_EQ) {
            return false;
        }
        if (!IsA(lsecond(op->args), Param)) {
            return false;
        }

        /* The LHS lives in the outer scope: walk it there. */
        if (!foreign_expr_walker((Node*)linitial(op->args), glob_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    }

    /* Scalar output must be a single column */
    if (subplan->subLinkType == EXPR_SUBLINK) {
        int n = 0;

        foreach (lc, query->targetList) {
            TargetEntry* tle = lfirst_node(TargetEntry, lc);

            if (!tle->resjunk) {
                n++;
            }
        }
        if (n != 1) {
            return false;
        }
    }

    /*
     * Correlation args are OUTER-scope expressions; deparseParam will inline
     * them with outer aliases, so they must be shippable out here.
     */
    if (!foreign_expr_walker((Node*)subplan->args, glob_cxt, EXPR_CTX_EXACT)) {
        return false;
    }

    /*
     * Walk the subquery's own expressions in a sub-scope whose relids are the
     * subquery's FROM entries.
     */
    memset(&sub_cxt, 0, sizeof(sub_cxt));
    sub_cxt.root           = subroot;
    sub_cxt.foreignrel     = glob_cxt->foreignrel; /* for fpinfo lookups */
    sub_cxt.subquery_scope = true;
    sub_cxt.relids         = NULL;
    foreach (lc, query->jointree->fromlist) {
        RangeTblRef* rtr = (RangeTblRef*)lfirst(lc);

        sub_cxt.relids = bms_add_member(sub_cxt.relids, rtr->rtindex);
    }

    foreach (lc, query->targetList) {
        TargetEntry* tle = lfirst_node(TargetEntry, lc);

        if (!foreign_expr_walker((Node*)tle->expr, &sub_cxt, EXPR_CTX_EXACT)) {
            return false;
        }
    }
    /* Interior quals only qualify the subquery's own rows: truth context */
    if (!foreign_expr_walker((Node*)query->jointree->quals, &sub_cxt, EXPR_CTX_TRUTH)) {
        return false;
    }
    if (query->havingQual &&
        !foreign_expr_walker(query->havingQual, &sub_cxt, EXPR_CTX_TRUTH)) {
        return false;
    }

    /*
     * ClickHouse evaluates IN under two-valued logic, so x IN (SELECT ..)
     * diverges from Postgres by yielding FALSE where Postgres computes
     * NULL (a NULL probe stays NULL on both systems). This behavior is invisible in a
     * truth context but observable under NOT --- which is how x NOT IN
     * (SELECT ..) arrives here --- and in value positions. Under one NOT in a
     * truth context (EXPR_CTX_NEGATED) the guarded deparse form keeps the
     * Postgres answer even when a NULL can reach the comparison, so only
     * grouped/aggregated bodies fall back; a genuine value position demands
     * exactness, which requires that no NULL can arrive from either side.
     * See deparseScalarArrayOpExpr's block comment for the divergence rules
     * and deparseGuardedNotIn for the guarded form.
     */
    if (subplan->subLinkType == ANY_SUBLINK && ctx != EXPR_CTX_TRUTH) {
        NotInShipForm form =
            classify_notin_subplan(subplan, glob_cxt->root, glob_cxt->relids);

        if (ctx == EXPR_CTX_NEGATED ? form == NOTIN_SHIP_NONE
                                    : form != NOTIN_SHIP_PLAIN) {
            return false;
        }
    }

    /*
     * Final gate: ClickHouse supports the correlated-subquery and NOT IN shapes
     * this pushes down from 25.8 onward (its new analyzer), and the exact SQL
     * we emit runs correctly there (verified against 25.8/26.3/26.5). Older
     * servers raise an error on the pushed SQL rather than degrade, so refuse
     * the pushdown below 25.8 and let Postgres run the SubPlan locally (correct,
     * if slower). Checked last so a plan-time connection is opened only for a
     * subplan that is otherwise shippable. The probe connects with the same
     * user mapping the executor will use for the scan (the rel is accessed as
     * its RTE's checkAsUser, e.g. a view's owner, when that is set), and when
     * no mapping exists it refuses the pushdown rather than erroring.
     */
    {
        UserMapping* user = subplan_gate_user_mapping(
            glob_cxt->root, glob_cxt->foreignrel, fpinfo->server->serverid
        );

        if (user == NULL || !chfdw_version_ge(chfdw_get_server_version(user), 25, 8)) {
            return false;
        }
    }

    return true;
}

/*
 * Returns true if given expr is something we'd have to send the value of
 * to the foreign server.
 *
 * This should return true when the expression is a shippable node that
 * deparseExpr would add to context->params_list.  Note that we don't care
 * if the expression *contains* such a node, only whether one appears at top
 * level.  We need this to detect cases where setrefs.c would recognize a
 * false match between an fdw_exprs item (which came from the params_list)
 * and an entry in fdw_scan_tlist (which we're considering putting the given
 * expression into).
 */
bool
is_foreign_param(PlannerInfo* root, RelOptInfo* baserel, Expr* expr) {
    if (expr == NULL) {
        return false;
    }

    switch (nodeTag(expr)) {
    case T_Var: {
        /* It would have to be sent unless it's a foreign Var */
        Var* var                  = (Var*)expr;
        CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)(baserel->fdw_private);
        Relids relids;

        if (IS_UPPER_REL(baserel)) {
            relids = fpinfo->outerrel->relids;
        } else {
            relids = baserel->relids;
        }

        if (bms_is_member(var->varno, relids) && var->varlevelsup == 0) {
            return false; /* foreign Var, so not a param */
        } else {
            return true; /* it'd have to be a param */
        }
        break;
    }
    case T_Param:
        /* Params always have to be sent to the foreign server */
        return true;
    default:
        break;
    }
    return false;
}

/*
 * Add typmod decoration to the basic type name. Copied from
 * src/backend/utils/adt/format_type.c in the Postgres source.
 */
static char*
printTypmod(const char* typname, int32 typmod, Oid typmodout) {
    char* res;

    /* Shouldn't be called if typmod is -1 */
    Assert(typmod >= 0);

    if (typmodout == InvalidOid) {
        /* Default behavior: just print the integer typmod with parens */
        res = psprintf("%s(%d)", typname, (int)typmod);
    } else {
        /* Use the type-specific typmodout procedure */
        char* tmstr;

        tmstr = DatumGetCString(OidFunctionCall1(typmodout, Int32GetDatum(typmod)));
        res   = psprintf("%s%s", typname, tmstr);
    }

    return res;
}

static char*
ch_format_type_extended(Oid type_oid, int32 typemod, uint16 flags) {
    HeapTuple tuple;
    Form_pg_type typeform;
    Oid array_base_type;
    bool is_array;
    char* buf;
    bool with_typemod;

    tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
    if (!HeapTupleIsValid(tuple)) {
        elog(ERROR, "pg_clickhouse: cache lookup failed for type %u", type_oid);
    }

    typeform = (Form_pg_type)GETSTRUCT(tuple);

    /*
     * Check if it's a regular (variable length) array type. Fixed-length
     * array types such as "name" shouldn't get deconstructed. As of Postgres
     * 8.1, rather than checking typlen we check the toast property, and don't
     * deconstruct "plain storage" array types --- this is because we don't
     * want to show oidvector as oid[].
     */
    array_base_type = typeform->typelem;

    if (array_base_type != InvalidOid && typeform->typstorage != 'p') {
        /* Switch our attention to the array element type */
        ReleaseSysCache(tuple);
        tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(array_base_type));
        if (!HeapTupleIsValid(tuple)) {
            elog(ERROR, "pg_clickhouse: cache lookup failed for type %u", type_oid);
        }

        typeform = (Form_pg_type)GETSTRUCT(tuple);
        type_oid = array_base_type;
        is_array = true;
    } else {
        is_array = false;
    }

    with_typemod = (flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0 && (typemod >= 0);

    /*
     * See if we want to special-case the output for certain built-in types.
     * Note that these special cases should all correspond to special
     * productions in gram.y, to ensure that the type name will be taken as a
     * system type, not a user type of the same name.
     *
     * If we do not provide a special-case output here, the type name will be
     * handled the same way as a user type name --- in particular, it will be
     * double-quoted if it matches any lexer keyword. This behavior is
     * essential for some cases, such as types "bit" and "char".
     */
    buf = NULL; /* flag for no special case */

    switch (type_oid) {
    case BOOLOID:
        buf = pstrdup("Boolean");
        break;

    case BPCHAROID:
        if (with_typemod) {
            buf = printTypmod("FixedString", typemod, typeform->typmodout);
        } else if ((flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0) {
            /*
             * bpchar with typmod -1 is not the same as CHARACTER, which
             * means CHARACTER(1) per SQL spec. Report it as bpchar so
             * that parser will not assign a bogus typmod.
             */
        } else {
            buf = pstrdup("String");
        }
        break;

    case FLOAT4OID:
        buf = pstrdup("Float32");
        break;

    case FLOAT8OID:
        buf = pstrdup("Float64");
        break;

    case INT2OID:
        buf = pstrdup("Int16");
        break;

    case INT4OID:
        buf = pstrdup("Int32");
        break;

    case INT8OID:
        buf = pstrdup("Int64");
        break;

    case NUMERICOID:
        if (with_typemod) {
            /*
             * Unconstrained numeric potentially much larger than ClickHouse can
               do; max it out when no precision specified.
            */
            buf = typemod < 0 ? "Decimal(76, 38)"
                              : printTypmod("Decimal", typemod, typeform->typmodout);
        } else {
            buf = pstrdup("Decimal");
        }
        break;

    case INTERVALOID:
        if (with_typemod) {
            buf = printTypmod("UInt64", typemod, typeform->typmodout);
        } else {
            buf = pstrdup("UInt64");
        }
        break;

    case TIMESTAMPTZOID:
    case TIMESTAMPOID:
        buf = pstrdup("DateTime");
        break;
    case DATEOID:
        buf = pstrdup("Date");
        break;

    case VARCHAROID:
        if (with_typemod) {
            buf = printTypmod("FixedString", typemod, typeform->typmodout);
        } else {
            buf = pstrdup("String");
        }
        break;
    case TEXTOID:
        buf = pstrdup("String");
        break;
    }

    if (buf == NULL) {
        CustomObjectDef* cdef;
        char* typname;

        cdef = chfdw_check_for_custom_type(type_oid);
        if (cdef && cdef->custom_name[0] != '\0') {
            buf = pstrdup(cdef->custom_name);
        } else {
            typname = NameStr(typeform->typname);
            buf     = quote_qualified_identifier(NULL, typname);

            if (with_typemod) {
                buf = printTypmod(buf, typemod, typeform->typmodout);
            }
        }
    }

    if (is_array) {
        buf = psprintf("Array(%s)", buf);
    }

    ReleaseSysCache(tuple);

    return buf;
}

/*
 * Convert type OID + typmod info into a type name we can ship to the remote
 * server. Someplace else had better have verified that this type name is
 * expected to be known on the remote end.
 *
 * This is almost just format_type_with_typemod(), except that if left to its
 * own devices, that function will make schema-qualification decisions based
 * on the local search_path, which is wrong. We must schema-qualify all
 * type names that are not in pg_catalog. We assume here that built-in types
 * are all in pg_catalog and need not be qualified; otherwise, qualify.
 */
static char*
deparse_type_name(Oid type_oid, int32 typemod) {
    uint16 flags = FORMAT_TYPE_TYPEMOD_GIVEN;

    if (!chfdw_is_builtin(type_oid)) {
        flags |= FORMAT_TYPE_FORCE_QUALIFY;
    }

    return ch_format_type_extended(type_oid, typemod, flags);
}

/*
 * Build the targetlist for given relation to be deparsed as SELECT clause.
 *
 * The output targetlist contains the columns that need to be fetched from the
 * foreign server for the given relation. If foreignrel is an upper relation,
 * then the output targetlist can also contain expressions to be evaluated on
 * foreign server.
 */
List*
chfdw_build_tlist_to_deparse(RelOptInfo* foreignrel) {
    List* tlist               = NIL;
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;
    ListCell* lc;

    /*
     * For an upper relation, we have already built the target list while
     * checking shippability, so just return that.
     */
    if (IS_UPPER_REL(foreignrel)) {
        return fpinfo->grouped_tlist;
    }

    /*
     * We require columns specified in foreignrel->reltarget->exprs and those
     * required for evaluating the local conditions.
     */
    tlist = add_to_flat_tlist(
        tlist,
        pull_var_clause((Node*)foreignrel->reltarget->exprs, PVC_RECURSE_PLACEHOLDERS)
    );
    foreach (lc, fpinfo->local_conds) {
        RestrictInfo* rinfo = lfirst_node(RestrictInfo, lc);

        tlist = add_to_flat_tlist(
            tlist, pull_var_clause((Node*)rinfo->clause, PVC_RECURSE_PLACEHOLDERS)
        );
    }

    return tlist;
}

/*
 * Deparse SELECT statement for given relation into buf.
 *
 * tlist contains the list of desired columns to be fetched from foreign server.
 * For a base relation fpinfo->attrs_used is used to construct SELECT clause,
 * hence the tlist is ignored for a base relation.
 *
 * remote_conds is the list of conditions to be deparsed into the WHERE clause
 * (or, in the case of upper relations, into the HAVING clause).
 *
 * If params_list is not NULL, it receives a list of Params and other-relation
 * Vars used in the clauses; these values must be transmitted to the remote
 * server as parameter values.
 *
 * If params_list is NULL, we're generating the query for EXPLAIN purposes,
 * so Params and other-relation Vars should be replaced by dummy values.
 *
 * pathkeys is the list of pathkeys to order the result by.
 *
 * is_subquery is the flag to indicate whether to deparse the specified
 * relation as a subquery.
 *
 * List of columns selected is returned in retrieved_attrs.
 */
void
chfdw_deparse_select_stmt_for_rel(
    StringInfo buf,
    PlannerInfo* root,
    RelOptInfo* rel,
    List* tlist,
    List* remote_conds,
    List* pathkeys,
    bool has_final_sort,
    bool has_limit,
    bool is_subquery,
    List** retrieved_attrs,
    List** params_list
) {
    deparse_expr_cxt context;
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)rel->fdw_private;
    List* quals;

    elog(DEBUG2, "> %s:%d", __FUNCTION__, __LINE__);

    /*
     * We handle relations for foreign tables, joins between those and upper
     * relations.
     */
    Assert(IS_JOIN_REL(rel) || IS_SIMPLE_REL(rel) || IS_UPPER_REL(rel));

    /* Fill portions of context common to upper, join and base relation */
    memset(&context, 0, sizeof(context));
    context.buf            = buf;
    context.root           = root;
    context.foreignrel     = rel;
    context.scanrel        = IS_UPPER_REL(rel) ? fpinfo->outerrel : rel;
    context.params_list    = params_list;
    context.func           = NULL;
    context.interval_op    = false;
    context.array_as_tuple = false;
    context.no_sort_parens = false;
    context.fpinfo         = fpinfo;

    /*
     * For upper relations, the WHERE clause is built from the remote
     * conditions of the underlying scan relation; otherwise, we can use the
     * supplied list of remote conditions directly.
     */
    if (IS_UPPER_REL(rel)) {
        CHFdwRelationInfo* ofpinfo;

        ofpinfo = (CHFdwRelationInfo*)fpinfo->outerrel->fdw_private;
        quals   = ofpinfo->remote_conds;
    } else {
        quals = remote_conds;
    }

    /*
     * Detect inlined SubPlans before emitting anything: their presence forces
     * r{N} qualification throughout the statement (see has_inlined_subplan).
     * Quals may carry RestrictInfo decoration, which expression_tree_walker
     * does not look through on all versions, so unwrap manually. remote_conds
     * is checked too: for upper relations it becomes the HAVING clause.
     */
    {
        ListCell* lc;

        foreach (lc, quals) {
            Node* clause = (Node*)lfirst(lc);

            if (IsA(clause, RestrictInfo)) {
                clause = (Node*)((RestrictInfo*)clause)->clause;
            }
            if (contain_subplans(clause)) {
                context.has_inlined_subplan = true;
            }
        }
        foreach (lc, remote_conds) {
            Node* clause = (Node*)lfirst(lc);

            if (IsA(clause, RestrictInfo)) {
                clause = (Node*)((RestrictInfo*)clause)->clause;
            }
            if (contain_subplans(clause)) {
                context.has_inlined_subplan = true;
            }
        }
        if (contain_subplans((Node*)tlist)) {
            context.has_inlined_subplan = true;
        }
    }

    /* Construct SELECT clause */
    deparseSelectSql(tlist, is_subquery, retrieved_attrs, &context);

    /* Construct FROM and WHERE clauses */
    deparseFromExpr(quals, &context);

    if (IS_UPPER_REL(rel)) {
        /* Append GROUP BY clause */
        appendGroupByClause(tlist, &context);

        /* Append HAVING clause */
        if (remote_conds) {
            appendStringInfoString(buf, " HAVING ");
            appendConditions(remote_conds, &context);
        }
    }

    /* Add ORDER BY clause if we found any useful pathkeys */
    if (pathkeys) {
        appendOrderByClause(pathkeys, has_final_sort, &context);
    }

    /* Add LIMIT clause if necessary */
    if (has_limit) {
        appendLimitClause(&context);
    }
}

/*
 * Construct a simple SELECT statement that retrieves desired columns
 * of the specified foreign table, and append it to "buf". The output
 * contains just "SELECT ... ".
 *
 * We also create an integer List of the columns being retrieved, which is
 * returned to *retrieved_attrs, unless we deparse the specified relation
 * as a subquery.
 *
 * tlist is the list of desired columns. is_subquery is the flag to
 * indicate whether to deparse the specified relation as a subquery.
 * Read prologue of chfdw_deparse_select_stmt_for_rel() for details.
 */
static void
deparseSelectSql(
    List* tlist,
    bool is_subquery,
    List** retrieved_attrs,
    deparse_expr_cxt* context
) {
    StringInfo buf            = context->buf;
    RelOptInfo* foreignrel    = context->foreignrel;
    PlannerInfo* root         = context->root;
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;

    /* reset var counter */
    var_counter = 0;
    elog(DEBUG2, "> %s:%d", __FUNCTION__, __LINE__);

    /*
     * Construct SELECT list
     */
    appendStringInfoString(buf, "SELECT ");

    if (is_subquery) {
        /*
         * For a relation that is deparsed as a subquery, emit expressions
         * specified in the relation's reltarget. Note that since this is for
         * the subquery, no need to care about *retrieved_attrs.
         */
        deparseSubqueryTargetList(context);
    } else if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel)) {
        /*
         * For a join or upper relation the input tlist gives the list of
         * columns required to be fetched from the foreign server.
         */
        deparseExplicitTargetList(tlist, retrieved_attrs, context);
    } else {
        /*
         * For a base relation fpinfo->attrs_used gives the list of columns
         * required to be fetched from the foreign server.
         */
        RangeTblEntry* rte = planner_rt_fetch(foreignrel->relid, root);

        /*
         * Core code already has some lock on each rel being planned, so we
         * can use NoLock here.
         */
        Relation rel = table_open_compat(rte->relid, NoLock);

        deparseTargetList(
            buf, rte, foreignrel->relid, rel, fpinfo->attrs_used, false, retrieved_attrs
        );
        table_close_compat(rel, NoLock);
    }
    elog(DEBUG2, "< %s:%d", __FUNCTION__, __LINE__);
}

/*
 * Construct a FROM clause and, if needed, a WHERE clause, and append those to
 * "buf".
 *
 * quals is the list of clauses to be included in the WHERE clause.
 * (These may or may not include RestrictInfo decoration.)
 */
static void
deparseFromExpr(List* quals, deparse_expr_cxt* context) {
    StringInfo buf      = context->buf;
    RelOptInfo* scanrel = context->scanrel;

    /* For upper relations, scanrel must be either a joinrel or a baserel */
    Assert(
        !IS_UPPER_REL(context->foreignrel) || IS_JOIN_REL(scanrel) ||
        IS_SIMPLE_REL(scanrel)
    );

    /* Construct FROM clause */
    appendStringInfoString(buf, " FROM ");
    deparseFromExprForRel(
        buf,
        context->root,
        scanrel,
        (bms_num_members(scanrel->relids) > 1 || context->has_inlined_subplan),
        (Index)0,
        NULL,
        context->params_list
    );

    /* Construct WHERE clause */
    if (quals != NIL) {
        appendStringInfoString(buf, " WHERE ");
        appendConditions(quals, context);
    }
}

/*
 * Emit a target list that retrieves the columns specified in attrs_used.
 * This is used for both SELECT and RETURNING targetlists; the is_returning
 * parameter is true only for a RETURNING targetlist.
 *
 * The tlist text is appended to buf, and we also create an integer List
 * of the columns being retrieved, which is returned to *retrieved_attrs.
 *
 * If qualify_col is true, add relation alias before the column name.
 */
static void
deparseTargetList(
    StringInfo buf,
    RangeTblEntry* rte,
    Index rtindex,
    Relation rel,
    Bitmapset* attrs_used,
    bool qualify_col,
    List** retrieved_attrs
) {
    TupleDesc tupdesc = RelationGetDescr(rel);
    bool have_wholerow;
    bool first;
    int i;

    *retrieved_attrs = NIL;

    /* If there's a whole-row reference, we'll need all the columns. */
    have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, attrs_used);

    first = true;
    for (i = 1; i <= tupdesc->natts; i++) {
        CustomObjectDef* cdef;
        Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

        /* Ignore dropped attributes. */
        if (attr->attisdropped) {
            continue;
        }

        if (have_wholerow ||
            bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used)) {
            if (!first) {
                appendStringInfoString(buf, ", ");
            }

            first = false;

            cdef = chfdw_check_for_custom_type(attr->atttypid);
            deparseColumnRef(buf, cdef, rtindex, i, rte, qualify_col);

            *retrieved_attrs = lappend_int(*retrieved_attrs, i);
        }
    }

    /*
     * check for ctid and oid
     */
    if (bms_is_member(
            SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber,
            attrs_used
        )) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("clickhouse does not support system columns")
        );
    }

    /* Don't generate bad syntax if no undropped columns */
    if (first) {
        appendStringInfoString(buf, "NULL");
    }
}

/*
 * Deparse conditions from the provided list and append them to buf.
 *
 * The conditions in the list are assumed to be ANDed. This function is used to
 * deparse WHERE clauses, JOIN .. ON clauses and HAVING clauses.
 *
 * Depending on the caller, the list elements might be either RestrictInfos
 * or bare clauses.
 */
static void
appendConditions(List* exprs, deparse_expr_cxt* context) {
    ListCell* lc;
    bool is_first  = true;
    StringInfo buf = context->buf;

    foreach (lc, exprs) {
        Expr* expr = (Expr*)lfirst(lc);

        /* Extract clause from RestrictInfo, if required */
        if (IsA(expr, RestrictInfo)) {
            expr = ((RestrictInfo*)expr)->clause;
        }

        /* Connect expressions with "AND" and parenthesize each condition. */
        if (!is_first) {
            appendStringInfoString(buf, " AND ");
        }

        appendStringInfoChar(buf, '(');
        deparseExpr(expr, context);
        appendStringInfoChar(buf, ')');

        is_first = false;
    }
}

/* Output join name for given join type */
const char*
chfdw_get_jointype_name(JoinType jointype) {
    switch (jointype) {
    case JOIN_INNER:
        return "INNER";

    case JOIN_LEFT:
        return "LEFT";

    case JOIN_RIGHT:
        return "RIGHT";

    case JOIN_FULL:
        return "FULL";

    case JOIN_SEMI:
        return "LEFT SEMI";

    case JOIN_ANTI:
        return "LEFT ANTI";

    default:
        /* Shouldn't come here, but protect from buggy code. */
        elog(ERROR, "unsupported join type %d", jointype);
    }

    /* Keep compiler happy */
    return NULL;
}

/*
 * Deparse given targetlist and append it to context->buf.
 *
 * tlist is list of TargetEntry's which in turn contain Var nodes.
 *
 * retrieved_attrs is the list of continuously increasing integers starting
 * from 1. It has same number of entries as tlist.
 */
static void
deparseExplicitTargetList(
    List* tlist,
    List** retrieved_attrs,
    deparse_expr_cxt* context
) {
    ListCell* lc;
    StringInfo buf = context->buf;
    int i          = 0;

    *retrieved_attrs = NIL;

    foreach (lc, tlist) {
        TargetEntry* tle = lfirst_node(TargetEntry, lc);

        if (i > 0) {
            appendStringInfoString(buf, ", ");
        }

        deparseExpr((Expr*)tle->expr, context);

        *retrieved_attrs = lappend_int(*retrieved_attrs, i + 1);
        i++;
    }

    if (i == 0) {
        appendStringInfoString(buf, "NULL");
    }
}

/*
 * Emit expressions specified in the given relation's reltarget.
 *
 * This is used for deparsing the given relation as a subquery.
 */
static void
deparseSubqueryTargetList(deparse_expr_cxt* context) {
    StringInfo buf         = context->buf;
    RelOptInfo* foreignrel = context->foreignrel;
    bool first;
    ListCell* lc;

    /* Should only be called in these cases. */
    Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

    first = true;
    foreach (lc, foreignrel->reltarget->exprs) {
        Node* node = (Node*)lfirst(lc);

        if (!first) {
            appendStringInfoString(buf, ", ");
        }

        first = false;

        deparseExpr((Expr*)node, context);
    }

    /* Don't generate bad syntax if no expressions */
    if (first) {
        appendStringInfoString(buf, "NULL");
    }
}

/*
 * Construct FROM clause for given relation
 *
 * The function constructs ... JOIN ... ON ... for join relation. For a base
 * relation it just returns schema-qualified tablename, with the appropriate
 * alias if so requested.
 *
 * 'ignore_rel' is either zero or the RT index of a target relation. In the
 * latter case the function constructs FROM clause of UPDATE or USING clause
 * of DELETE; it deparses the join relation as if the relation never contained
 * the target relation, and creates a List of conditions to be deparsed into
 * the top-level WHERE clause, which is returned to *ignore_conds.
 */
static void
deparseFromExprForRel(
    StringInfo buf,
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    bool use_alias,
    Index ignore_rel,
    List** ignore_conds,
    List** params_list
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;

    if (IS_JOIN_REL(foreignrel)) {
        StringInfoData join_sql_o;
        StringInfoData join_sql_i;
        RelOptInfo* outerrel    = fpinfo->outerrel;
        RelOptInfo* innerrel    = fpinfo->innerrel;
        bool outerrel_is_target = false;
        bool innerrel_is_target = false;

        if (ignore_rel > 0 && bms_is_member(ignore_rel, foreignrel->relids)) {
            /*
             * If this is an inner join, add joinclauses to *ignore_conds and
             * set it to empty so that those can be deparsed into the WHERE
             * clause. Note that since the target relation can never be within
             * the nullable side of an outer join, those could safely be
             * pulled up into the WHERE clause (see foreign_join_ok()). Note
             * also that since the target relation is only inner-joined to any
             * other relation in the query, all conditions in the join tree
             * mentioning the target relation could be deparsed into the WHERE
             * clause by doing this recursively.
             */
            if (fpinfo->jointype == JOIN_INNER) {
                *ignore_conds =
                    list_concat(*ignore_conds, list_copy(fpinfo->joinclauses));
                fpinfo->joinclauses = NIL;
            }

            /*
             * Check if either of the input relations is the target relation.
             */
            if (outerrel->relid == ignore_rel) {
                outerrel_is_target = true;
            } else if (innerrel->relid == ignore_rel) {
                innerrel_is_target = true;
            }
        }

        /* Deparse outer relation if not the target relation. */
        if (!outerrel_is_target) {
            initStringInfo(&join_sql_o);
            deparseRangeTblRef(
                &join_sql_o,
                root,
                outerrel,
                fpinfo->make_outerrel_subquery,
                ignore_rel,
                ignore_conds,
                params_list
            );

            /*
             * If inner relation is the target relation, skip deparsing it.
             * Note that since the join of the target relation with any other
             * relation in the query is an inner join and can never be within
             * the nullable side of an outer join, the join could be
             * interchanged with higher-level joins (cf. identity 1 on outer
             * join reordering shown in src/backend/optimizer/README), which
             * means it's safe to skip the target-relation deparsing here.
             */
            if (innerrel_is_target) {
                Assert(fpinfo->jointype == JOIN_INNER);
                Assert(fpinfo->joinclauses == NIL);
                appendStringInfoString(buf, join_sql_o.data);
                return;
            }
        }

        /* Deparse inner relation if not the target relation. */
        if (!innerrel_is_target) {
            initStringInfo(&join_sql_i);
            deparseRangeTblRef(
                &join_sql_i,
                root,
                innerrel,
                fpinfo->make_innerrel_subquery,
                ignore_rel,
                ignore_conds,
                params_list
            );

            /*
             * If outer relation is the target relation, skip deparsing it.
             * See the above note about safety.
             */
            if (outerrel_is_target) {
                Assert(fpinfo->jointype == JOIN_INNER);
                Assert(fpinfo->joinclauses == NIL);
                appendStringInfoString(buf, join_sql_i.data);
                return;
            }
        }

        /* Neither of the relations is the target relation. */
        Assert(!outerrel_is_target && !innerrel_is_target);

        /*
         * For a join relation FROM clause entry is deparsed as
         *
         * ((outer relation) <join type> (inner relation) ON (joinclauses))
         *
         * ClickHouse doesn't use ALL modifier for SEMI/ANTI joins.
         */
        if (fpinfo->jointype == JOIN_SEMI || fpinfo->jointype == JOIN_ANTI) {
            appendStringInfo(
                buf,
                " %s %s JOIN %s ON ",
                join_sql_o.data,
                chfdw_get_jointype_name(fpinfo->jointype),
                join_sql_i.data
            );
        } else {
            appendStringInfo(
                buf,
                " %s ALL %s JOIN %s ON ",
                join_sql_o.data,
                chfdw_get_jointype_name(fpinfo->jointype),
                join_sql_i.data
            );
        }

        /* Append join clause; (TRUE) if no join clause */
        if (fpinfo->joinclauses) {
            deparse_expr_cxt context;

            memset(&context, 0, sizeof(context));
            context.buf            = buf;
            context.foreignrel     = foreignrel;
            context.scanrel        = foreignrel;
            context.root           = root;
            context.params_list    = params_list;
            context.func           = NULL;
            context.interval_op    = false;
            context.array_as_tuple = false;
            context.no_sort_parens = false;
            context.fpinfo         = fpinfo;

            appendStringInfoChar(buf, '(');
            appendConditions(fpinfo->joinclauses, &context);
            appendStringInfoChar(buf, ')');
        } else {
            appendStringInfoString(buf, "(TRUE)");
        }
    } else {
        RangeTblEntry* rte = planner_rt_fetch(foreignrel->relid, root);

        /*
         * Core code already has some lock on each rel being planned, so we
         * can use NoLock here.
         */
        Relation rel = table_open_compat(rte->relid, NoLock);

        deparseRelation(buf, rel);

        /*
         * Add a unique alias to avoid any conflict in relation names due to
         * pulled up subqueries in the query being built for a pushed down
         * join.
         */
        if (use_alias) {
            appendStringInfo(buf, " %s%d", REL_ALIAS_PREFIX, foreignrel->relid);
        }

        table_close_compat(rel, NoLock);
    }
}

/*
 * Append FROM clause entry for the given relation into buf.
 */
static void
deparseRangeTblRef(
    StringInfo buf,
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    bool make_subquery,
    Index ignore_rel,
    List** ignore_conds,
    List** params_list
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;

    /* Should only be called in these cases. */
    Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

    Assert(fpinfo->local_conds == NIL);

    /* If make_subquery is true, deparse the relation as a subquery. */
    if (make_subquery) {
        List* retrieved_attrs;
        int ncols;

        /*
         * The given relation shouldn't contain the target relation, because
         * this should only happen for input relations for a full join, and
         * such relations can never contain an UPDATE/DELETE target.
         */
        Assert(ignore_rel == 0 || !bms_is_member(ignore_rel, foreignrel->relids));

        /* Deparse the subquery representing the relation. */
        appendStringInfoChar(buf, '(');
        chfdw_deparse_select_stmt_for_rel(
            buf,
            root,
            foreignrel,
            NIL,
            fpinfo->remote_conds,
            NIL,
            false,
            false,
            true,
            &retrieved_attrs,
            params_list
        );
        appendStringInfoChar(buf, ')');

        /* Append the relation alias. */
        appendStringInfo(
            buf, " %s%d", SUBQUERY_REL_ALIAS_PREFIX, fpinfo->relation_index
        );

        /*
         * Append the column aliases if needed. Note that the subquery emits
         * expressions specified in the relation's reltarget (see
         * deparseSubqueryTargetList).
         */
        ncols = list_length(foreignrel->reltarget->exprs);
        if (ncols > 0) {
            int i;

            appendStringInfoChar(buf, '(');
            for (i = 1; i <= ncols; i++) {
                if (i > 1) {
                    appendStringInfoString(buf, ", ");
                }

                appendStringInfo(buf, "%s%d", SUBQUERY_COL_ALIAS_PREFIX, i);
            }
            appendStringInfoChar(buf, ')');
        }
    } else {
        deparseFromExprForRel(
            buf, root, foreignrel, true, ignore_rel, ignore_conds, params_list
        );
    }
}

/*
 * deparse remote INSERT statement
 */
char*
chfdw_deparse_insert_sql(
    StringInfo buf,
    RangeTblEntry* rte,
    Index rtindex,
    Relation rel,
    List* targetAttrs
) {
    bool first;
    ListCell* lc;
    StringInfoData table_name;

    initStringInfo(&table_name);
    appendStringInfoString(buf, "INSERT INTO ");
    deparseRelation(&table_name, rel);
    appendStringInfoString(buf, table_name.data);

    if (targetAttrs) {
        appendStringInfoChar(buf, '(');

        first = true;
        foreach (lc, targetAttrs) {
            int attnum = lfirst_int(lc);

            if (!first) {
                appendStringInfoString(buf, ", ");
            }

            first = false;

            deparseColumnRef(buf, NULL, rtindex, attnum, rte, false);
        }
        appendStringInfoChar(buf, ')');
    }

    return table_name.data;
}

/*
 * Construct name to use for given column, and emit it into buf.
 * If it has a column_name FDW option, use that instead of attribute name.
 *
 * If qualify_col is true, qualify column name with the alias of relation.
 */
static void
deparseColumnRef(
    StringInfo buf,
    CustomObjectDef* cdef,
    int varno,
    int varattno,
    RangeTblEntry* rte,
    bool qualify_col
) {
    char* colname = NULL;
    CustomColumnInfo* cinfo;

    /* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
    Assert(!IS_SPECIAL_VARNO(varno));

    if (varattno <= 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("ClickHouse does not support system attributes")
        );
    }

    /* Get FDW specific options for this column */
    cinfo = chfdw_get_custom_column_info(rte->relid, varattno);
    if (cinfo) {
        colname = cinfo->colname;
    }

    /*
     * If it's a column of a regular table or it doesn't have column_name FDW
     * option, use attribute name.
     */
    if (colname == NULL) {
        colname = get_attname(rte->relid, varattno, false);
    }

    if (qualify_col) {
        ADD_REL_QUALIFIER(buf, varno);
    }
    appendStringInfoString(buf, quote_identifier(colname));
}

/*
 * Append remote name of specified foreign table to buf.
 * Use value of table_name FDW option (if any) instead of relation's name.
 * Similarly, schema_name FDW option overrides schema name.
 */
static void
deparseRelation(StringInfo buf, Relation rel) {
    ForeignTable* table;
    const char* relname   = NULL;
    char* dbname          = "default";
    ForeignServer* server = chfdw_get_foreign_server(rel);
    ListCell* lc;

    chfdw_extract_options(
        server->options, NULL, NULL, NULL, &dbname, NULL, NULL, NULL, NULL, NULL
    );

    /* obtain additional catalog information. */
    table = GetForeignTable(RelationGetRelid(rel));

    /*
     * Use value of FDW options if any, instead of the name of object itself.
     */
    foreach (lc, table->options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (strcmp(def->defname, "table_name") == 0) {
            relname = defGetString(def);
        } else if (strcmp(def->defname, "database") == 0) {
            dbname = defGetString(def);
        }
    }
    if (relname == NULL) {
        relname = RelationGetRelationName(rel);
    }

    appendStringInfo(buf, "%s.%s", quote_identifier(dbname), quote_identifier(relname));
}

/*
 * Append a SQL string literal representing "val" to buf.
 */
static void
deparseStringLiteral(StringInfo buf, const char* val) {
    char* quoted = ch_quote_literal(val);

    appendStringInfoString(buf, quoted);
    pfree(quoted);
}

/*
 * Deparse given expression into context->buf.
 *
 * This function must support all the same node types that foreign_expr_walker
 * accepts.
 *
 * Note: unlike ruleutils.c, we just use a simple hard-wired parenthesization
 * scheme: anything more complex than a Var, Const, function call or cast
 * should be self-parenthesized.
 */
static void
deparseExpr(Expr* node, deparse_expr_cxt* context) {
    if (node == NULL) {
        return;
    }

    switch (nodeTag(node)) {
    case T_Var:
        deparseVar((Var*)node, context);
        break;
    case T_Const:
        deparseConst((Const*)node, context, 0);
        break;
    case T_Param:
        deparseParam((Param*)node, context);
        break;
    case T_SubscriptingRef:
        deparseSubscriptingRef((SubscriptingRef*)node, context);
        break;
    case T_FuncExpr:
        deparseFuncExpr((FuncExpr*)node, context);
        break;
    case T_SQLValueFunction:
        deparseSQLValueFunction((SQLValueFunction*)node, context);
        break;
    case T_OpExpr:
        deparseOpExpr((OpExpr*)node, context);
        break;
    case T_DistinctExpr:
        deparseDistinctExpr((DistinctExpr*)node, context);
        break;
    case T_NullIfExpr:
        deparseNullIfExpr((NullIfExpr*)node, context);
        break;
    case T_ScalarArrayOpExpr:
        deparseScalarArrayOpExpr((ScalarArrayOpExpr*)node, context);
        break;
    case T_RelabelType:
        deparseRelabelType((RelabelType*)node, context);
        break;
    case T_BoolExpr:
        deparseBoolExpr((BoolExpr*)node, context);
        break;
    case T_NullTest:
        deparseNullTest((NullTest*)node, context);
        break;
    case T_ArrayExpr:
        deparseArrayExpr((ArrayExpr*)node, context);
        break;
    case T_Aggref:
        deparseAggref((Aggref*)node, context);
        break;
    case T_WindowFunc:
        deparseWindowFunc((WindowFunc*)node, context);
        break;
    case T_CaseExpr:
        deparseCaseExpr((CaseExpr*)node, context);
        break;
    case T_CaseWhen:
        deparseCaseWhen((CaseWhen*)node, context);
        break;
    case T_CoalesceExpr:
        deparseCoalesceExpr((CoalesceExpr*)node, context);
        break;
    case T_MinMaxExpr:
        deparseMinMaxExpr((MinMaxExpr*)node, context);
        break;
    case T_CoerceViaIO:
        deparseCoerceViaIO((CoerceViaIO*)node, context);
        break;
    case T_RowExpr:
        deparseRowExpr((RowExpr*)node, context);
        break;
    case T_SubPlan:
        deparseSubPlan((SubPlan*)node, context);
        break;
    default:
        elog(ERROR, "unsupported expression type for deparse: %d", (int)nodeTag(node));
        break;
    }
}

/*
 * Deparse given Var node into context->buf.
 *
 * If the Var belongs to the foreign relation, just print its remote name.
 * Otherwise, it's effectively a Param (and will in fact be a Param at
 * run time). Handle it the same way we handle plain Params.
 */
static void
deparseVar(Var* node, deparse_expr_cxt* context) {
    CustomObjectDef* cdef;
    Relids relids = context->scanrel->relids;
    int relno;
    int colno;

    /*
     * Qualify columns when multiple relations are involved, or when a SubPlan
     * is inlined anywhere in the statement (unqualified outer columns would
     * be captured by the subquery's scope).
     */
    bool qualify_col = (bms_num_members(relids) > 1 || context->has_inlined_subplan);

    /*
     * SubPlan scope: the Var's varno indexes the subquery's own rtable
     * (context->root is the SubPlan's PlannerInfo here), and the alias
     * namespace is q{plan_id}_{varno} — never the outer r{N}. Pass
     * qualify_col=false to deparseColumnRef so it cannot add an r{N}
     * qualifier of its own.
     */
    if (context->subplan != NULL) {
        Assert(node->varlevelsup == 0);

        cdef = context->func;
        if (!cdef) {
            cdef = chfdw_check_for_custom_type(node->vartype);
        }

        ADD_SUBPLAN_REL_QUALIFIER(context->buf, context->subplan->plan_id, node->varno);
        deparseColumnRef(
            context->buf,
            cdef,
            node->varno,
            node->varattno,
            planner_rt_fetch(node->varno, context->root),
            false
        );
        return;
    }

    /*
     * Outer scope from here down: r{N} / s{N}.c{M} namespaces only (see the
     * INVARIANT comment at SUBPLAN_REL_ALIAS_PREFIX).
     */
    Assert(context->subplan == NULL);

    /*
     * If the Var belongs to the foreign relation that is deparsed as a
     * subquery, use the relation and column alias to the Var provided by the
     * subquery, instead of the remote name.
     */
    if (is_subquery_var(node, context->scanrel, &relno, &colno)) {
        appendStringInfo(
            context->buf,
            "%s%d.%s%d",
            SUBQUERY_REL_ALIAS_PREFIX,
            relno,
            SUBQUERY_COL_ALIAS_PREFIX,
            colno
        );
        return;
    }

    cdef = context->func;
    if (!cdef) {
        cdef = chfdw_check_for_custom_type(node->vartype);
    }

    if (bms_is_member(node->varno, relids) && node->varlevelsup == 0) {
        deparseColumnRef(
            context->buf,
            cdef,
            node->varno,
            node->varattno,
            planner_rt_fetch(node->varno, context->root),
            qualify_col
        );
    } else {
        /* Treat like a Param */
        if (context->params_list) {
            int pindex = 0;
            ListCell* lc;

            /* find its index in params_list */
            foreach (lc, *context->params_list) {
                pindex++;
                if (equal(node, (Node*)lfirst(lc))) {
                    break;
                }
            }
            if (lc == NULL) {
                /* not in list, so add it */
                pindex++;
                *context->params_list = lappend(*context->params_list, node);
            }

            printRemoteParam(pindex, node->vartype, node->vartypmod, context);
        } else {
            printRemotePlaceholder(node->vartype, node->vartypmod, context);
        }
    }
}

#define USE_ISO_DATES 1

Datum
ch_time_out(PG_FUNCTION_ARGS) {
    TimeADT time = PG_GETARG_TIMEADT(0);
    char* result;
    struct pg_tm tt, *tm = &tt;
    fsec_t fsec;
    char buf[MAXDATELEN + 1];

    time2tm(time, tm, &fsec);
    EncodeTimeOnly(tm, fsec, false, 0, USE_ISO_DATES, buf);

    result = pstrdup(buf);
    PG_RETURN_CSTRING(result);
}

/* date_out()
 * Given internal format date, convert to text string.
 */
Datum
ch_date_out(PG_FUNCTION_ARGS) {
    DateADT date = PG_GETARG_DATEADT(0);
    char* result;
    struct pg_tm tt, *tm = &tt;
    char buf[MAXDATELEN + 1];

    if (DATE_NOT_FINITE(date)) {
        EncodeSpecialDate(date, buf);
    } else {
        j2date(
            date + POSTGRES_EPOCH_JDATE, &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday)
        );
        EncodeDateOnly(tm, USE_ISO_DATES, buf);
    }

    result = pstrdup(buf);
    PG_RETURN_CSTRING(result);
}

Datum
ch_timestamp_out(PG_FUNCTION_ARGS) {
    Timestamp timestamp = PG_GETARG_TIMESTAMP(0);
    char* result;
    struct pg_tm tt, *tm = &tt;
    fsec_t fsec;
    char buf[MAXDATELEN + 1];

    if (TIMESTAMP_NOT_FINITE(timestamp)) {
        EncodeSpecialTimestamp(timestamp, buf);
    } else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0) {
        EncodeDateTime(tm, fsec, false, 0, NULL, USE_ISO_DATES, buf);
    } else {
        ereport(
            ERROR,
            errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
            errmsg("timestamp out of range")
        );
    }

    result = pstrdup(buf);
    PG_RETURN_CSTRING(result);
}

/*
 * Emit printable ASCII as-is and \xHH for everything else. PG's bytea_out
 * hex format (\xHHHH...) does not round-trip through CH's string lexer past
 * a single byte: \x consumes exactly two hex digits.
 */
static void
deparseByteaLiteral(StringInfo buf, Datum value) {
    bytea* bp         = DatumGetByteaPP(value);
    const char* bytes = VARDATA_ANY(bp);
    int len           = VARSIZE_ANY_EXHDR(bp);

    appendStringInfoChar(buf, '\'');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];

        if (c == '\'') {
            appendStringInfoString(buf, "\\'");
        } else if (c == '\\') {
            appendStringInfoString(buf, "\\\\");
        } else if (c >= 0x20 && c <= 0x7E) {
            appendStringInfoChar(buf, (char)c);
        } else {
            appendStringInfo(buf, "\\x%02x", c);
        }
    }
    appendStringInfoChar(buf, '\'');
}

static void
emitArrayElement(
    StringInfo buf,
    Datum elt,
    bool isnull,
    Oid element_type,
    Oid typiofunc
) {
    char* extval;

    if (isnull) {
        appendStringInfoString(buf, "NULL");
        return;
    }

    if (element_type == BYTEAOID) {
        deparseByteaLiteral(buf, elt);
        return;
    }

    extval = OidOutputFunctionCall(typiofunc, elt);

    switch (element_type) {
    case INT2OID:
    case INT4OID:
    case INT8OID:
    case OIDOID:
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID: {
        /*
         * No need to quote unless it's a special value such as 'NaN'.
         * See comments in get_const_expr().
         */
        if (strspn(extval, "0123456789+-eE.") == strlen(extval)) {
            if (extval[0] == '+' || extval[0] == '-') {
                appendStringInfo(buf, "(%s)", extval);
            } else {
                appendStringInfoString(buf, extval);
            }
        } else {
            appendStringInfo(buf, "'%s'", extval);
        }
    } break;
    case BOOLOID:
        if (strcmp(extval, "t") == 0) {
            appendStringInfoString(buf, "true");
        } else {
            appendStringInfoString(buf, "false");
        }
        break;
    default:
        deparseStringLiteral(buf, extval);
        break;
    }
    pfree(extval);
}

/*
 * Walk a multi-dim postgres array level by level, emitting nested
 * `[...]` literals so ClickHouse sees the same shape. iter is consumed
 * in row-major order, matching postgres' flat element layout.
 */
static void
emitArrayLevel(
    StringInfo buf,
    int level,
    int ndims,
    int* dims,
    array_iter* iter,
    Oid element_type,
    int16 typlen,
    bool typbyval,
    char typalign,
    Oid typiofunc,
    int* nleaf
) {
    appendStringInfoChar(buf, '[');
    for (int i = 0; i < dims[level]; i++) {
        if (i > 0) {
            appendStringInfoChar(buf, ',');
        }

        if (level + 1 < ndims) {
            emitArrayLevel(
                buf,
                level + 1,
                ndims,
                dims,
                iter,
                element_type,
                typlen,
                typbyval,
                typalign,
                typiofunc,
                nleaf
            );
        } else {
            Datum elt;
            bool isnull;
            int n = (*nleaf)++;

#if PG_VERSION_NUM < 190000
            elt = array_iter_next(iter, &isnull, n, typlen, typbyval, typalign);
#else
            elt = array_iter_next(iter, &isnull, n);
#endif
            emitArrayElement(buf, elt, isnull, element_type, typiofunc);
        }
    }
    appendStringInfoChar(buf, ']');
}

static void
deparseArray(Datum arr, deparse_expr_cxt* context) {
    StringInfo buf      = context->buf;
    AnyArrayType* array = DatumGetAnyArrayP(arr);
    int ndims           = AARR_NDIM(array);
    int* dims           = AARR_DIMS(array);
    Oid element_type    = AARR_ELEMTYPE(array);

    int16 typlen;
    bool typbyval;
    char typalign;
    char typdelim;
    Oid typioparam;
    Oid typiofunc;
    array_iter iter;

    if (context->array_as_tuple && ndims > 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("pg_clickhouse: tuple-formatted arrays must be one-dimensional")
        );
    }

    get_type_io_data(
        element_type,
        IOFunc_output,
        &typlen,
        &typbyval,
        &typalign,
        &typdelim,
        &typioparam,
        &typiofunc
    );

#if PG_VERSION_NUM < 190000
    array_iter_setup(&iter, array);
#else
    array_iter_setup(&iter, array, typlen, typbyval, typalign);
#endif

    if (ndims > 1) {
        int nleaf = 0;

        emitArrayLevel(
            buf,
            0,
            ndims,
            dims,
            &iter,
            element_type,
            typlen,
            typbyval,
            typalign,
            typiofunc,
            &nleaf
        );
    } else {
        int nitems = ArrayGetNItems(ndims, dims);
        char open  = context->array_as_tuple ? '(' : '[';
        char close = context->array_as_tuple ? ')' : ']';

        appendStringInfoChar(buf, open);
        for (int i = 0; i < nitems; i++) {
            Datum elt;
            bool isnull;

            if (i > 0) {
                appendStringInfoChar(buf, ',');
            }

#if PG_VERSION_NUM < 190000
            elt = array_iter_next(&iter, &isnull, i, typlen, typbyval, typalign);
#else
            elt = array_iter_next(&iter, &isnull, i);
#endif
            emitArrayElement(buf, elt, isnull, element_type, typiofunc);
        }
        appendStringInfoChar(buf, close);
    }
}

/*
 * Exported function to convert an array to a ClickHouse string literal array.
 */
char*
chfdw_array_to_ch_literal(Datum arr) {

    deparse_expr_cxt context;

    memset(&context, 0, sizeof(context));
    context.array_as_tuple = false;
    context.buf            = makeStringInfo();
    deparseArray(arr, &context);
    return context.buf->data;
}

/*
 * Deparse given constant value into context->buf.
 *
 * This function has to be kept in sync with ruleutils.c's get_const_expr.
 * As for that function, showtype can be -1 to never show "::typename" decoration,
 * or +1 to always show it, or 0 to show it only if the constant wouldn't be assumed
 * to be the right type by default.
 */
static void
deparseConst(Const* node, deparse_expr_cxt* context, int showtype) {
    StringInfo buf = context->buf;
    Oid typoutput;
    bool typIsVarlena;
    char* extval = NULL;
    bool closebr = false;

    if (node->constisnull) {
        appendStringInfoString(buf, "NULL");
        return;
    }

    if (showtype > 0) {
        appendStringInfoString(buf, "cast(");
    }

    getTypeOutputInfo(node->consttype, &typoutput, &typIsVarlena);

    if (typoutput == F_TIMESTAMPTZ_OUT || typoutput == F_TIMESTAMP_OUT) {
        extval =
            DatumGetCString(DirectFunctionCall1(ch_timestamp_out, node->constvalue));
    } else if (typoutput == F_INTERVAL_OUT) {
        /*
         * basically we can't convert month part since we should know about
         * related timestamp first.
         *
         * for other types we just convert to seconds.
         */
        uint64 sec;
        Interval* ival = DatumGetIntervalP(node->constvalue);
        char bufint8[MAXINT8LEN + 1];

        if (ival->month != 0) {
            ereport(
                ERROR,
                errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                errmsg("cannot convert interval with months into clickhouse")
            );
        }

        sec = 86400 /* sec in day */ * ival->day + (int64)(ival->time / 1000000);
        pg_lltoa(sec, bufint8);
        appendStringInfoString(buf, bufint8);
        goto cleanup;
    } else if (typoutput == F_ARRAY_OUT) {
        deparseArray(node->constvalue, context);
        goto cleanup;
    } else if (node->consttype == BYTEAOID) {
        deparseByteaLiteral(buf, node->constvalue);
        goto cleanup;
    } else {
        extval = OidOutputFunctionCall(typoutput, node->constvalue);
    }

    switch (node->consttype) {
    case INT2OID:
    case INT4OID:
    case INT8OID:
    case OIDOID:
    case FLOAT4OID:
    case FLOAT8OID:
    case NUMERICOID: {
        /*
         * No need to quote unless it's a special value such as 'NaN'.
         * See comments in get_const_expr().
         */
        if (strspn(extval, "0123456789+-eE.") == strlen(extval)) {
            if (extval[0] == '+' || extval[0] == '-') {
                appendStringInfo(buf, "(%s)", extval);
            } else {
                appendStringInfoString(buf, extval);
            }
        } else {
            deparseStringLiteral(buf, extval);
        }
    } break;
    case BITOID:
    case VARBITOID:
        appendStringInfo(buf, "B'%s'", extval);
        break;
    case BOOLOID:
        if (strcmp(extval, "t") == 0) {
            appendStringInfoChar(buf, '1');
        } else {
            appendStringInfoChar(buf, '0');
        }
        break;
    default:
        deparseStringLiteral(buf, extval);
        break;
    }

    if (closebr) {
        appendStringInfoChar(buf, ')');
    }

cleanup:
    if (showtype > 0) {
        appendStringInfo(
            buf, " as %s)", deparse_type_name(node->consttype, node->consttypmod)
        );
    }
    if (extval) {
        pfree(extval);
    }
}

/*
 * Deparse given Param node.
 *
 * If we're generating the query "for real", add the Param to
 * context->params_list if it's not already present, and then use its index
 * in that list as the remote parameter number.  During EXPLAIN, there's
 * no need to identify a parameter number.
 */
static void
deparseParam(Param* node, deparse_expr_cxt* context) {
    /*
     * SubPlan scope: a PARAM_EXEC Param here is (usually) a correlation
     * reference — the planner's replacement for an outer-query Var inside
     * the subquery (SS_replace_correlation_vars). subplan->parParam and
     * subplan->args run in parallel: paramid -> the outer expression that
     * feeds it. Inline that expression, deparsed in the PARENT scope so it
     * picks up the outer query's aliases.
     */
    if (context->subplan != NULL && node->paramkind == PARAM_EXEC) {
        ListCell* pp;
        ListCell* ap;

        Assert(context->parent_ctx != NULL);

        forboth(pp, context->subplan->parParam, ap, context->subplan->args) {
            if (lfirst_int(pp) == node->paramid) {
                appendStringInfoChar(context->buf, '(');
                deparseExpr((Expr*)lfirst(ap), context->parent_ctx);
                appendStringInfoChar(context->buf, ')');
                return;
            }
        }
        /* Not a correlation param; fall through to normal handling. */
    }

    if (context->params_list) {
        int pindex = 0;
        ListCell* lc;

        /* find its index in params_list */
        foreach (lc, *context->params_list) {
            pindex++;
            if (equal(node, (Node*)lfirst(lc))) {
                break;
            }
        }
        if (lc == NULL) {
            /* not in list, so add it */
            pindex++;
            *context->params_list = lappend(*context->params_list, node);
        }

        printRemoteParam(pindex, node->paramtype, node->paramtypmod, context);
    } else {
        printRemotePlaceholder(node->paramtype, node->paramtypmod, context);
    }
}

/*
 * Print the representation of a parameter to be sent to the remote side
 * by param number and remote data type.
 */
static void
printRemoteParam(
    int paramindex,
    Oid paramtype,
    int32 paramtypmod,
    deparse_expr_cxt* context
) {
    StringInfo buf  = context->buf;
    char* ptypename = deparse_type_name(paramtype, paramtypmod);

    appendStringInfo(buf, "{p%d:%s}", paramindex, ptypename);
}

/*
 * Print the representation of a placeholder for a parameter that will be
 * sent to the remote side at execution time.
 *
 * This is used when we're just trying to EXPLAIN the remote query. We don't
 * have the actual value of the runtime parameter yet, and we don't want the
 * remote planner to generate a plan that depends on such a value anyway.
 * Thus, we can't do something simple like "CAST($1 AS paramtype)". Instead,
 * we emit "SELECT CAST(NULL AS Nullable(paramtype))", which ClickHouse's
 * planner should see as an unknown constant value, which is what we want.
 * This might need adjustment if we ever make the planner flatten scalar
 * subqueries.
 */
static void
printRemotePlaceholder(Oid paramtype, int32 paramtypmod, deparse_expr_cxt* context) {
    StringInfo buf  = context->buf;
    char* ptypename = deparse_type_name(paramtype, paramtypmod);

    appendStringInfo(buf, "((SELECT CAST(null AS Nullable(%s))", ptypename);
}

/*
 * Deparse a SubPlan node: the planner's per-row subquery. Emits a real SQL
 * subquery in place, in the shape appropriate to the SubLinkType. The
 * subquery body itself is printed by deparseSubPlanQuery; shippability was
 * established by is_shippable_subplan, so the shapes here are exhaustive.
 */
static void
deparseSubPlan(SubPlan* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    switch (node->subLinkType) {
    case EXISTS_SUBLINK:
        appendStringInfoString(buf, "EXISTS (");
        deparseSubPlanQuery(node, context, SUBPLAN_BODY_QUERY);
        appendStringInfoChar(buf, ')');
        break;
    case EXPR_SUBLINK:
        appendStringInfoChar(buf, '(');
        deparseSubPlanQuery(node, context, SUBPLAN_BODY_QUERY);
        appendStringInfoChar(buf, ')');
        break;
    case ANY_SUBLINK: {
        /*
         * testexpr is OpExpr('=', lhs, Param), enforced by
         * is_shippable_subplan. The LHS belongs to the OUTER scope:
         * deparse it with the current (outer) context.
         */
        OpExpr* op = castNode(OpExpr, node->testexpr);

        appendStringInfoChar(buf, '(');
        deparseExpr((Expr*)linitial(op->args), context);
        appendStringInfoString(buf, " IN (");
        deparseSubPlanQuery(node, context, SUBPLAN_BODY_QUERY);
        appendStringInfoString(buf, "))");
    } break;
    default:
        /* Unreachable unless a new SubLinkType is added above. */
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "pg_clickhouse: unsupported SubLink type for deparse: %d",
                (int)node->subLinkType
            )
        );
    }
}

/*
 * Deparse a SubPlan's WHERE/HAVING qual. The planner's qual preprocessing
 * turns a parse tree's WHERE/HAVING into a bare List of implicitly-ANDed
 * clauses, but a single-expression shape survives in some paths, so accept
 * both. The List case is exactly appendConditions (AND-join, parenthesize
 * each clause); reuse it rather than re-implement.
 */
static void
deparseSubPlanQuals(Node* quals, deparse_expr_cxt* context) {
    if (IsA(quals, List)) {
        appendConditions((List*)quals, context);
    } else {
        deparseExpr((Expr*)quals, context);
    }
}

/*
 * Emit a pushed-down SubPlan subquery's SELECT list (sans the SELECT keyword).
 *
 * This reads like deparseExplicitTargetList but is a different beast: it
 * consumes the SubPlan's *raw Query* targetList, so it must skip resjunk
 * entries (a planned tlist has none), and it short-circuits EXISTS to a
 * constant, since EXISTS ignores the select list entirely.
 */
static void
deparseSubPlanTargetList(deparse_expr_cxt* context) {
    SubPlan* subplan = context->subplan;
    Query* query     = context->root->parse;
    StringInfo buf   = context->buf;
    ListCell* lc;
    bool first = true;

    Assert(subplan != NULL);

    if (subplan->subLinkType == EXISTS_SUBLINK) {
        appendStringInfoChar(buf, '1');
        return;
    }

    foreach (lc, query->targetList) {
        TargetEntry* tle = lfirst_node(TargetEntry, lc);

        if (tle->resjunk) {
            continue;
        }
        if (!first) {
            appendStringInfoString(buf, ", ");
        }
        first = false;
        deparseExpr(tle->expr, context);
    }
}

/*
 * Emit a pushed-down SubPlan subquery's FROM list (sans the FROM keyword).
 *
 * This looks like deparseFromExpr but we cannot use it: there is no planned
 * RelOptInfo for a SubPlan, so it walks the raw Query's jointree fromlist, and
 * it emits the q{plan_id}_ alias namespace (not r{N}) to stay collision-free
 * with the outer query's aliases. Only the deparseRelation leaf is shared.
 */
static void
deparseSubPlanFrom(deparse_expr_cxt* context) {
    SubPlan* subplan = context->subplan;
    Query* query     = context->root->parse;
    StringInfo buf   = context->buf;
    ListCell* lc;
    bool first = true;

    Assert(subplan != NULL);

    foreach (lc, query->jointree->fromlist) {
        RangeTblRef* rtr   = lfirst_node(RangeTblRef, lc);
        RangeTblEntry* rte = rt_fetch(rtr->rtindex, query->rtable);
        Relation rel       = table_open_compat(rte->relid, NoLock);

        if (!first) {
            appendStringInfoString(buf, ", ");
        }
        first = false;
        deparseRelation(buf, rel);
        appendStringInfo(
            buf, " %s%d_%d", SUBPLAN_REL_ALIAS_PREFIX, subplan->plan_id, rtr->rtindex
        );
        table_close_compat(rel, NoLock);
    }
}

/*
 * Print the body of a pushed-down SubPlan as a SQL subquery.
 *
 * We deparse from the SubPlan's *Query* tree (subroot->parse), not from its
 * Plan: the Query preserves the declarative shape (FROM list, quals, GROUP
 * BY) that maps 1:1 onto remote SQL. By this point the planner has already
 * replaced outer-query references with PARAM_EXEC Params in that tree, which
 * deparseParam resolves back to outer-alias text via parent_ctx.
 *
 * Scope discipline: the sub-context's root is the SubPlan's own PlannerInfo,
 * so every varno resolves against the subquery's rtable, and every table
 * gets a q{plan_id}_{rtindex} alias — collision-free with the outer r{N}/
 * s{N} namespaces and with sibling SubPlans.
 */
static void
deparseSubPlanQuery(SubPlan* subplan, deparse_expr_cxt* context, SubPlanBodyForm form) {
    PlannerInfo* subroot;
    Query* query;
    StringInfo buf = context->buf;
    deparse_expr_cxt subctx;

    Assert(context->root->glob != NULL);
    subroot =
        (PlannerInfo*)list_nth(context->root->glob->subroots, subplan->plan_id - 1);
    query = subroot->parse;

    /* Sub-scope context: same buffer, subquery's planner state. */
    memset(&subctx, 0, sizeof(subctx));
    subctx.root        = subroot;
    subctx.foreignrel  = context->foreignrel;
    subctx.scanrel     = context->scanrel;
    subctx.buf         = buf;
    subctx.params_list = context->params_list;
    subctx.fpinfo      = context->fpinfo;
    subctx.subplan     = subplan;
    subctx.parent_ctx  = context;

    /*
     * Emit the subquery clause by clause. The callee per clause is the reuse
     * map: a bespoke deparseSubPlan* helper where the SubPlan form genuinely
     * differs from the planned-relation path, or a shared helper
     * (appendConditions / appendGroupByClause) where it does not.
     */
    appendStringInfoString(buf, "SELECT ");
    if (form == SUBPLAN_BODY_NULL_PROBE) {
        appendStringInfoChar(buf, '1');
    } else {
        deparseSubPlanTargetList(&subctx);
    }

    appendStringInfoString(buf, " FROM ");
    deparseSubPlanFrom(&subctx);

    if (query->jointree->quals != NULL) {
        appendStringInfoString(buf, " WHERE ");
        deparseSubPlanQuals(query->jointree->quals, &subctx);
    }

    /*
     * The null-probe form asks whether any row the subquery would emit has a
     * NULL output. classify_notin_subplan only admits ungrouped bodies here,
     * so appending the qual to the row-level WHERE asks exactly that.
     */
    if (form == SUBPLAN_BODY_NULL_PROBE) {
        TargetEntry* out = NULL;
        ListCell* lc;

        foreach (lc, query->targetList) {
            TargetEntry* tle = lfirst_node(TargetEntry, lc);

            if (!tle->resjunk) {
                out = tle;
                break;
            }
        }
        Assert(out != NULL); /* classify_notin_subplan guarantees it */

        appendStringInfoString(
            buf, query->jointree->quals != NULL ? " AND (" : " WHERE ("
        );
        deparseExpr(out->expr, &subctx);
        appendStringInfoString(buf, " IS NULL)");
    }

    /* Keys off context->root->parse, which is the subquery here. */
    appendGroupByClause(query->targetList, &subctx);

    if (query->havingQual != NULL) {
        appendStringInfoString(buf, " HAVING ");
        deparseSubPlanQuals(query->havingQual, &subctx);
    }
}

/*
 * Deparse Postgres's `x NOT IN (SELECT ..)` — arriving as NOT over an ANY
 * SubPlan — when a NULL could reach the comparison. ClickHouse's native
 * `NOT IN` computes the plain set complement, where Postgres's three-valued
 * answer can only be TRUE if the probe is non-NULL, the subquery output
 * holds no NULL, and the probe matches nothing — except that an empty
 * result is TRUE regardless of the probe. In a truth context (the only
 * place the walker admits this form, via EXPR_CTX_NEGATED) that TRUE-set
 * is all that matters, so emit it directly:
 *
 *   CASE WHEN probe IS NULL
 *        THEN NOT EXISTS (SELECT ..)                            -- empty?
 *        ELSE probe NOT IN (SELECT ..)
 *             AND NOT EXISTS (SELECT 1 .. AND (output IS NULL)) -- poison?
 *   END
 *
 * Each piece a plan-time proof makes unnecessary is left out: a non-NULL
 * probe drops the `CASE` (`x NOT IN (SELECT .. WHERE false)` is already
 * TRUE on both systems for a non-NULL probe), and a non-NULL output column
 * drops the poison guard. All subqueries are uncorrelated with each other
 * and evaluated once.
 */
static void
deparseGuardedNotIn(SubPlan* subplan, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    OpExpr* op     = castNode(OpExpr, subplan->testexpr);
    Expr* probe    = (Expr*)linitial(op->args);
    NotInNullability nulls =
        notin_prove_nullability(subplan, context->root, context->scanrel->relids);

    if (nulls & NOTIN_PROBE_NULLABLE) {
        appendStringInfoString(buf, "(CASE WHEN (");
        deparseExpr(probe, context);
        appendStringInfoString(buf, ") IS NULL THEN NOT EXISTS (");
        deparseSubPlanQuery(subplan, context, SUBPLAN_BODY_QUERY);
        appendStringInfoString(buf, ") ELSE ");
    }

    appendStringInfoChar(buf, '(');
    deparseExpr(probe, context);
    appendStringInfoString(buf, " NOT IN (");
    deparseSubPlanQuery(subplan, context, SUBPLAN_BODY_QUERY);
    appendStringInfoChar(buf, ')');
    if (nulls & NOTIN_OUTPUT_NULLABLE) {
        appendStringInfoString(buf, " AND NOT EXISTS (");
        deparseSubPlanQuery(subplan, context, SUBPLAN_BODY_NULL_PROBE);
        appendStringInfoChar(buf, ')');
    }
    appendStringInfoChar(buf, ')');

    if (nulls & NOTIN_PROBE_NULLABLE) {
        appendStringInfoString(buf, " END)");
    }
}

/*
 * Deparse an array subscript expression.
 */
static void
deparseSubscriptingRef(SubscriptingRef* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    if (node->reflowerindexpr != NIL) {
        /*
         * Slice: CH doesn't support [L:U] syntax, emit arraySlice(). PG slice
         * is inclusive both ends, CH arraySlice takes (arr, offset, length).
         * Only 1D arrays are supported (enforced elsewhere).
         */
        Expr* lower = (Expr*)linitial(node->reflowerindexpr);
        Expr* upper = (Expr*)linitial(node->refupperindexpr);

        appendStringInfoString(buf, "arraySlice(");
        deparseExpr(node->refexpr, context);
        appendStringInfoString(buf, ", ");

        if (lower) {
            deparseExpr(lower, context);
        } else {
            appendStringInfoChar(buf, '1');
        }

        if (upper) {
            appendStringInfoString(buf, ", (");
            deparseExpr(upper, context);
            appendStringInfoString(buf, ") - (");
            if (lower) {
                deparseExpr(lower, context);
            } else {
                appendStringInfoChar(buf, '1');
            }
            appendStringInfoString(buf, ") + 1");
        }

        appendStringInfoChar(buf, ')');
    } else {
        /* Single element: emit arr[idx] */
        appendStringInfoChar(buf, '(');
        if (IsA(node->refexpr, Var)) {
            deparseExpr(node->refexpr, context);
        } else {
            appendStringInfoChar(buf, '(');
            deparseExpr(node->refexpr, context);
            appendStringInfoChar(buf, ')');
        }
        appendStringInfoChar(buf, '[');
        deparseExpr((Expr*)linitial(node->refupperindexpr), context);
        appendStringInfoString(buf, "])");
    }
}

/*
 * Deparse jsonb_extract_path_text() / jsonb_extract_path() into ClickHouse
 * JSON dot notation.
 *
 *   jsonb_extract_path_text(col, 'k1', 'k2') → col."k1"."k2"
 *   jsonb_extract_path(col, 'k1')            → toJSONString(col."k1")
 *
 * The planner presents these in variadic form: two args where the second
 * is a text[] constant holding the path keys.
 */
static void
deparseJsonbExtractPath(FuncExpr* node, deparse_expr_cxt* context, bool wrap_json) {
    StringInfo buf = context->buf;
    Const* arr     = (Const*)lsecond(node->args);
    ArrayType* array;
    Datum* elems;
    bool* nulls;
    int nelems;

    if (wrap_json) {
        appendStringInfoString(buf, "toJSONString(");
    }

    /* First arg is the JSONB column expression. */
    deparseExpr((Expr*)linitial(node->args), context);

    /* Second arg is text[] with path keys → dot notation. */
    array = DatumGetArrayTypeP(arr->constvalue);
    deconstruct_array(array, TEXTOID, -1, false, TYPALIGN_INT, &elems, &nulls, &nelems);

    for (int i = 0; i < nelems; i++) {
        char* key = TextDatumGetCString(elems[i]);

        appendStringInfoChar(buf, '.');
        appendStringInfoString(buf, quote_identifier(key));
    }

    if (wrap_json) {
        appendStringInfoChar(buf, ')');
    }
}

/*
 * Utility function to append regular expression flags to `context->buf`. All
 * flags have already been vetted by `regex_flags_ok()`. The flag treatment is:
 *
 *
 *  - 'i' is applied as 'i'
 *  - 'm' is applied as "m-s"
 *  - 'n' is applied as "m-s"
 *  - 'p' is applied as "-s"
 *  - 's' is applied as 's'
 *  - 't' is ignored
 *  - 'w' is applied as 'm'

 *  Whichever of the `[smpw]` flags appears last is the only one applied.
 *
*/
static void
appendRegexFlags(Const* arg, deparse_expr_cxt* context) {
    char* str = TextDatumGetCString(arg->constvalue);
    enum {
        flag_0 = 0,
        flag_i = 1,
        flag_m = 2,
        flag_p = 4,
        flag_s = 8,
        flag_w = 16,
    };
    uint16 flags = flag_0;

    /* Iterate over the flags. */
    while (*str) {
        switch (*str) {
        case 'i':
            flags |= flag_i;
            break;
        case 'm':
        case 'n': /* Postgres new name for m. */
            flags |= flag_m;
            flags &= ~flag_s; /* Cancels out s. */
            flags &= ~flag_p; /* Cancels out p. */
            flags &= ~flag_w; /* Cancels out w. */
            break;
        case 'p':
            flags |= flag_p;
            flags &= ~flag_m; /* Cancels out m. */
            flags &= ~flag_s; /* Cancels out s. */
            flags &= ~flag_w; /* Cancels out w. */
            break;
        case 's':
            flags |= flag_s;
            flags &= ~flag_m; /* Cancels out m. */
            flags &= ~flag_p; /* Cancels out p. */
            flags &= ~flag_w; /* Cancels out w. */
            break;
        case 't': /* Always tight syntax, skip. */
            break;
        case 'w':
            flags |= flag_w;
            flags &= ~flag_m; /* Cancels out m. */
            flags &= ~flag_p; /* Cancels out p. */
            flags &= ~flag_s; /* Cancels out s. */
            break;
        }
        str++;
    }

    if (flags & flag_i) {
        appendStringInfoChar(context->buf, 'i');
    }
    if (flags & flag_m) {
        appendStringInfoString(context->buf, "m-s");
    }
    if (flags & flag_p) {
        appendStringInfoString(context->buf, "-s");
    }
    if (flags & flag_s) {
        appendStringInfoChar(context->buf, 's');
    }
    if (flags & flag_w) {
        appendStringInfoChar(context->buf, 'm');
    }
}

/*
 * Utility function used by the regular expression functions to generate the
 * regular expression argument. It expects the second item in `args` to be the
 * regular expression, and the third, optional item to be the flags. If there
 * are no flags it simply appends the regexp expression. If there are flags,
 * it emits the regular expression as `concat('(?$flags), $regexp)`,
 * delegating the actually flag output to `appendRegexFlags()`.
 */
static void
appendRegex(List* args, deparse_expr_cxt* context) {

    if (list_length(args) <= 2) {
        /* No flags argument, just append the regexp expression. */
        deparseExpr((Expr*)list_nth(args, 1), context);
        return;
    }

    /* Concatenate the flags with the regexp expression. */
    appendStringInfoString(context->buf, "concat('(?");
    appendRegexFlags((Const*)list_nth(args, 2), context);
    appendStringInfoString(context->buf, ")', ");
    deparseExpr((Expr*)list_nth(args, 1), context);
    appendStringInfoChar(context->buf, ')');
}

/*
 * Map a Postgres `to_char()` template into a ClickHouse `formatDateTime()`
 * template.  Only translates keywords whose CH equivalent matches PG output
 * byte-for-byte across supported CH versions.  Any keyword PG recognizes but
 * which cannot translate exactly (case-following names, blank-padded names,
 * ordinal postfix, fill-mode prefixes, fractional seconds, ISO week, etc.)
 * causes refusal.  Outside `"..."` quoted text, an uppercase or lowercase
 * letter PG would bind to one of its date keywords also refuses,
 * so a stray `Y` cannot silently survive as a literal.
 *
 * See PG `DCH_keywords` in `src/backend/utils/adt/formatting.c` for the
 * full PG token grammar.
 */
typedef struct {
    const char* pg;
    const char* ch; /* NULL → recognized keyword we refuse */
} ToCharTok;

static const ToCharTok to_char_toks[] = {
    { "Y,YYY", NULL },
    { "y,yyy", NULL },
    { "SSSSS", NULL },
    { "sssss", NULL },
    { "MONTH", NULL },
    { "Month", NULL },
    { "month", NULL },
    { "YYYY",  "%Y" },
    { "yyyy",  "%Y" },
    { "HH24",  "%H" },
    { "hh24",  "%H" },
    { "HH12",  "%I" },
    { "hh12",  "%I" },
    { "IYYY",  NULL },
    { "iyyy",  NULL },
    { "IDDD",  NULL },
    { "iddd",  NULL },
    { "SSSS",  NULL },
    { "ssss",  NULL },
    { "A.D.",  NULL },
    { "a.d.",  NULL },
    { "B.C.",  NULL },
    { "b.c.",  NULL },
    { "P.M.",  NULL },
    { "p.m.",  NULL },
    { "A.M.",  NULL },
    { "a.m.",  NULL },
    { "YYY",   NULL },
    { "yyy",   NULL },
    { "DDD",   "%j" },
    { "ddd",   "%j" },
    { "DAY",   NULL },
    { "day",   NULL },
    { "Day",   NULL },
    { "Mon",   "%b" },
    { "MON",   NULL },
    { "mon",   NULL },
    { "IYY",   NULL },
    { "iyy",   NULL },
    { "FF1",   NULL },
    { "FF2",   NULL },
    { "FF3",   NULL },
    { "FF4",   NULL },
    { "FF5",   NULL },
    { "FF6",   NULL },
    { "ff1",   NULL },
    { "ff2",   NULL },
    { "ff3",   NULL },
    { "ff4",   NULL },
    { "ff5",   NULL },
    { "ff6",   NULL },
    { "TZH",   NULL },
    { "tzh",   NULL },
    { "TZM",   NULL },
    { "tzm",   NULL },
    { "AD",    NULL },
    { "ad",    NULL },
    { "AM",    "%p" },
    { "PM",    "%p" },
    { "am",    NULL },
    { "pm",    NULL },
    { "BC",    NULL },
    { "bc",    NULL },
    { "CC",    NULL },
    { "cc",    NULL },
    { "DD",    "%d" },
    { "dd",    "%d" },
    { "Dy",    "%a" },
    { "DY",    NULL },
    { "dy",    NULL },
    { "FM",    NULL },
    { "fm",    NULL },
    { "FX",    NULL },
    { "fx",    NULL },
    { "HH",    "%I" },
    { "hh",    "%I" },
    { "ID",    NULL },
    { "id",    NULL },
    { "IW",    NULL },
    { "iw",    NULL },
    { "IY",    NULL },
    { "iy",    NULL },
    { "MI",    "%i" },
    { "mi",    "%i" },
    { "MM",    "%m" },
    { "mm",    "%m" },
    { "MS",    NULL },
    { "ms",    NULL },
    { "OF",    NULL },
    { "of",    NULL },
    { "RM",    NULL },
    { "rm",    NULL },
    { "SS",    "%S" },
    { "ss",    "%S" },
    { "TM",    NULL },
    { "tm",    NULL },
    { "TZ",    NULL },
    { "tz",    NULL },
    { "US",    NULL },
    { "us",    NULL },
    { "WW",    NULL },
    { "ww",    NULL },
    { "YY",    "%y" },
    { "yy",    "%y" },
    { "D",     NULL },
    { "d",     NULL },
    { "I",     NULL },
    { "i",     NULL },
    { "J",     NULL },
    { "j",     NULL },
    { "Q",     "%Q" },
    { "q",     "%Q" },
    { "W",     NULL },
    { "w",     NULL },
    { "Y",     NULL },
    { "y",     NULL },
    { NULL,    NULL }
};

bool
chfdw_translate_to_char_format(const char* pgfmt, StringInfo out) {
    const char* p     = pgfmt;
    bool just_keyword = false;

    while (*p) {
        const ToCharTok* t;
        bool matched = false;

        /*
         * Ordinal postfix (TH/th) attaches to a preceding numeric keyword; CH
         * has no ordinal output, so refuse rather than diverge.
         */
        if (just_keyword && (p[0] == 'T' || p[0] == 't') &&
            (p[1] == 'H' || p[1] == 'h')) {
            return false;
        }
        just_keyword = false;

        /* Quoted literal: pass inner chars through, doubling % for CH. */
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '\\' && p[1]) {
                    p++;
                } else if (*p == '"') {
                    p++;
                    break;
                }
                if (out) {
                    if (*p == '%') {
                        appendStringInfoString(out, "%%");
                    } else {
                        appendStringInfoChar(out, *p);
                    }
                }
                p++;
            }
            continue;
        }

        /* Outside quotes, backslash escapes only a literal " */
        if (p[0] == '\\' && p[1] == '"') {
            if (out) {
                appendStringInfoChar(out, '"');
            }
            p += 2;
            continue;
        }

        for (t = to_char_toks; t->pg; t++) {
            size_t l = strlen(t->pg);

            if (strncmp(p, t->pg, l) == 0) {
                if (!t->ch) {
                    return false;
                }
                if (out) {
                    appendStringInfoString(out, t->ch);
                }
                p += l;
                just_keyword = true;
                matched      = true;
                break;
            }
        }
        if (matched) {
            continue;
        }

        if (out) {
            if (*p == '%') {
                appendStringInfoString(out, "%%");
            } else {
                appendStringInfoChar(out, *p);
            }
        }
        p++;
    }

    return true;
}

/*
 * Deparse a function call.
 */
static void
deparseFuncExpr(FuncExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    bool first;
    ListCell* arg;
    CustomObjectDef *cdef, *old_cdef;

    /*
     * If the function call came from an implicit coercion, then just show the
     * first argument.
     */
    if (node->funcformat == COERCE_IMPLICIT_CAST) {
        deparseExpr((Expr*)linitial(node->args), context);
        return;
    }

    /*
     * If the function call came from a cast, then show the first argument
     * plus an explicit cast operation.
     */
    if (node->funcformat == COERCE_EXPLICIT_CAST) {
        Oid rettype = node->funcresulttype;
        int32 coercedTypmod;

        /* Get the typmod if this is a length-coercion function */
        (void)exprIsLengthCoercion((Node*)node, &coercedTypmod);

        appendStringInfoString(buf, "cast(");
        deparseExpr((Expr*)linitial(node->args), context);
        appendStringInfo(
            buf, ", 'Nullable(%s)')", deparse_type_name(rettype, coercedTypmod)
        );
        return;
    }

    /*
     * Normal function: display as proname(args). CF_ARRAY_SORT_DESC name
     * depends on boolean arg, resolve before printing.
     */
    cdef = appendFunctionName(node->funcid, context);

    if (cdef) {
        /* Special casses. */
        switch (cdef->cf_type) {
        case CF_JSON_EXTRACT_PATH_TEXT:
        case CF_JSON_EXTRACT_PATH: {
            deparseJsonbExtractPath(
                node, context, cdef->cf_type == CF_JSON_EXTRACT_PATH
            );
            return;
        }
        case CF_CURRENT_DATABASE: {
            SQLValueFunction svf;

            svf.op = SVFOP_CURRENT_CATALOG;
            deparseSQLValueFunction(&svf, context);
            return;
        }
        case CF_CURRENT_SCHEMA: {
            SQLValueFunction svf;

            svf.op = SVFOP_CURRENT_SCHEMA;
            deparseSQLValueFunction(&svf, context);
            return;
        }
        case CF_CLOCK_TIMESTAMP: {
            appendStringInfo(buf, "nowInBlock64(6, %s)", QUOTED_TZ);
            return;
        }
        case CF_TO_TIMESTAMP: {
            appendStringInfoString(buf, "toDateTime64(");
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ", 6, 'UTC')");
            return;
        }
        case CF_DATE_TRUNC: {
            Const* arg      = (Const*)linitial(node->args);
            char* trunctype = TextDatumGetCString(arg->constvalue);

            CSTRING_TOLOWER(trunctype);
            int cast_to_datetime64 = 0;

            if (strcmp(trunctype, "week") == 0) {
                appendStringInfoString(buf, "toMonday");
            } else if (strcmp(trunctype, "second") == 0) {
                cast_to_datetime64 = 1;
                appendStringInfoString(buf, "toStartOfSecond");
            } else if (strcmp(trunctype, "minute") == 0) {
                appendStringInfoString(buf, "toStartOfMinute");
            } else if (strcmp(trunctype, "hour") == 0) {
                appendStringInfoString(buf, "toStartOfHour");
            } else if (strcmp(trunctype, "day") == 0) {
                appendStringInfoString(buf, "toStartOfDay");
            } else if (strcmp(trunctype, "month") == 0) {
                appendStringInfoString(buf, "toStartOfMonth");
            } else if (strcmp(trunctype, "quarter") == 0) {
                appendStringInfoString(buf, "toStartOfQuarter");
            } else if (strcmp(trunctype, "year") == 0) {
                appendStringInfoString(buf, "toStartOfYear");
            } else {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("date_trunc cannot be exported for: %s", trunctype)
                );
            }

            pfree(trunctype);
            if (cast_to_datetime64) {
                appendStringInfoString(buf, "(toDateTime64(");
                deparseExpr(list_nth(node->args, 1), context);
                appendStringInfoString(buf, ", 1))");
            } else {
                appendStringInfoChar(buf, '(');
                deparseExpr(list_nth(node->args, 1), context);
                appendStringInfoChar(buf, ')');
            }
            return;
        }
        case CF_DATE_PART: {
            Const* arg        = (Const*)linitial(node->args);
            char* parttype    = TextDatumGetCString(arg->constvalue);
            bool postgres_dow = false;

            CSTRING_TOLOWER(parttype);

            if (strcmp(parttype, "day") == 0) {
                appendStringInfoString(buf, "toDayOfMonth");
            } else if (strcmp(parttype, "doy") == 0) {
                appendStringInfoString(buf, "toDayOfYear");
            } else if (strcmp(parttype, "dow") == 0) {
                appendStringInfoString(buf, "toDayOfWeek");
                postgres_dow = true;
            } else if (strcmp(parttype, "year") == 0) {
                appendStringInfoString(buf, "toYear");
            } else if (strcmp(parttype, "month") == 0) {
                appendStringInfoString(buf, "toMonth");
            } else if (strcmp(parttype, "hour") == 0) {
                appendStringInfoString(buf, "toHour");
            } else if (strcmp(parttype, "minute") == 0) {
                appendStringInfoString(buf, "toMinute");
            } else if (strcmp(parttype, "second") == 0) {
                appendStringInfoString(buf, "toSecond");
            } else if (strcmp(parttype, "quarter") == 0) {
                appendStringInfoString(buf, "toQuarter");
            } else if (strcmp(parttype, "isoyear") == 0) {
                appendStringInfoString(buf, "toISOYear");
            } else if (strcmp(parttype, "week") == 0) {
                appendStringInfoString(buf, "toISOWeek");
            } else if (strcmp(parttype, "epoch") == 0) {
                appendStringInfoString(buf, "toUnixTimestamp");
            } else {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("date_part cannot be exported for: %s", parttype)
                );
            }

            pfree(parttype);
            appendStringInfoChar(buf, '(');
            deparseExpr(list_nth(node->args, 1), context);
            if (postgres_dow) {
                /* Mode 2 aligns with Postgres behavior (Sunday = 0). */
                appendStringInfoString(buf, ", 2");
            }
            appendStringInfoChar(buf, ')');
            return;
        }
        case CF_TIMEZONE:
        case CF_ARRAY_PREPEND:
        case CF_STRING_TO_ARRAY:
        case CF_STRING_TO_ARRAY_PART: {
            /* Arguments are reversed. */
            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)list_nth(node->args, 1), context);
            appendStringInfoString(buf, ", ");
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoChar(buf, ')');
            if (cdef->cf_type == CF_STRING_TO_ARRAY_PART) {
                /* Use array subscript to extract item. */
                appendStringInfoChar(buf, '[');
                deparseExpr((Expr*)list_nth(node->args, 2), context);
                appendStringInfoChar(buf, ']');
            }
            return;
        }
        case CF_MATCH: {
            /* match(haystack, pattern) */
            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ", ");
            appendRegex(node->args, context);
            appendStringInfoChar(buf, ')');
            return;
        }
        case CF_SPLIT_BY_REGEX: {
            /* splitByRegexp(regexp, s) */
            appendStringInfoChar(buf, '(');
            appendRegex(node->args, context);
            appendStringInfoString(buf, ", ");
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoChar(buf, ')');
            return;
        }
        case CF_REPLACE_REGEX: {
            /* replaceRegexpOne() or replaceRegexpAll() */
            char* flags = NULL;

            if (list_length(node->args) >= 4) {
                /* Determine function name from "g" in flags. */
                /* XXX May not be a constant. Enforce elsewhere. */
                Const* arg = (Const*)list_nth(node->args, 3);

                flags   = TextDatumGetCString(arg->constvalue);
                char* c = strchr(flags, 'g');

                if (c) {
                    appendStringInfoString(buf, "replaceRegexpAll");
                    /* Remove any and all g flags. */
                    while (c[0]) {
                        while (c[1] == 'g') {
                            ++c;
                        }
                        c[0] = c[1];
                        ++c;
                    }
                } else {
                    appendStringInfoString(buf, "replaceRegexpOne");
                }
            } else {
                appendStringInfoString(buf, "replaceRegexpOne");
            }

            appendStringInfoChar(buf, '(');

            /* Emit the first string to search ("haystack"). */
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ", ");

            /* Emit the regular expression. */
            if (flags && strlen(flags)) {
                /* Concatenate flags. */
                appendStringInfo(context->buf, "concat('(?%s)', ", flags);
                deparseExpr((Expr*)list_nth(node->args, 1), context);
                appendStringInfoChar(buf, ')');
            } else {
                deparseExpr((Expr*)list_nth(node->args, 1), context);
            }

            /* Emit the replacement string and finish. */
            appendStringInfoString(buf, ", ");
            deparseExpr((Expr*)list_nth(node->args, 2), context);
            appendStringInfoChar(buf, ')');
            return;
        }
        case CF_REGEX_PG_MATCH: {
            /* Parse the regex so we can determine if it has captures. */
            Const* arg        = (Const*)list_nth(((FuncExpr*)node)->args, 1);
            pg_regex_t* regex = RE_compile_and_cache(
                DatumGetTextP(arg->constvalue), REG_ADVANCED, node->inputcollid
            );

            /*
             * If no captures, use arraySlice(extractAll(), otherwise
             * use extractGroups().
             */
            appendStringInfoString(
                buf, regex->re_nsub ? "extractGroups(" : "arraySlice(extractAll("
            );
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ", ");
            appendRegex(node->args, context);
            appendStringInfoString(buf, regex->re_nsub ? ")" : "), 1, 1)");
            return;
        }
        case CF_ARRAY_LENGTH:
            appendStringInfoString(buf, "(nullIf(length(");
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, "), 0))");
            return;
        case CF_ARRAY_POSITION:
            appendStringInfoChar(buf, '(');
            /* Use nullIf() to Convert CH 0 return value to NULL. */
            appendStringInfoString(buf, "nullIf(indexOf(");
            if (list_length(node->args) == 3) {
                /* Use arraySlice() to start search from the specified index. */
                appendStringInfoString(buf, "arraySlice(");
                deparseExpr((Expr*)linitial(node->args), context);
                appendStringInfoString(buf, ", ");
                deparseExpr((Expr*)list_nth(node->args, 2), context);
                appendStringInfoString(buf, "), ");
            } else {
                deparseExpr((Expr*)linitial(node->args), context);
                appendStringInfoString(buf, ", ");
            }
            deparseExpr((Expr*)list_nth(node->args, 1), context);
            appendStringInfoString(buf, "), 0)");
            if (list_length(node->args) == 3) {
                /* Restore start index to index number. */
                appendStringInfoString(buf, " + (");
                deparseExpr((Expr*)list_nth(node->args, 2), context);
                appendStringInfoString(buf, " - 1)");
            }
            appendStringInfoChar(buf, ')');
            return;
        case CF_TRIM_ARRAY:
            /* arrayResize(arr, length(arr) - n) */
            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ", length(");
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ") - ");
            deparseExpr((Expr*)list_nth(node->args, 1), context);
            appendStringInfoChar(buf, ')');
            return;
        case CF_ARRAY_SORT_DESC: {
            /* Determine function name from reverse boolean arg. */
            Const* desc_arg = (Const*)list_nth(node->args, 1);

            appendStringInfoString(
                buf,
                !desc_arg->constisnull && DatumGetBool(desc_arg->constvalue)
                    ? "arrayReverseSort"
                    : "arraySort"
            );
            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoChar(buf, ')');
            return;
        }

        case CF_ARRAY_FILL:
            /* arrayWithConstant(n, val): swap args */
            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)list_nth(node->args, 1), context);
            appendStringInfoString(buf, ", ");
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoChar(buf, ')');
            return;
        case CF_TO_CHAR: {
            /*
             * formatDateTime(ts, '<translated fmt>'). Shippability
             * already validated the format string is a non-NULL Const
             * whose tokens all translate; redoing the translation
             * here is just to materialize the result for SQL-literal
             * quoting.
             */
            Const* fmt_const = (Const*)list_nth(node->args, 1);
            char* pgfmt      = TextDatumGetCString(fmt_const->constvalue);
            StringInfoData chfmt;

            initStringInfo(&chfmt);
            if (!chfdw_translate_to_char_format(pgfmt, &chfmt)) {
                elog(
                    ERROR,
                    "to_char format unexpectedly rejected during deparse: %s",
                    pgfmt
                );
            }

            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)linitial(node->args), context);
            appendStringInfoString(buf, ", ");
            deparseStringLiteral(buf, chfmt.data);
            appendStringInfoChar(buf, ')');

            pfree(chfmt.data);
            pfree(pgfmt);
            return;
        }
        case CF_ENCODE: {
            Const* fmt   = (Const*)list_nth(node->args, 1);
            char* format = TextDatumGetCString(fmt->constvalue);

            if (pg_strcasecmp(format, "hex") == 0) {
                appendStringInfoString(buf, "lower(hex(");
                deparseExpr((Expr*)linitial(node->args), context);
                appendStringInfoString(buf, "))");
            } else if (pg_strcasecmp(format, "base64") == 0) {
                /*
                 * Postgres base64 is MIME (RFC 2045): a newline every 76
                 * output chars, but none trailing the final padded group.
                 */
                appendStringInfoString(
                    buf, "replaceRegexpAll(replaceRegexpAll(base64Encode("
                );
                deparseExpr((Expr*)linitial(node->args), context);
                appendStringInfoString(
                    buf, "), '(.{76})', '\\\\1\\n'), '(=+)\\n$', '\\\\1')"
                );
            } else if (pg_strcasecmp(format, "base64url") == 0) {
                /* PG19 base64url: RFC 4648 URL alphabet, unpadded, no line
                 * breaks, matching ClickHouse base64URLEncode exactly. */
                appendStringInfoString(buf, "base64URLEncode(");
                deparseExpr((Expr*)linitial(node->args), context);
                appendStringInfoChar(buf, ')');
            } else {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("encode cannot be exported for: %s", format)
                );
            }

            pfree(format);
            return;
        }
        default:
            break;
        }
    }

    appendStringInfoChar(buf, '(');
    old_cdef = context->func;

    /* ... and all the arguments */
    first = true;
    foreach (arg, node->args) {
        if (!first) {
            appendStringInfoString(buf, ", ");
        }

        deparseExpr((Expr*)lfirst(arg), context);
        first = false;
    }

    context->func = old_cdef;
    if (cdef) {
        for (int i = 0; i < cdef->paren_count; i++) {
            appendStringInfoChar(buf, ')');
        }
    } else {
        appendStringInfoChar(buf, ')');
    }
}

/*
 * Deparse a SQL Value function, which is a (potentially) parameterless
 * function with special grammar production separate from a regular function.
 * Some have ClickHouse equivalents; in particular, date and time values
 * always resolve relative to the Postgres session time zone (both "current"
 * and "local" variants). Others we generate here in Postgres and push down as
 * a literal string. See `ExecEvalSQLValueFunction` in the
 * `src/backend/executor/execExprInterp.c` Postgres source file for reference.
 */
static void
deparseSQLValueFunction(SQLValueFunction* svf, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    LOCAL_FCINFO(fcinfo, 0);
    Datum datum;

    /*
     * ClickHouse does not support TZs as part of DateTimes, so current and
     * local variants both render to the session time zone.
     */
    switch (svf->op) {
    case SVFOP_CURRENT_DATE:
        appendStringInfo(buf, "toDate(now(%s))", QUOTED_TZ);
        break;
    case SVFOP_CURRENT_TIME:
    case SVFOP_LOCALTIME:
        appendStringInfo(buf, "toTime64(now64(6, %s), 6)", QUOTED_TZ);
        break;
    case SVFOP_CURRENT_TIME_N:
    case SVFOP_LOCALTIME_N:
        appendStringInfo(
            buf, "toTime64(now64(%1$d, %2$s), %1$d)", svf->typmod, QUOTED_TZ
        );
        break;
    case SVFOP_CURRENT_TIMESTAMP:
    case SVFOP_LOCALTIMESTAMP:
        appendStringInfo(buf, "now64(6, %s)", QUOTED_TZ);
        break;
    case SVFOP_CURRENT_TIMESTAMP_N:
    case SVFOP_LOCALTIMESTAMP_N:
        appendStringInfo(buf, "now64(%d, %s)", svf->typmod, QUOTED_TZ);
        break;
    case SVFOP_CURRENT_ROLE:
    case SVFOP_CURRENT_USER:
    case SVFOP_USER:
        InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
        datum = current_user(fcinfo);
        if (fcinfo->isnull) {
            appendStringInfoString(buf, "NULL");
        } else {
            appendStringInfoString(buf, ch_quote_literal(DatumGetCString(datum)));
        }
        break;
    case SVFOP_SESSION_USER:
        InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
        datum = session_user(fcinfo);
        if (fcinfo->isnull) {
            appendStringInfoString(buf, "NULL");
        } else {
            appendStringInfoString(buf, ch_quote_literal(DatumGetCString(datum)));
        }
        break;
    case SVFOP_CURRENT_CATALOG:
        InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
        datum = current_database(fcinfo);
        appendStringInfoString(buf, ch_quote_literal(DatumGetCString(datum)));
        break;
    case SVFOP_CURRENT_SCHEMA:
        InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
        datum = current_schema(fcinfo);
        if (fcinfo->isnull) {
            appendStringInfoString(buf, "NULL");
        } else {
            appendStringInfoString(buf, ch_quote_literal(DatumGetCString(datum)));
        }
        break;
    default:
        elog(ERROR, "unknown SQL Value function: %i", svf->op);
    }
}

/*
 * Emit ` +/- INTERVAL <amount> <unit>`, skipping zero amounts. A negative
 * amount flips the operator so clickhouse always sees a positive literal.
 */
static void
appendIntervalTerm(StringInfo buf, bool plus, int64 amount, const char* unit) {
    if (amount == 0) {
        return;
    }

    if (amount < 0) {
        plus   = !plus;
        amount = -amount;
    }

    appendStringInfo(
        buf, " %s INTERVAL " INT64_FORMAT " %s", plus ? "+" : "-", amount, unit
    );
}

static void
deparseIntervalOp(Node* first, Node* second, deparse_expr_cxt* context, bool plus) {
    StringInfo buf = context->buf;
    Const* constval;
    Interval* span;

    appendStringInfoChar(buf, '(');
    deparseExpr((Expr*)first, context);

    if (!IsA(second, Const)) {
        bool old_op = context->interval_op;

        appendStringInfoString(buf, plus ? " + INTERVAL " : " - INTERVAL ");

        /* second */
        context->interval_op = true;
        deparseExpr((Expr*)second, context);
        context->interval_op = old_op;

        appendStringInfoChar(buf, ')');
        return;
    }

    constval = (Const*)second;
    span     = DatumGetIntervalP(constval->constvalue);

    /* clickhouse has no single interval kind, emit one term per unit */
    appendIntervalTerm(buf, plus, span->month, "MONTH");
    appendIntervalTerm(buf, plus, span->day, "DAY");
    /* sub-second precision dropped, matching ch_timestamp_out */
    appendIntervalTerm(buf, plus, span->time / USECS_PER_SEC, "SECOND");

    appendStringInfoChar(buf, ')');
}

static Oid
findFunction(Oid typoid, char* name) {
    int i;
    Oid result = InvalidOid;
    HeapTuple proctup;
    Form_pg_proc procform;
    CatCList* catlist;

    catlist = SearchSysCacheList1(PROCNAMEARGSNSP, CStringGetDatum(name));

    for (i = 0; i < catlist->n_members; i++) {
        proctup  = &catlist->members[i]->tuple;
        procform = (Form_pg_proc)GETSTRUCT(proctup);
        if (procform->proargtypes.values[0] == typoid)
#if PG_VERSION_NUM < 120000
            result = HeapTupleGetOid(proctup);
#else
            result = procform->oid;
#endif
    }

    ReleaseSysCacheList(catlist);

    return result;
}

static void
deparseJsonbDocument(
    Expr* document,
    JsonbDocumentKind document_kind,
    deparse_expr_cxt* context
) {
    if (document_kind == JSONB_DOCUMENT_NATIVE) {
        appendStringInfoString(context->buf, "toJSONString(");
    }
    deparseExpr(document, context);
    if (document_kind == JSONB_DOCUMENT_NATIVE) {
        appendStringInfoChar(context->buf, ')');
    }
}

static void
deparseJsonbNonnullDocument(
    Expr* document,
    JsonbDocumentKind document_kind,
    deparse_expr_cxt* context
) {
    appendStringInfoString(context->buf, "ifNull(");
    deparseJsonbDocument(document, document_kind, context);
    appendStringInfoString(context->buf, ", 'null')");
}

static void
deparseJsonbExists(
    Expr* document,
    Expr* key,
    JsonbDocumentKind document_kind,
    deparse_expr_cxt* context
) {
    StringInfo buf = context->buf;

    /* First we return NULL if either the document or the key is NULL. */
    appendStringInfoString(buf, "(if(isNull(");
    deparseJsonbDocument(document, document_kind, context);
    appendStringInfoString(buf, ") OR isNull(");
    deparseExpr(key, context);
    appendStringInfoString(buf, "), NULL, ");

    if (document_kind == JSONB_DOCUMENT_STRING) {
        /* Validate the string is valid JSON. */
        appendStringInfoString(buf, "if(NOT isValidJSON(");
        deparseJsonbNonnullDocument(document, document_kind, context);
        appendStringInfoString(
            buf, "), throwIf(1, 'invalid input syntax for type json'), "
        );
    }

    /* Use JSONHas if the JSON is an object. */
    appendStringInfoString(buf, "multiIf(JSONType(");
    deparseJsonbNonnullDocument(document, document_kind, context);
    appendStringInfoString(buf, ") = 'Object', JSONHas(");
    deparseJsonbNonnullDocument(document, document_kind, context);
    appendStringInfoString(buf, ", ");
    deparseExpr(key, context);
    /* Use arrayExists() if the JSON is an array. */
    appendStringInfoString(buf, "), JSONType(");
    deparseJsonbNonnullDocument(document, document_kind, context);
    appendStringInfoString(
        buf,
        ") = 'Array', arrayExists(elem -> "
        "JSONType(elem) = 'String' AND "
        "JSONExtractString(elem) = "
    );
    deparseExpr(key, context);
    appendStringInfoString(buf, ", JSONExtractArrayRaw(");
    deparseJsonbNonnullDocument(document, document_kind, context);
    /* Otherwise return false. */
    appendStringInfoString(buf, ")), 0)");

    if (document_kind == JSONB_DOCUMENT_STRING) {
        appendStringInfoChar(buf, ')');
    }
    appendStringInfoString(buf, "))");
}

/*
 * Deparse given operator expression. To avoid problems around priority of
 * operations, we always parenthesize the arguments.
 */
static void
deparseOpExpr(OpExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    HeapTuple tuple;
    Form_pg_operator form;
    char oprkind;
    ListCell* arg;
    CustomObjectDef* cdef;

    /* Retrieve information about the operator from system catalog. */
    tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
    if (!HeapTupleIsValid(tuple)) {
        elog(ERROR, "cache lookup failed for operator %u", node->opno);
    }

    form    = (Form_pg_operator)GETSTRUCT(tuple);
    oprkind = form->oprkind;

    /* Sanity check. */
    Assert(
        (oprkind == 'r' && list_length(node->args) == 1) ||
        (oprkind == 'l' && list_length(node->args) == 1) ||
        (oprkind == 'b' && list_length(node->args) == 2)
    );

    cdef = chfdw_check_for_custom_operator(node->opno, form);
    if (cdef) {
        switch (cdef->cf_type) {
        case CF_REGEX_MATCH:
        case CF_REGEX_NO_MATCH:
        case CF_REGEX_ICASE_MATCH:
        case CF_REGEX_ICASE_NO_MATCH: {
            bool negated =
                (cdef->cf_type == CF_REGEX_NO_MATCH ||
                 cdef->cf_type == CF_REGEX_ICASE_NO_MATCH);
            bool icase =
                (cdef->cf_type == CF_REGEX_ICASE_MATCH ||
                 cdef->cf_type == CF_REGEX_ICASE_NO_MATCH);

            if (negated) {
                appendStringInfoString(buf, "(NOT ");
            }
            appendStringInfoString(buf, "match(");
            deparseExpr(linitial(node->args), context);
            appendStringInfo(buf, ", concat('(?%s-s)', ", icase ? "i" : "");
            deparseExpr(lsecond(node->args), context);
            appendStringInfoString(buf, "))");
            if (negated) {
                appendStringInfoChar(buf, ')');
            }
            goto cleanup;
        } break;
        case CF_DATETIME_PL_INTERVAL: {
            deparseIntervalOp(
                linitial(node->args), list_nth(node->args, 1), context, true
            );
            goto cleanup;
        } break;
        case CF_DATETIME_MI_INTERVAL: {
            deparseIntervalOp(
                linitial(node->args), list_nth(node->args, 1), context, false
            );
            goto cleanup;
        } break;
        case CF_HSTORE_FETCHVAL: {
            Expr* arg1 = linitial(node->args);
            Expr* arg2 = list_nth(node->args, 1);

            if (IsA(arg1, Const)) {
                Const* constval = (Const*)arg1;
                Oid akeys       = findFunction(constval->consttype, "akeys");
                Oid avalues     = findFunction(constval->consttype, "avals");

                /* vals[nullif(indexOf(keys,toString(arg1)), 0)] */
                appendStringInfoChar(buf, '(');
                deparseArray(OidFunctionCall1(avalues, constval->constvalue), context);
                appendStringInfoString(buf, "[nullif(indexOf(");
                deparseArray(OidFunctionCall1(akeys, constval->constvalue), context);
                appendStringInfoChar(buf, ',');
                deparseExpr(arg2, context);
                appendStringInfoString(buf, "), 0)])");
            } else {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg(
                        "pg_clickhouse supports hstore fetchval "
                        "only for scalars"
                    )
                );
            }

            goto cleanup;
        } break;
        case CF_RE2_MATCH: {
            appendStringInfoString(buf, "match(");
            deparseExpr(linitial(node->args), context);
            appendStringInfoString(buf, ", ");
            deparseExpr(lsecond(node->args), context);
            appendStringInfoChar(buf, ')');
            goto cleanup;
        } break;
        case CF_ARRAY_CONTAINS: {
            appendStringInfoString(buf, "hasAll(");
            deparseExpr(linitial(node->args), context);
            appendStringInfoString(buf, ", ");
            deparseExpr(lsecond(node->args), context);
            appendStringInfoChar(buf, ')');
            goto cleanup;
        } break;
        case CF_ARRAY_CONTAINED_BY: {
            appendStringInfoString(buf, "hasAll(");
            deparseExpr(lsecond(node->args), context);
            appendStringInfoString(buf, ", ");
            deparseExpr(linitial(node->args), context);
            appendStringInfoChar(buf, ')');
            goto cleanup;
        } break;
        case CF_ARRAY_OVERLAP: {
            appendStringInfoString(buf, "hasAny(");
            deparseExpr(linitial(node->args), context);
            appendStringInfoString(buf, ", ");
            deparseExpr(lsecond(node->args), context);
            appendStringInfoChar(buf, ')');
            goto cleanup;
        } break;
        case CF_JSON_EXISTS: {
            Expr* document;
            JsonbDocumentKind document_kind =
                classifyJsonbDocument(linitial(node->args), &document);

            Assert(document_kind != JSONB_DOCUMENT_UNSUPPORTED);
            deparseJsonbExists(document, lsecond(node->args), document_kind, context);
            goto cleanup;
        } break;
        case CF_JSON_FETCHVAL:
        case CF_JSON_FETCHVAL_TEXT: {
            Expr* arg1 = linitial(node->args);
            Expr* arg2 = lsecond(node->args);

            /*
             * Convert jsonb ->> 'key' to ClickHouse dot notation:
             * column.key Convert jsonb -> 'key' to JSON-wrapped dot
             * notation: toJSONString(column.key) The -> operator
             * returns jsonb, so we need to wrap the result with
             * toJSONString() for type compatibility.
             */
            if (IsA(arg2, Const)) {
                Const* key = (Const*)arg2;

                if (key->consttype == TEXTOID && !key->constisnull) {
                    char* keyval = TextDatumGetCString(key->constvalue);

                    if (cdef->cf_type == CF_JSON_FETCHVAL) {
                        appendStringInfoString(buf, "toJSONString(");
                    }

                    deparseExpr(arg1, context);
                    appendStringInfoChar(buf, '.');
                    appendStringInfoString(buf, quote_identifier(keyval));

                    if (cdef->cf_type == CF_JSON_FETCHVAL) {
                        appendStringInfoChar(buf, ')');
                    }

                    pfree(keyval);
                    goto cleanup;
                }
            }
            /* Non-text key: fall through to standard deparse */
        } break;
        default: /* keep compiler quiet */;
        }
    }

    if ((node->opresulttype == INT2OID || node->opresulttype == INT4OID ||
         node->opresulttype == INT8OID) &&
        strcmp(NameStr(form->oprname), "/") == 0) {
        char* s = ch_format_type_extended(node->opresulttype, 0, 0);

        appendStringInfo(buf, "to%s", s);
    }

    /* Always parenthesize the expression. */
    appendStringInfoChar(buf, '(');

    /* Deparse left operand. */
    if (oprkind == 'r' || oprkind == 'b') {
        arg = list_head(node->args);

        /*
         * Check for TestCaseExpr, in statements like CASE expr WHEN <val>.
         * Basically they would look like, OpExpr->(TestCaseExpr, Const). We
         * should just skip first arg and deparse second.
         */
        if (IsA(lfirst(arg), CaseTestExpr)) {
            arg = list_tail(node->args);
            deparseExpr(lfirst(arg), context);
            appendStringInfoChar(buf, ')');
            goto cleanup;
        }

        deparseExpr(lfirst(arg), context);
        appendStringInfoChar(buf, ' ');
    }

    /*
     * Here we add support of special case like (<expr> || ' days')::interval.
     * We convert it to (<expr>) day. INTERVAL keyword added earlier
     */
    if (context->interval_op && strcmp(NameStr(form->oprname), "||") == 0) {
        Const* right = lfirst(list_tail(node->args));

        if (IsA(right, Const) && right->consttype == TEXTOID) {
            char* s = TextDatumGetCString(right->constvalue);

            if (strstr(s, "day") != NULL) {
                appendStringInfoString(buf, ") day");
            } else if (strstr(s, "year") != NULL) {
                appendStringInfoString(buf, ") year");
            } else if (strstr(s, "month") != NULL) {
                appendStringInfoString(buf, ") month");
            } else {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("unsupported type of interval")
                );
            }
            pfree(s);

            goto cleanup;
        }
    }

    /* Deparse operator name. */
    deparseOperatorName(buf, form);

    /* Deparse right operand. */
    if (oprkind == 'l' || oprkind == 'b') {
        arg = list_tail(node->args);
        appendStringInfoChar(buf, ' ');
        deparseExpr(lfirst(arg), context);
    }

    appendStringInfoChar(buf, ')');

cleanup:
    ReleaseSysCache(tuple);
}

/*
 * Print the name of an operator.
 */
static void
deparseOperatorName(StringInfo buf, Form_pg_operator opform) {
    char* opname;

    opname = NameStr(opform->oprname);

    if (strcmp(opname, "~~") == 0) {
        appendStringInfoString(buf, "LIKE");
    } else if (strcmp(opname, "~~*") == 0) {
        appendStringInfoString(buf, "ILIKE");
    } else if (strcmp(opname, "!~~") == 0) {
        appendStringInfoString(buf, "NOT LIKE");
    } else if (strcmp(opname, "!~~*") == 0) {
        appendStringInfoString(buf, "NOT ILIKE");
    } else {
        appendStringInfoString(buf, opname);
    }
}

/*
 * Deparse IS DISTINCT FROM.
 */
static void
deparseDistinctExpr(DistinctExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    Assert(list_length(node->args) == 2);

    appendStringInfoChar(buf, '(');
    deparseExpr(linitial(node->args), context);
    appendStringInfoString(buf, " IS DISTINCT FROM ");
    deparseExpr(lsecond(node->args), context);
    appendStringInfoChar(buf, ')');
}

static void
deparseNullIfExpr(NullIfExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    Assert(list_length(node->args) == 2);

    appendStringInfoString(buf, "NULLIF(");
    deparseExpr(linitial(node->args), context);
    appendStringInfoChar(buf, ',');
    deparseExpr(lsecond(node->args), context);
    appendStringInfoChar(buf, ')');
}

static void
deparseAsIn(ScalarArrayOpExpr* node, deparse_expr_cxt* context, CHEqualOp optype) {
    StringInfo buf = context->buf;
    Expr* arg1     = linitial(node->args);
    Expr* arg2     = lsecond(node->args);

    deparseExpr(arg1, context);
    if (optype == CH_OP_EQ) {
        appendStringInfoString(buf, " IN ");
    } else {
        appendStringInfoString(buf, " NOT IN ");
    }

    Assert(IsA(arg2, Const));
    context->array_as_tuple = true;
    deparseExpr(arg2, context);
    context->array_as_tuple = false;
}

/*
 * Returns true if `node` is a constant and, if it's output is an array, the
 * array has more than one value. Replace with `IsA(expr, Const)` when
 * ClickHouse 24 support dropped.
 */
static bool
is_ok_in_expr(Expr* expr) {
    Const* node;
    Oid typoutput;
    bool typIsVarlena;
    AnyArrayType* array;

    if (!IsA(expr, Const)) {
        return false;
    }

    node = (Const*)expr;
    getTypeOutputInfo(node->consttype, &typoutput, &typIsVarlena);
    if (typoutput != F_ARRAY_OUT) {
        return true;
    }

    /*
     * An empty `IN()` was invalid prior to ClickHouse 25, so disqualify an
     * empty array. We could change things to `WHERE false` in this situation,
     * but hopefully this workaround is temporary until we drop ClickHouse 24
     * support someday, so just leave it to the ClickHouse optimizer to deal
     * with it.
     */
    array = DatumGetAnyArrayP((Datum)node->constvalue);
    return AARR_NDIM(array) > 0;
}

/*
 * Deparse `expr` into a freshly allocated string instead of context->buf,
 * for callers that splice the same SQL into a template more than once.
 * Slightly ups our memory footprint during planning, but safer than trying
 * to reuse a buffer slice and more performant than deparsing multiple times.
 */
static char*
deparseExprToString(Expr* expr, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    StringInfoData capture;

    initStringInfo(&capture);
    context->buf = &capture;
    deparseExpr(expr, context);
    context->buf = buf;
    return capture.data;
}

/*
 * Emit a guarded CASE reproducing Postgres's exact three-valued answer for
 * any combination of the IN family (useOr, optype), checking at runtime
 * rather than requiring a plan-time proof:
 *   CASE WHEN <probe> IS NULL AND notEmpty(<array>) THEN NULL
 *        WHEN <a non-NULL element matches probe> THEN <match_result>
 *        WHEN <array contains a NULL> THEN NULL
 *        ELSE <not(match_result)>
 *   END
 *
 * This is ExecEvalScalarArrayOp's order of business. Here, match_result is what
 * is returned when an element of the array genuinely matches the probe, which
 * depends on the operation (elaboration below).
 *
 * An empty array gives the complement of that (`no_match_result` in the code)
 * for every probe, NULL included: Postgres actually resolves it before looking
 * at the probe, and in our guarded SQL, every WHEN will be false thanks to the
 * `notEmpty()` check in the first branch, so the ELSE answers.
 *
 * A NULL probe against a non-empty array is NULL on both systems.
 *
 * The actual predicate implementation for the form above must use `countEqual`,
 * and there is no `countNotEqual`, which means that complemented forms must
 * instead compute their count by subtracting from the full array length.
 * Here, Postgres's `useOr` determines most of our logic, while optype logically
 * determines which operation we apply to each element but in practice instead
 * determines how we use the result of `countEqual()`.
 *
 * `= ANY`:  optype=EQ, useOr=True,  exhaustive_op=NE (match > 0)
 * `= ALL`:  optype=EQ, useOr=False, exhaustive_op=EQ (match + null < length)
 * `<> ANY`: optype=NE, useOr=True,  exhaustive_op=NE (match + null < length)
 * `<> ALL`: optype=NE, useOr=False, exhaustive_op=EQ (match > 0)
 */
static void
deparseGuardedScalarArrayOp(
    bool useOr,
    CHEqualOp optype,
    Expr* probe,
    Expr* arr,
    deparse_expr_cxt* context
) {
    StringInfo buf              = context->buf;
    CHEqualOp exhaustive_op     = useOr ? CH_OP_NE : CH_OP_EQ;
    const char* match_result    = useOr ? "true" : "false";
    const char* no_match_result = useOr ? "false" : "true";
    char* probesql              = deparseExprToString(probe, context);
    char* arrsql                = deparseExprToString(arr, context);

    appendStringInfo(
        buf, "(CASE WHEN %s IS NULL AND notEmpty(%s) THEN NULL WHEN ", probesql, arrsql
    );
    if (optype != exhaustive_op) {
        appendStringInfo(buf, "countEqual(%s, %s) > 0", arrsql, probesql);
    } else {
        appendStringInfo(
            buf,
            "(length(%s) - countEqual(%s, %s) - countEqual(%s, NULL)) > 0",
            arrsql,
            arrsql,
            probesql,
            arrsql
        );
    }
    appendStringInfo(
        buf,
        " THEN %s WHEN countEqual(%s, NULL) > 0 THEN NULL ELSE %s END)",
        match_result,
        arrsql,
        no_match_result
    );
}

/*
 * Deparse given ScalarArrayOpExpr expression. Every branch below
 * self-parenthesizes its own output rather than relying on a shared
 * wrapper, so none of them depend on how a neighboring branch is spelled.
 *
 * ClickHouse's IN-family behavior diverges from Postgres, so we push down two
 * forms (simple and guarded) below to account for it. This comment is the
 * reference for the divergence rules used here and in the SubPlan gate
 * (`is_shippable_subplan`) and deparse (`deparseGuardedNotIn`).
 *
 * ClickHouse evaluates IN under two-valued logic:
 *   1    in {1, 2, 3}:   1
 *   10   in {1, 2, 3}:   0
 *   1    in {1, NULL}:   1
 *   10   in {1, NULL}:   0
 *   NULL in {1, NULL}:   1 if transform_null_in else NULL
 *   NULL in {1, 2, 3}:   0 if transform_null_in else NULL
 *
 * Postgres/Standard SQL usually disagrees:
 *   1    in {1, 2, 3}:   1
 *   10   in {1, 2, 3}:   0
 *   1    in {1, NULL}:   1
 *   10   in {1, NULL}:   NULL  <-- The problem case
 *   NULL in {1, NULL}:   NULL
 *   NULL in {1, 2, 3}:   NULL
 *
 * The two systems agree when NULL is not involved, but in standard SQL, NULL serves as
 * "dunno, LOL" and wildcard matches anything, resulting in a final "dunno, LOL"
 * answer, while ClickHouse just treats NULL as an ordinary value to match on.
 * The two systems technically agree where FALSE/NULL are both falsey, such as
 * WHERE/JOIN/HAVING conditions, CASE WHEN branches, and aggregate FILTERs.
 *
 * Under NOT, in a function/operator/aggregate argument, in a grouping or
 * ordering expression, or anywhere else the boolean's value is observable,
 * exact three-valued equivalence is required. And anywhere ClickHouse gives
 * TRUE where Postgres gives NULL is guaranteed to be wrong, so the complemented
 * forms require a provably NULL-free right-hand side or guardrails to handle
 * the divergent cases.
 *
 * ClickHouse has a separate behavioral quirk for arrays, similar to its `IN`
 * mechanics: `has()` and `countEqual()` will match NULL strictly against NULL
 * in those contexts as well, and are unaffected by `transform_null_in` or
 * similar settings. This means these routines cannot directly be used to
 * implement `<> ALL()` / `= ANY()`, but it also gives us a correct (if uglier)
 * pushdown spelling.
 *
 * We force `transform_null_in=0` to cover the cases it can, and in cases where
 * values are nullable (or we cannot prove that they aren't), emit the extra guard
 * branches below to ensure Postgres behavior is preserved after pushdown.
 */
static void
deparseScalarArrayOpExpr(ScalarArrayOpExpr* node, deparse_expr_cxt* context) {
    StringInfo buf   = context->buf;
    Expr* arg1       = linitial(node->args);
    Expr* arg2       = lsecond(node->args);
    CHEqualOp optype = chfdw_is_equal_op(node->opno);
    foreign_glob_cxt glob_cxt;
    SaopArrayNulls nulls;
    bool cheap_ok;

    if (optype == CH_OP_NONE) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "pg_clickhouse supports only equal and not equal operations on ANY/ALL "
                "functions"
            )
        );
    }

    Assert(list_length(node->args) == 2);

    memset(&glob_cxt, 0, sizeof(glob_cxt));
    glob_cxt.root   = context->root;
    glob_cxt.relids = context->scanrel->relids;
    nulls           = saop_array_nulls(arg2, &glob_cxt);

    /*
     * Very narrow case for IN()/NOT IN(): the native form is exact once the
     * array is a nice, non-empty, provably NULL-free constant, regardless
     * of the probe (ClickHouse's native IN propagates a NULL probe
     * correctly; see the block comment above)
     */
    if (nulls == SAOP_ARR_NULL_FREE && is_ok_in_expr(arg2) &&
        ((node->useOr && optype == CH_OP_EQ) || (!node->useOr && optype == CH_OP_NE))) {
        appendStringInfoChar(buf, '(');
        deparseAsIn(node, context, optype);
        appendStringInfoChar(buf, ')');
        return;
    }

    /*
     * An empty array is exact via the cheap form below regardless of the
     * probe's nullability (has()/countEqual() degenerate correctly on an
     * empty array by arithmetic), so it counts as "cheap ok" on its own;
     * otherwise both the array and the probe must be provably non-NULL.
     */
    cheap_ok = nulls == SAOP_ARR_EMPTY ||
               (nulls == SAOP_ARR_NULL_FREE && expr_never_null(arg1, &glob_cxt));

    if (cheap_ok) {
        char* probesql = deparseExprToString(arg1, context);
        char* arrsql   = deparseExprToString(arg2, context);

        if (node->useOr && optype == CH_OP_EQ) {
            /* = ANY */
            appendStringInfo(buf, "(has(%s, %s))", arrsql, probesql);
        } else if (node->useOr) {
            /*
             * <> ANY: TRUE exactly when an array element differs from the
             * probe. `not has()` would compute <> ALL instead (see the
             * block comment above); with the array proven NULL-free, "an
             * element differs" is just "fewer matches than elements".
             */
            appendStringInfo(
                buf, "(countEqual(%s, %s) < length(%s))", arrsql, probesql, arrsql
            );
        } else if (optype == CH_OP_EQ) {
            /* = ALL */
            appendStringInfo(
                buf, "(countEqual(%s, %s) = length(%s))", arrsql, probesql, arrsql
            );
        } else {
            /* <> ALL */
            appendStringInfo(buf, "(countEqual(%s, %s) = 0)", arrsql, probesql);
        }
        return;
    }

    deparseGuardedScalarArrayOp(node->useOr, optype, arg1, arg2, context);
}

/*
 * Deparse a RelabelType (binary-compatible cast) node.
 */
static void
deparseRelabelType(RelabelType* node, deparse_expr_cxt* context) {
    deparseExpr(node->arg, context);
}

/*
 * Deparse a BoolExpr node.
 */
static void
deparseBoolExpr(BoolExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    const char* op = NULL; /* keep compiler quiet */
    bool first;
    ListCell* lc;

    switch (node->boolop) {
    case AND_EXPR:
        op = "AND";
        break;
    case OR_EXPR:
        op = "OR";
        break;
    case NOT_EXPR: {
        Node* arg = (Node*)linitial(node->args);

        /*
         * NOT over an ANY SubPlan is Postgres's x NOT IN (SELECT ..). When a
         * NULL could reach the comparison the native complement is wrong;
         * emit the guarded form instead. The classifier answers exactly as
         * the walker's EXPR_CTX_NEGATED gate did, so every shape arriving
         * here has a remote form.
         */
        if (IsA(arg, SubPlan) && ((SubPlan*)arg)->subLinkType == ANY_SUBLINK &&
            classify_notin_subplan(
                (SubPlan*)arg, context->root, context->scanrel->relids
            ) == NOTIN_SHIP_GUARDED) {
            deparseGuardedNotIn((SubPlan*)arg, context);
            return;
        }

        appendStringInfoString(buf, "(NOT ");
        deparseExpr((Expr*)arg, context);
        appendStringInfoChar(buf, ')');
        return;
    }
    }

    appendStringInfoChar(buf, '(');
    first = true;
    foreach (lc, node->args) {
        if (!first) {
            appendStringInfo(buf, " %s ", op);
        }
        deparseExpr((Expr*)lfirst(lc), context);
        first = false;
    }
    appendStringInfoChar(buf, ')');
}

/*
 * Deparse IS [NOT] NULL expression.
 */
static void
deparseNullTest(NullTest* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    appendStringInfoChar(buf, '(');
    deparseExpr(node->arg, context);

    /*
     * For scalar inputs, we prefer to print as IS [NOT] NULL, which is
     * shorter and traditional. If it's a rowtype input but we're applying a
     * scalar test, must print IS [NOT] DISTINCT FROM NULL to be semantically
     * correct.
     */
    if (node->argisrow || !type_is_rowtype(exprType((Node*)node->arg))) {
        if (node->nulltesttype == IS_NULL) {
            appendStringInfoString(buf, " IS NULL)");
        } else {
            appendStringInfoString(buf, " IS NOT NULL)");
        }
    } else {
        if (node->nulltesttype == IS_NULL) {
            appendStringInfoString(buf, " IS NOT DISTINCT FROM NULL)");
        } else {
            appendStringInfoString(buf, " IS DISTINCT FROM NULL)");
        }
    }
}

/*
 * Deparse ARRAY[...] construct.
 */
static void
deparseArrayExpr(ArrayExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    if (node->elements == NIL) {
        appendStringInfoString(buf, "CAST(");
    }

    appendStringInfoString(buf, "[");
    deparseArrayList(node, context);
    appendStringInfoChar(buf, ']');

    /* If the array is empty, we need an explicit cast to the array type. */
    if (node->elements == NIL) {
        appendStringInfo(buf, ", '%s')", deparse_type_name(node->array_typeid, -1));
    }
}

/*
 * Deparse an array to a list, as in converting a variadic function call's
 * array to the list of individual values.
 */
static void
deparseArrayList(ArrayExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    bool first     = true;
    ListCell* lc;

    foreach (lc, node->elements) {
        if (!first) {
            appendStringInfoString(buf, ", ");
        }
        deparseExpr(lfirst(lc), context);
        first = false;
    }
}

/*
 * Detect DESC, USING <OPERATOR> and NULLS FIRST / NULLS LAST parts
 * of an ORDER BY clause and raise an exception a appropriate, since
 * they're not (yet) supported by ClickHouse aggregates.
 */
static void
appendAggOrderBySuffix(
    Oid sortop,
    Oid sortcoltype,
    bool nulls_first,
    deparse_expr_cxt* context
) {
    /* StringInfo buf = context->buf; */
    TypeCacheEntry* typentry;

    /* See whether operator is default < or > for sort expr's datatype. */
    typentry = lookup_type_cache(sortcoltype, TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);

    if (sortop == typentry->lt_opr) {
        /*
         * Okay.
         *
         * appendStringInfoString(buf, " ASC");
         */
        ;
    } else if (sortop == typentry->gt_opr) {
        /* appendStringInfoString(buf, " DESC"); */
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "pg_clickhouse: ClickHouse does not support \"DESC\" in aggregate "
                "expressions"
            )
        );
    } else {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "pg_clickhouse: ClickHouse does not support \"USING\" in aggregate "
                "expressions"
            )
        );
    }

    if (nulls_first) {
        /* appendStringInfoString(buf, " NULLS FIRST"); */
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg(
                "pg_clickhouse: ClickHouse does not support \"NULLS FIRST\" in "
                "aggregate expressions"
            )
        );
    } else {
        /*
         * Okay unless DESC support added.
         *
         * appendStringInfoString(buf, " NULLS LAST");
         */
    }
}

/*
 * Append ORDER BY arguments to aggregate function arguments.
 */
static void
appendAggOrderBy(List* orderList, List* targetList, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    ListCell* lc;
    bool first = true;

    foreach (lc, orderList) {
        SortGroupClause* srt = (SortGroupClause*)lfirst(lc);
        Node* sortexpr;

        if (!first) {
            appendStringInfoString(buf, ", ");
        }
        first = false;

        /* Deparse the sort expression proper. */
        sortexpr =
            deparseSortGroupClause(srt->tleSortGroupRef, targetList, false, context);
        /* Add decoration as needed. */
        appendAggOrderBySuffix(
            srt->sortop, exprType(sortexpr), srt->nulls_first, context
        );
    }
}

/*
 * Check if Aggref operates on a ClickHouse AggregateFunction column.
 *
 * ClickHouse AggregateFunction columns store partially aggregated state
 * (e.g., AggregateFunction(sum, Int64)) that must be finalized with a
 * -Merge suffix (e.g., sumMerge) instead of plain aggregate.
 *
 * Walks Vars in node->args checking if any Var belongs to foreign scan relation
 * and has "aggregatefunction" FDW column option set.
 */
static bool
aggref_on_aggregate_function(Aggref* node, deparse_expr_cxt* context) {
    List* vars = pull_var_clause((Node*)node->args, 0);
    ListCell* lc;
    Relids relids = context->scanrel->relids;
    bool found    = false;

    foreach (lc, vars) {
        Var* var = (Var*)lfirst(lc);

        if (bms_is_member(var->varno, relids) && var->varlevelsup == 0) {
            RangeTblEntry* rte = planner_rt_fetch(var->varno, context->root);

            /*
             * Check FDW column options directly rather than relying on
             * custom_columns_cache, which can be invalidated between
             * GetForeignRelSize and deparse.
             */
            List* options = GetForeignColumnOptions(rte->relid, var->varattno);
            ListCell* olc;

            foreach (olc, options) {
                DefElem* def = (DefElem*)lfirst(olc);

                if (strcmp(def->defname, "aggregatefunction") == 0) {
                    found = true;
                    break;
                }
            }
            if (found) {
                break;
            }
        }
    }
    list_free(vars);
    return found;
}

/*
 * Emit partial aggregate as a ClickHouse array holding Postgres
 * transition state, so local Finalize combines across partitions.
 *   AVG_INT    -> int8[2]   {count, sum}
 *   STAT_FLOAT -> float8[3] {N, sum(x), sum((x - Sx/N)^2)}
 * See float8_accum, int8_avg in PostgreSQL.
 *
 * float8_accum's third slot is the sum of squared deviations, which
 * ClickHouse rebuilds via the identity Sxx = sum(x^2) - sum(x)^2 / N.
 */
static void
deparsePartialStatArray(Aggref* node, AggPartialKind kind, deparse_expr_cxt* context) {
    StringInfo buf       = context->buf;
    TargetEntry* tle     = (TargetEntry*)linitial(node->args);
    const char* ifSuffix = node->aggfilter ? "If" : "";
    char* arg;
    char* cf = ""; /* trailing -If condition arg, empty without FILTER */

    Assert(!tle->resjunk);

    /* Capture argument SQL so it can be referenced several times. */
    arg = deparseExprToString((Expr*)tle->expr, context);

    /* Capture FILTER condition as each component's -If argument. */
    if (node->aggfilter) {
        cf = psprintf(
            ", (%s) > 0", deparseExprToString((Expr*)node->aggfilter, context)
        );
    }

    if (kind == AGG_PARTIAL_AVG_INT) {
        // https://github.com/postgres/postgres/blob/f5cc81719e6da4cbdb1f797c48b693e91018153a/src/backend/utils/adt/numeric.c#L6760
        appendStringInfo(
            buf,
            "[toInt64(count%s(%s%s)), toInt64(sum%s(%s%s))]",
            ifSuffix,
            arg,
            cf,
            ifSuffix,
            arg,
            cf
        );
    } else if (kind == AGG_PARTIAL_STAT_FLOAT) {
        // https://github.com/postgres/postgres/blob/f5cc81719e6da4cbdb1f797c48b693e91018153a/src/backend/utils/adt/float.c#L2891
        appendStringInfo(
            buf,
            "[toFloat64(count%s(%s%s)), sum%s(toFloat64(%s)%s), "
            "if(count%s(%s%s) > 0, sum%s(pow(toFloat64(%s), 2)%s) - "
            "pow(sum%s(toFloat64(%s)%s), 2) / count%s(%s%s), 0)]",
            ifSuffix,
            arg,
            cf,
            ifSuffix,
            arg,
            cf,
            ifSuffix,
            arg,
            cf,
            ifSuffix,
            arg,
            cf,
            ifSuffix,
            arg,
            cf,
            ifSuffix,
            arg,
            cf
        );
    } else {
        Assert(false);
        elog(ERROR, "unknown aggregate type %d", kind);
    }

    if (node->aggfilter) {
        pfree(cf);
    }
    pfree(arg);
}

/*
 * Deparse an Aggref node.
 */
static void
deparseAggref(Aggref* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    CustomObjectDef* cdef;
    CHFdwRelationInfo* fpinfo = context->scanrel->fdw_private;
    bool aggfilter            = false;
    bool sign_count_filter    = false;
    uint8 brcount             = 1;
    char* name                = get_func_name(node->aggfnoid);
    bool omit_star            = false; /* Explained below. */
    bool use_variadic;

    /* Simple aggregates push down directly */
    Assert(
        node->aggsplit == AGGSPLIT_SIMPLE || node->aggsplit == AGGSPLIT_INITIAL_SERIAL
    );

    if (node->aggsplit == AGGSPLIT_INITIAL_SERIAL) {
        AggPartialKind kind = agg_partial_kind(node);

        if (kind == AGG_PARTIAL_AVG_INT || kind == AGG_PARTIAL_STAT_FLOAT) {
            deparsePartialStatArray(node, kind, context);
            return;
        }
    }

    /* Check if need to print expand VARIADIC (cf. ruleutils.c) */
    use_variadic = node->aggvariadic;

    /* Find aggregate name from aggfnoid which is a pg_proc entry */
    cdef          = context->func;
    context->func = appendFunctionName(node->aggfnoid, context);

    /* 'If' part */
    if (context->func && context->func->cf_type == CF_SIGN_COUNT && !node->aggstar) {
        sign_count_filter = true;
    }

    if (aggref_on_aggregate_function(node, context)) {
        appendStringInfoString(buf, "Merge");
    }

    if (node->aggfilter || sign_count_filter) {
        aggfilter = true;
        appendStringInfoString(buf, "If");
    }

    /*
     * groupConcat(delimiter)(expr): emit delimiter as parametric arg, then
     * only the first non-junk arg (the expression).
     */
    if (context->func && context->func->cf_type == CF_STRING_AGG) {
        ListCell* arg;

        /* Emit delimiter (2nd non-junk arg) as parametric arg. */
        appendStringInfoChar(buf, '(');
        foreach (arg, node->args) {
            TargetEntry* tle = (TargetEntry*)lfirst(arg);

            if (tle->resjunk) {
                continue;
            }
            if (arg == list_head(node->args)) {
                continue;
            }
            deparseExpr((Expr*)tle->expr, context);
            break;
        }
        appendStringInfoString(buf, ")(");

        /* Emit expression (1st non-junk arg). */
        if (node->aggdistinct != NIL) {
            appendStringInfoString(buf, "DISTINCT ");
        }
        deparseExpr((Expr*)((TargetEntry*)linitial(node->args))->expr, context);
        appendStringInfoChar(buf, ')');

        context->func = cdef;
        return;
    }

    if (AGGKIND_IS_ORDERED_SET(node->aggkind)) {
        /* Need to emit the parametric args. */
        Assert(!node->aggvariadic);
        Assert(node->aggorder != NIL);

        if (context->func && context->func->cf_type == CF_PARAM_LIST_AGG) {
            /* Convert array to list of parametric args. */
            ListCell* arg = list_head(node->aggdirectargs);
            Node* array   = lfirst(arg);
            bool atup     = context->array_as_tuple;

            Assert(list_length(node->aggdirectargs) == 1);
            Assert(nodeTag(array) == T_Const);
            context->array_as_tuple = true; /* Formats with () instead of []. */
            deparseArray(((Const*)array)->constvalue, context);
            context->array_as_tuple = atup;
        } else {
            /* Emit direct args as ClickHouse parametric args. */
            ListCell* arg;
            bool first = true;

            appendStringInfoChar(buf, '(');
            foreach (arg, node->aggdirectargs) {
                if (!first) {
                    appendStringInfoString(buf, ", ");
                }
                first = false;

                deparseExpr((Expr*)lfirst(arg), context);
            }
            appendStringInfoChar(buf, ')');
        }

        /* Close parameter args and start regular args. */
        appendStringInfoChar(buf, '(');
        /* Emit `WITHIN GROUP (ORDER BY ..)` args with no closing paren. */
        context->no_sort_parens = true;
        appendAggOrderBy(node->aggorder, node->args, context);
        context->no_sort_parens = false;
    } else {
        /* Add DISTINCT */
        appendStringInfoChar(buf, '(');
        if (node->aggdistinct != NIL) {
            appendStringInfoString(buf, "DISTINCT ");
        }

        /* aggstar can be set only in zero-argument aggregates */
        if (node->aggstar) {
            /*
             * Omit * for COUNT(*) but not COUNT(DISTINCT *)
             * https://github.com/ClickHouse/pg_clickhouse/issues/25 To be
             * fixed in ClickHouse 25.11, so can be omitted once released.
             * https://github.com/ClickHouse/ClickHouse/pull/89373
             *
             * XXX Once ClickHouse has made a release fixing this issue,
             * consider adding the Client::ServerInfo struct returned from
             * Client::GetServerInfo() to deparse_expr_cxt so we can allow *
             * to be passed through for the fixed version.
             */
            omit_star =
                node->aggfilter && node->aggdistinct == NIL && !strcmp(name, "count");
            if (context->func && context->func->cf_type == CF_SIGN_COUNT) {
                Assert(fpinfo && fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE);
                appendStringInfoString(buf, fpinfo->ch_table_sign_field);
            }
            /* Omit * for COUNT (*) but not COUNT(DISTINCT *). */
            else if (!omit_star) {
                appendStringInfoChar(buf, '*');
            }
        } else {
            ListCell* arg;
            bool first = true;
            bool signMultiply =
                (context->func && (context->func->cf_type == CF_SIGN_AVG ||
                                   context->func->cf_type == CF_SIGN_SUM));

            /* Add all the arguments */
            if (sign_count_filter) {

                /*
                 * in case if COUNT(col) we should get countIf(sign, col is
                 * not null)
                 */
                appendStringInfoString(buf, fpinfo->ch_table_sign_field);
            } else {
                /* default arguments output */
                foreach (arg, node->args) {
                    TargetEntry* tle = (TargetEntry*)lfirst(arg);
                    Node* n          = (Node*)tle->expr;

                    if (tle->resjunk) {
                        continue;
                    }

                    if (!first) {
                        appendStringInfoString(buf, ", ");
                    }

                    first = false;

                    if (use_variadic && lnext(node->args, arg) == NULL) {
                        /* Convert variadic array to list of arguments. */
                        Assert(nodeTag(n) == T_ArrayExpr);
                        deparseArrayList((ArrayExpr*)n, context);
                    } else {
                        deparseExpr((Expr*)n, context);
                    }
                }

                if (signMultiply) {
                    Assert(fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE);
                    appendStringInfoString(buf, " * ");
                    appendStringInfoString(buf, fpinfo->ch_table_sign_field);
                }
            }
        }
    }

    /* Add 'If' part condition */
    if (aggfilter) {
        /* No argument output for COUNT (*). */
        if (!omit_star) {
            appendStringInfoChar(buf, ',');
        }

        if (node->aggfilter) {
            appendStringInfoString(buf, "((");
            deparseExpr((Expr*)node->aggfilter, context);
            appendStringInfoString(buf, ") > 0)");
        }

        if (sign_count_filter) {
            if (node->aggfilter) {
                appendStringInfoString(buf, " AND ");
            }

            appendStringInfoChar(buf, '(');
            deparseExpr((Expr*)((TargetEntry*)linitial(node->args))->expr, context);
            appendStringInfoString(buf, ") IS NOT NULL");
        }
    }

    while (brcount--) {
        appendStringInfoChar(buf, ')');
    }

    /* AVG stuff */
    if (context->func && context->func->cf_type == CF_SIGN_AVG) {
        appendStringInfoString(buf, " / sumIf(");
        appendStringInfoString(buf, fpinfo->ch_table_sign_field);
        appendStringInfoChar(buf, ',');
        if (node->aggfilter) {
            deparseExpr((Expr*)node->aggfilter, context);
            appendStringInfoString(buf, " AND ");
        }
        deparseExpr((Expr*)((TargetEntry*)linitial(node->args))->expr, context);
        appendStringInfoString(buf, " IS NOT NULL");
        appendStringInfoChar(buf, ')');
    }

    /* original */
    context->func = cdef;
}

/*
 * Deparse a WindowFunc node into context->buf.
 *
 * Generates: func_name(args) OVER (PARTITION BY ... ORDER BY ... frame)
 */
static void
deparseWindowFunc(WindowFunc* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    Query* query   = context->root->parse;
    WindowClause* wc;
    ListCell* lc;
    bool first;
    char* funcname;

    /* Find the WindowClause referenced by this WindowFunc */
    wc = (WindowClause*)list_nth(query->windowClause, node->winref - 1);

    /* Emit function name */
    funcname = get_func_name(node->winfnoid);
    CSTRING_TOLOWER(funcname);
    appendStringInfoString(buf, funcname);
    appendStringInfoChar(buf, '(');

    /* Emit arguments */
    first = true;
    foreach (lc, node->args) {
        if (!first) {
            appendStringInfoString(buf, ", ");
        }
        first = false;
        deparseExpr((Expr*)lfirst(lc), context);
    }

    appendStringInfoString(buf, ") OVER (");

    /* PARTITION BY */
    if (wc->partitionClause) {
        appendStringInfoString(buf, "PARTITION BY ");
        first = true;
        foreach (lc, wc->partitionClause) {
            SortGroupClause* sgc = (SortGroupClause*)lfirst(lc);
            TargetEntry* tle =
                get_sortgroupref_tle(sgc->tleSortGroupRef, query->targetList);

            if (!first) {
                appendStringInfoString(buf, ", ");
            }
            first = false;
            deparseExpr((Expr*)tle->expr, context);
        }
    }

    /* ORDER BY */
    if (wc->orderClause) {
        if (wc->partitionClause) {
            appendStringInfoChar(buf, ' ');
        }
        appendStringInfoString(buf, "ORDER BY ");
        first = true;
        foreach (lc, wc->orderClause) {
            SortGroupClause* sgc = (SortGroupClause*)lfirst(lc);
            TargetEntry* tle =
                get_sortgroupref_tle(sgc->tleSortGroupRef, query->targetList);
            TypeCacheEntry* typentry;

            if (!first) {
                appendStringInfoString(buf, ", ");
            }
            first = false;
            deparseExpr((Expr*)tle->expr, context);

            /* Determine sort direction from the sort operator */
            typentry = lookup_type_cache(
                exprType((Node*)tle->expr), TYPECACHE_LT_OPR | TYPECACHE_GT_OPR
            );
            if (sgc->sortop == typentry->gt_opr) {
                appendStringInfoString(buf, " DESC");
            } else {
                appendStringInfoString(buf, " ASC");
            }

            if (sgc->nulls_first) {
                appendStringInfoString(buf, " NULLS FIRST");
            }
        }
    }

    /*
     * Frame clause.  Skip for ranking functions (row_number, rank,
     * dense_rank, ntile, cume_dist, percent_rank) since ClickHouse does not
     * accept frame specifications for these.  For other functions, emit
     * non-default frames only.
     */
    if (node->winfnoid != F_ROW_NUMBER && node->winfnoid != F_RANK_ &&
        node->winfnoid != F_DENSE_RANK_ && node->winfnoid != F_NTILE &&
        node->winfnoid != F_CUME_DIST_ && node->winfnoid != F_PERCENT_RANK_ &&
        wc->frameOptions != (FRAMEOPTION_DEFAULTS | FRAMEOPTION_NONDEFAULT) &&
        (wc->frameOptions & FRAMEOPTION_NONDEFAULT)) {
        appendStringInfoChar(buf, ' ');

        /* Frame type */
        if (wc->frameOptions & FRAMEOPTION_ROWS) {
            appendStringInfoString(buf, "ROWS ");
        } else if (wc->frameOptions & FRAMEOPTION_RANGE) {
            appendStringInfoString(buf, "RANGE ");
        } else if (wc->frameOptions & FRAMEOPTION_GROUPS) {
            appendStringInfoString(buf, "GROUPS ");
        }

        /* Frame start and end */
        if (wc->frameOptions & FRAMEOPTION_BETWEEN) {
            appendStringInfoString(buf, "BETWEEN ");

            /* Start bound */
            if (wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) {
                appendStringInfoString(buf, "UNBOUNDED PRECEDING");
            } else if (wc->frameOptions & FRAMEOPTION_START_CURRENT_ROW) {
                appendStringInfoString(buf, "CURRENT ROW");
            } else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING) {
                deparseExpr((Expr*)wc->startOffset, context);
                appendStringInfoString(buf, " PRECEDING");
            } else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING) {
                deparseExpr((Expr*)wc->startOffset, context);
                appendStringInfoString(buf, " FOLLOWING");
            }

            appendStringInfoString(buf, " AND ");

            /* End bound */
            if (wc->frameOptions & FRAMEOPTION_END_UNBOUNDED_FOLLOWING) {
                appendStringInfoString(buf, "UNBOUNDED FOLLOWING");
            } else if (wc->frameOptions & FRAMEOPTION_END_CURRENT_ROW) {
                appendStringInfoString(buf, "CURRENT ROW");
            } else if (wc->frameOptions & FRAMEOPTION_END_OFFSET_PRECEDING) {
                deparseExpr((Expr*)wc->endOffset, context);
                appendStringInfoString(buf, " PRECEDING");
            } else if (wc->frameOptions & FRAMEOPTION_END_OFFSET_FOLLOWING) {
                deparseExpr((Expr*)wc->endOffset, context);
                appendStringInfoString(buf, " FOLLOWING");
            }
        } else {
            /* No BETWEEN — single start bound */
            if (wc->frameOptions & FRAMEOPTION_START_UNBOUNDED_PRECEDING) {
                appendStringInfoString(buf, "UNBOUNDED PRECEDING");
            } else if (wc->frameOptions & FRAMEOPTION_START_CURRENT_ROW) {
                appendStringInfoString(buf, "CURRENT ROW");
            } else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_PRECEDING) {
                deparseExpr((Expr*)wc->startOffset, context);
                appendStringInfoString(buf, " PRECEDING");
            } else if (wc->frameOptions & FRAMEOPTION_START_OFFSET_FOLLOWING) {
                deparseExpr((Expr*)wc->startOffset, context);
                appendStringInfoString(buf, " FOLLOWING");
            }
        }
    }

    appendStringInfoChar(buf, ')');
}

static void
deparseCaseExpr(CaseExpr* node, deparse_expr_cxt* context) {
#define DEPARSE_WRAPPED(node)                                                          \
    do {                                                                               \
        bool isnull = (IsA(node, Const) && ((Const*)(node))->constisnull);             \
        if (conv && !isnull)                                                           \
            appendStringInfoString(buf, conv);                                         \
        deparseExpr(node, context);                                                    \
        if (conv && !isnull)                                                           \
            appendStringInfoChar(buf, ')');                                            \
    } while (0)

    StringInfo buf = context->buf;
    ListCell* lc;
    char* conv = NULL;

    if (node->casetype == INT2OID || node->casetype == INT4OID ||
        node->casetype == INT8OID) {
        conv = ch_format_type_extended(node->casetype, 0, 0);
        conv = psprintf("to%s(", conv);
    }

    appendStringInfoString(buf, "CASE");
    if (node->arg) {
        appendStringInfoChar(buf, ' ');
        deparseExpr(node->arg, context);
    }

    foreach (lc, node->args) {
        CaseWhen* arg = lfirst(lc);

        Assert(IsA(arg, CaseWhen));
        appendStringInfoString(buf, " WHEN ");
        deparseExpr(arg->expr, context);

        /*
         * in simple cases like WHEN val THEN we should extend the condition
         * for WHEN val = 1 since there is no bool type in ClickHouse
         */
        if (IsA(arg->expr, Var)) {
            appendStringInfoString(buf, " = 1");
        }

        appendStringInfoString(buf, " THEN ");
        DEPARSE_WRAPPED(arg->result);
    }

    if (node->defresult) {
        appendStringInfoString(buf, " ELSE ");
        DEPARSE_WRAPPED(node->defresult);
    }

    if (conv) {
        pfree(conv);
    }
    appendStringInfoString(buf, " END");
}

static void
deparseCaseWhen(CaseWhen* node, deparse_expr_cxt* context) {
    /*
     * XXX Needs implementation.
     *
     * StringInfo buf = context->buf;
     *
     * ListCell * lc;
     */
}

static void
deparseRowExpr(RowExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    ListCell* lc;

    bool first = true;

    appendStringInfoChar(buf, '(');
    foreach (lc, node->args) {
        if (!first) {
            appendStringInfoChar(buf, ',');
        }

        first = false;
        if (IsA(lfirst(lc), Const)) {
            deparseConst((Const*)lfirst(lc), context, 1);
        } else {
            deparseExpr(lfirst(lc), context);
        }
    }
    appendStringInfoChar(buf, ')');
}

static void
deparseCoerceViaIO(CoerceViaIO* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;

    if (node->resulttype == INTERVALOID) {
        deparseExpr(node->arg, context);
    } else {
        appendStringInfoString(buf, "CAST(");
        deparseExpr(node->arg, context);
        appendStringInfoString(buf, " AS ");

        if (node->resultcollid == 1) {
            appendStringInfoString(buf, "Nullable(");
        }

        appendStringInfoString(buf, deparse_type_name(node->resulttype, 0));

        if (node->resultcollid == 1) {
            appendStringInfoChar(buf, ')');
        }

        appendStringInfoChar(buf, ')');
    }
}

static void
deparseCoalesceExpr(CoalesceExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    ListCell* lc;
    bool first;

    appendStringInfoString(buf, "COALESCE(");

    first = true;
    foreach (lc, node->args) {
        Expr* arg = lfirst(lc);

        if (!first) {
            appendStringInfoString(buf, ", ");
        }

        /* first arg should be nullable */
        if (IsA(arg, CoerceViaIO)) {
            CoerceViaIO* vio = (CoerceViaIO*)arg;

            if (arg != llast(node->args)) {
                vio->resultcollid = 1;
            } else {
                vio->resultcollid = InvalidOid;
            }
        }

        first = false;
        deparseExpr(arg, context);
    }
    appendStringInfoChar(buf, ')');
}

static void
deparseMinMaxExpr(MinMaxExpr* node, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    ListCell* lc;
    bool first;

    if (node->op == IS_GREATEST) {
        appendStringInfoString(buf, "greatest");
    } else {
        appendStringInfoString(buf, "least");
    }

    appendStringInfoChar(buf, '(');
    first = true;
    foreach (lc, node->args) {
        if (!first) {
            appendStringInfoString(buf, ", ");
        }

        first = false;

        deparseExpr(lfirst(lc), context);
    }
    appendStringInfoChar(buf, ')');
}

/*
 * Deparse GROUP BY clause.
 */
static void
appendGroupByClause(List* tlist, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    Query* query   = context->root->parse;
    ListCell* lc;
    bool first = true;

    /* Nothing to be done, if there's no GROUP BY clause in the query. */
    if (!query->groupClause) {
        return;
    }

    appendStringInfoString(buf, " GROUP BY ");

    /*
     * Queries with grouping sets are not pushed down, so we don't expect
     * grouping sets here.
     */
    Assert(!query->groupingSets);

    foreach (lc, query->groupClause) {
        SortGroupClause* grp = (SortGroupClause*)lfirst(lc);

        if (!first) {
            appendStringInfoString(buf, ", ");
        }

        first = false;

        deparseSortGroupClause(grp->tleSortGroupRef, tlist, true, context);
    }
}

/*
 * Deparse ORDER BY clause according to the given pathkeys for given base
 * relation. From given pathkeys expressions belonging entirely to the given
 * base relation are obtained and deparsed.
 */
static void
appendOrderByClause(List* pathkeys, bool has_final_sort, deparse_expr_cxt* context) {
    ListCell* lcell;
    char* delim               = " ";
    RelOptInfo* baserel       = context->scanrel;
    StringInfo buf            = context->buf;
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)context->foreignrel->fdw_private;

    appendStringInfoString(buf, " ORDER BY");
    foreach (lcell, pathkeys) {
        PathKey* pathkey = lfirst(lcell);
        Expr* em_expr;

        if (has_final_sort) {
            /*
             * By construction, context->foreignrel is the input relation to
             * the final sort.  Upper rels may have an empty reltarget; fall
             * back to the planner's upper target for that stage.
             */
            PathTarget* target = context->foreignrel->reltarget;

            if (target->exprs == NIL) {
                target = context->root->upper_targets[fpinfo->stage];
            }
            em_expr = chfdw_find_em_expr_for_input_target(
                context->root, pathkey->pk_eclass, target
            );
        } else if (
            IS_JOIN_REL(context->foreignrel) &&
            (fpinfo->jointype == JOIN_SEMI || fpinfo->jointype == JOIN_ANTI)
        ) {
            /*
             * For SEMI/ANTI JOINs, prefer expressions from the outer relation
             * since inner relation columns are not visible in the output.
             */
            em_expr = chfdw_find_em_expr_for_rel(pathkey->pk_eclass, fpinfo->outerrel);
            if (em_expr == NULL) {
                em_expr = chfdw_find_em_expr_for_rel(pathkey->pk_eclass, baserel);
            }
        } else {
            em_expr = chfdw_find_em_expr_for_rel(pathkey->pk_eclass, baserel);
        }

        Assert(em_expr != NULL);

        appendStringInfoString(buf, delim);
        deparseExpr(em_expr, context);
#if PG_VERSION_NUM >= 180000
        if (pathkey->pk_cmptype == BTLessStrategyNumber)
#else
        if (pathkey->pk_strategy == BTLessStrategyNumber)
#endif
            appendStringInfoString(buf, " ASC");
        else {
            appendStringInfoString(buf, " DESC");
        }

        if (pathkey->pk_nulls_first) {
            appendStringInfoString(buf, " NULLS FIRST");
        } else {
            appendStringInfoString(buf, " NULLS LAST");
        }

        delim = ", ";
    }
}

/*
 * Deparse LIMIT/OFFSET clause.
 */
static void
appendLimitClause(deparse_expr_cxt* context) {
    PlannerInfo* root = context->root;
    StringInfo buf    = context->buf;

    if (root->parse->limitCount) {
        appendStringInfoString(buf, " LIMIT ");
        deparseExpr((Expr*)root->parse->limitCount, context);
    }
    if (root->parse->limitOffset) {
        appendStringInfoString(buf, " OFFSET ");
        deparseExpr((Expr*)root->parse->limitOffset, context);
    }
}

/*
 * appendFunctionName
 *		Deparses function name from given function oid.
 *		Returns was custom or not.
 */
static CustomObjectDef*
appendFunctionName(Oid funcid, deparse_expr_cxt* context) {
    StringInfo buf = context->buf;
    HeapTuple proctup;
    Form_pg_proc procform;
    const char* proname;
    CustomObjectDef* cdef;
    CHFdwRelationInfo* fpinfo = context->scanrel->fdw_private;

    cdef = chfdw_check_for_custom_function(funcid);
    if (cdef && cdef->custom_name[0] != '\0') {
        if (cdef->custom_name[0] != '\1') {
            appendStringInfoString(buf, cdef->custom_name);
        }
        return cdef;
    }

    proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
    if (!HeapTupleIsValid(proctup)) {
        elog(ERROR, "cache lookup failed for function %u", funcid);
    }

    procform = (Form_pg_proc)GETSTRUCT(proctup);
    proname  = NameStr(procform->proname);

    /* we have some additional conditions on aggregation functions */
    if (chfdw_is_builtin(funcid) && procform->prokind == PROKIND_AGGREGATE &&
        fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE) {
        if (strcmp(proname, "sum") == 0) {
            cdef          = palloc(sizeof(CustomObjectDef));
            cdef->cf_oid  = funcid;
            cdef->cf_type = CF_SIGN_SUM;
        } else if (strcmp(proname, "avg") == 0) {
            cdef          = palloc(sizeof(CustomObjectDef));
            cdef->cf_oid  = funcid;
            cdef->cf_type = CF_SIGN_AVG;
            ;
            proname = "sum";
        } else if (strcmp(proname, "count") == 0) {
            cdef          = palloc(sizeof(CustomObjectDef));
            cdef->cf_oid  = funcid;
            cdef->cf_type = CF_SIGN_COUNT;
            proname       = "sum";
        }
    }

    /* Map PG aggregate names to ClickHouse equivalents */
    switch (funcid) {
    case F_BOOL_AND:
    case F_EVERY:
        proname = "groupBitAnd";
        break;
    case F_BOOL_OR:
        proname = "groupBitOr";
        break;
    case F_STRING_AGG_TEXT_TEXT:
        proname       = "groupConcat";
        cdef          = palloc0(sizeof(CustomObjectDef));
        cdef->cf_oid  = funcid;
        cdef->cf_type = CF_STRING_AGG;
        break;
    }

    /* Always print the function name */
    appendStringInfoString(buf, proname);

    ReleaseSysCache(proctup);

    return cdef;
}

/*
 * Appends a sort or group clause.
 *
 * Like get_rule_sortgroupclause(), returns the expression tree, so caller
 * need not find it again.
 */
static Node*
deparseSortGroupClause(
    Index ref,
    List* tlist,
    bool force_colno,
    deparse_expr_cxt* context
) {
    StringInfo buf = context->buf;
    TargetEntry* tle;
    Expr* expr;

    tle  = get_sortgroupref_tle(ref, tlist);
    expr = tle->expr;

    if (expr && IsA(expr, Const)) {
        /*
         * Force a typecast here so that we don't emit something like "GROUP
         * BY 2", which will be misconstrued as a column position rather than
         * a constant.
         */
        deparseConst((Const*)expr, context, 1);
    } else if (!expr || IsA(expr, Var) || context->no_sort_parens) {
        deparseExpr(expr, context);
    } else {
        /* Always parenthesize the expression. */
        appendStringInfoChar(buf, '(');
        deparseExpr(expr, context);
        appendStringInfoChar(buf, ')');
    }

    return (Node*)expr;
}

/*
 * Returns true if given Var is deparsed as a subquery output column, in
 * which case, *relno and *colno are set to the IDs for the relation and
 * column alias to the Var provided by the subquery.
 */
static bool
is_subquery_var(Var* node, RelOptInfo* foreignrel, int* relno, int* colno) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;
    RelOptInfo* outerrel      = fpinfo->outerrel;
    RelOptInfo* innerrel      = fpinfo->innerrel;

    /* Should only be called in these cases. */
    Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

    /*
     * If the given relation isn't a join relation, it doesn't have any lower
     * subqueries, so the Var isn't a subquery output column.
     */
    if (!IS_JOIN_REL(foreignrel)) {
        return false;
    }

    /*
     * If the Var doesn't belong to any lower subqueries, it isn't a subquery
     * output column.
     */
    if (!bms_is_member(node->varno, fpinfo->lower_subquery_rels)) {
        return false;
    }

    if (bms_is_member(node->varno, outerrel->relids)) {
        /*
         * If outer relation is deparsed as a subquery, the Var is an output
         * column of the subquery; get the IDs for the relation/column alias.
         */
        if (fpinfo->make_outerrel_subquery) {
            get_relation_column_alias_ids(node, outerrel, relno, colno);
            return true;
        }

        /* Otherwise, recurse into the outer relation. */
        return is_subquery_var(node, outerrel, relno, colno);
    } else {
        Assert(bms_is_member(node->varno, innerrel->relids));

        /*
         * If inner relation is deparsed as a subquery, the Var is an output
         * column of the subquery; get the IDs for the relation/column alias.
         */
        if (fpinfo->make_innerrel_subquery) {
            get_relation_column_alias_ids(node, innerrel, relno, colno);
            return true;
        }

        /* Otherwise, recurse into the inner relation. */
        return is_subquery_var(node, innerrel, relno, colno);
    }
}

/*
 * Get the IDs for the relation and column alias to given Var belonging to
 * given relation, which are returned into *relno and *colno.
 */
static void
get_relation_column_alias_ids(
    Var* node,
    RelOptInfo* foreignrel,
    int* relno,
    int* colno
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;
    int i;
    ListCell* lc;

    /* Get the relation alias ID */
    *relno = fpinfo->relation_index;

    /* Get the column alias ID */
    i = 1;
    foreach (lc, foreignrel->reltarget->exprs) {
        if (equal(lfirst(lc), (Node*)node)) {
            *colno = i;
            return;
        }
        i++;
    }

    /* Shouldn't get here */
    elog(ERROR, "unexpected expression in subquery output");
}
