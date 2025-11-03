/*-------------------------------------------------------------------------
 *
 * deparse.c
 *		  Query deparser for clickhouse_fdw
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/clickhouse_fdw/src/deparse.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "nodes/nodeFuncs.h"
#include "nodes/nodes.h"
#include "nodes/primnodes.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
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
#include "lib/stringinfo.h"

#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#endif

#include "fdw.h"

#ifndef MAXINT8LEN
#define MAXINT8LEN		25
#endif

/* variable counter */
static uint32 var_counter = 0;

/*
 * Global context for foreign_expr_walker's search of an expression tree.
 */
typedef struct foreign_glob_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
	Relids		relids;			/* relids of base relations in the underlying
								 * scan */
} foreign_glob_cxt;

typedef struct foreign_loc_cxt
{
	bool	found_AggregateFunction;	/* indicator or CH AggregateFunction fields */
} foreign_loc_cxt;

/*
 * Context for deparseExpr
 */
typedef struct deparse_expr_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
	RelOptInfo *scanrel;		/* the underlying scan relation. Same as
								 * foreignrel, when that represents a join or
								 * a base relation. */
	StringInfo	buf;			/* output buffer to append to */
	List	  **params_list;	/* exprs that will become remote Params */
	CustomObjectDef	*func;		/* custom function deparse */
	void		*func_arg;		/* custom function context args */
	CHFdwRelationInfo *fpinfo;	/* fdw relation info */
	bool		interval_op;
	bool		array_as_tuple;
} deparse_expr_cxt;

#define REL_ALIAS_PREFIX	"r"
/* Handy macro to add relation name qualification */
#define ADD_REL_QUALIFIER(buf, varno)	\
		appendStringInfo((buf), "%s%d.", REL_ALIAS_PREFIX, (varno))
#define SUBQUERY_REL_ALIAS_PREFIX	"s"
#define SUBQUERY_COL_ALIAS_PREFIX	"c"

#define CSTRING_TOLOWER(str) \
do { \
	for (int i = 0; str[i]; i++) { \
	  str[i] = tolower(str[i]); \
	} \
} while (0)

/*
 * Functions to determine whether an expression can be evaluated safely on
 * remote server.
 */
static bool foreign_expr_walker(Node *node,
					foreign_glob_cxt *glob_cxt,
					foreign_loc_cxt *outer_cxt);
static char *deparse_type_name(Oid type_oid, int32 typemod);

/*
 * Functions to construct string representation of a node tree.
 */
static void deparseTargetList(StringInfo buf,
				  RangeTblEntry *rte,
				  Index rtindex,
				  Relation rel,
				  Bitmapset *attrs_used,
				  bool qualify_col,
				  List **retrieved_attrs);
static void deparseExplicitTargetList(List *tlist,
						  List **retrieved_attrs,
						  deparse_expr_cxt *context);
static void deparseSubqueryTargetList(deparse_expr_cxt *context);
static void deparseColumnRef(StringInfo buf, CustomObjectDef *cdef,
	int varno, int varattno, RangeTblEntry *rte, bool qualify_col);
static void deparseRelation(StringInfo buf, Relation rel);
static void deparseExpr(Expr *expr, deparse_expr_cxt *context);
static void deparseVar(Var *node, deparse_expr_cxt *context);
static void deparseConst(Const *node, deparse_expr_cxt *context, int showtype);
static void deparseSubscriptingRef(SubscriptingRef *node, deparse_expr_cxt *context);
static void deparseFuncExpr(FuncExpr *node, deparse_expr_cxt *context);
static void deparseOpExpr(OpExpr *node, deparse_expr_cxt *context);
static void deparseOperatorName(StringInfo buf, Form_pg_operator opform);
static void deparseDistinctExpr(DistinctExpr *node, deparse_expr_cxt *context);
static void deparseScalarArrayOpExpr(ScalarArrayOpExpr *node,
                                     deparse_expr_cxt *context);
static void deparseCaseExpr(CaseExpr *node, deparse_expr_cxt *context);
static void deparseCaseWhen(CaseWhen *node, deparse_expr_cxt *context);
static void deparseRelabelType(RelabelType *node, deparse_expr_cxt *context);
static void deparseBoolExpr(BoolExpr *node, deparse_expr_cxt *context);
static void deparseNullTest(NullTest *node, deparse_expr_cxt *context);
static void deparseArrayExpr(ArrayExpr *node, deparse_expr_cxt *context);
static void deparseArrayList(ArrayExpr *node, deparse_expr_cxt *context);
static void deparseSelectSql(List *tlist, bool is_subquery, List **retrieved_attrs,
				 deparse_expr_cxt *context);
static void appendOrderByClause(List *pathkeys, bool has_final_sort,
					deparse_expr_cxt *context);
static void appendLimitClause(deparse_expr_cxt *context);
static void appendConditions(List *exprs, deparse_expr_cxt *context);
static void deparseFromExprForRel(StringInfo buf, PlannerInfo *root,
					  RelOptInfo *foreignrel, bool use_alias,
					  Index ignore_rel, List **ignore_conds,
					  List **params_list);
static void deparseFromExpr(List *quals, deparse_expr_cxt *context);
static void deparseRangeTblRef(StringInfo buf, PlannerInfo *root,
				   RelOptInfo *foreignrel, bool make_subquery,
				   Index ignore_rel, List **ignore_conds, List **params_list);
static void deparseAggref(Aggref *node, deparse_expr_cxt *context);
static void appendGroupByClause(List *tlist, deparse_expr_cxt *context);
static CustomObjectDef *appendFunctionName(Oid funcid, deparse_expr_cxt *context);
static Node *deparseSortGroupClause(Index ref, List *tlist, bool force_colno,
					   deparse_expr_cxt *context);
static void deparseCoerceViaIO(CoerceViaIO *node, deparse_expr_cxt *context);
static void deparseCoalesceExpr(CoalesceExpr *node, deparse_expr_cxt *context);
static void deparseMinMaxExpr(MinMaxExpr *node, deparse_expr_cxt *context);
static void deparseRowExpr(RowExpr *node, deparse_expr_cxt *context);
static void deparseNullIfExpr(NullIfExpr *node, deparse_expr_cxt *context);

/*
 * Helper functions
 */
static bool is_subquery_var(Var *node, RelOptInfo *foreignrel,
				int *relno, int *colno);
static void get_relation_column_alias_ids(Var *node, RelOptInfo *foreignrel,
							  int *relno, int *colno);

static char *
get_alias_name()
{
	var_counter++;
	return psprintf("_tmp%d", var_counter++);
}

/*
 * Examine each qual clause in input_conds, and classify them into two groups,
 * which are returned as two lists:
 *	- remote_conds contains expressions that can be evaluated remotely
 *	- local_conds contains expressions that can't be evaluated remotely
 */
void
chfdw_classify_conditions(PlannerInfo *root,
				   RelOptInfo *baserel,
				   List *input_conds,
				   List **remote_conds,
				   List **local_conds)
{
	ListCell   *lc;

	*remote_conds = NIL;
	*local_conds = NIL;

	foreach(lc, input_conds)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);

		if (chfdw_is_foreign_expr(root, baserel, ri->clause))
			*remote_conds = lappend(*remote_conds, ri);
		else
			*local_conds = lappend(*local_conds, ri);
	}
}

/*
 * Returns true if given expr is safe to evaluate on the foreign server.
 */
bool
chfdw_is_foreign_expr(PlannerInfo *root,
				RelOptInfo *baserel,
				Expr *expr)
{
	foreign_glob_cxt glob_cxt;
	foreign_loc_cxt loc_cxt = {false};
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *)(baserel->fdw_private);

	/*
	 * Check that the expression consists of nodes that are safe to execute
	 * remotely.
	 */
	glob_cxt.root = root;
	glob_cxt.foreignrel = baserel;

	/*
	 * For an upper relation, use relids from its underneath scan relation,
	 * because the upperrel's own relids currently aren't set to anything
	 * meaningful by the core code.  For other relation, use their own relids.
	 */
	if (IS_UPPER_REL(baserel))
		glob_cxt.relids = fpinfo->outerrel->relids;
	else
		glob_cxt.relids = baserel->relids;

	if (!foreign_expr_walker((Node *) expr, &glob_cxt, &loc_cxt))
		return false;

	/* OK to evaluate on the remote server */
	return true;
}

/* 1: '=', 2: '<>', 0 - false */
int
chfdw_is_equal_op(Oid opno)
{
	Form_pg_operator	operform;
	HeapTuple			opertup;
	int					res = 0;

	opertup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(opertup))
		elog(ERROR, "cache lookup failed for operator %u", opno);

	operform = (Form_pg_operator) GETSTRUCT(opertup);

	if (NameStr(operform->oprname)[0] == '=' && NameStr(operform->oprname)[1] == '\0')
		res = 1;
	else if (NameStr(operform->oprname)[0] == '<' && NameStr(operform->oprname)[1] == '>')
		res = 2;

	ReleaseSysCache(opertup);
	return res;
}

/*
 * Check if expression is safe to execute remotely, and return true if so.
 *
 * In addition, *outer_cxt is updated with collation information.
 *
 * We must check that the expression contains only node types we can deparse,
 * that all types/functions/operators are safe to send (they are "shippable"),
 * and that all collations used in the expression derive from Vars of the
 * foreign table.  Because of the latter, the logic is pretty close to
 * assign_collations_walker() in parse_collate.c, though we can assume here
 * that the given expression is valid.  Note function mutability is not
 * currently considered here.
 */
static bool
foreign_expr_walker(Node *node,
                    foreign_glob_cxt *glob_cxt,
                    foreign_loc_cxt *outer_cxt)
{
	bool		check_type = true;
	CHFdwRelationInfo *fpinfo;
	foreign_loc_cxt inner_cxt;

	/* Need do nothing for empty subexpressions */
	if (node == NULL)
		return true;

	/* May need server info from baserel's fdw_private struct */
	fpinfo = (CHFdwRelationInfo *)(glob_cxt->foreignrel->fdw_private);

	switch (nodeTag(node))
	{
	case T_Var:
	{
		Var		   *var = (Var *) node;

		/*
		 * If the Var is from the foreign table, we consider its
		 * collation (if any) safe to use.  If it is from another
		 * table, we treat its collation the same way as we would a
		 * Param's collation, ie it's not safe for it to have a
		 * non-default collation.
		 */
		if (bms_is_member(var->varno, glob_cxt->relids) &&
				var->varlevelsup == 0)
		{
			RangeTblEntry		*rte;
			CustomColumnInfo	*cinfo;

			/* Var belongs to foreign table */

			/*
			 * System columns other than ctid and oid should not be
			 * sent to the remote, since we don't make any effort to
			 * ensure that local and remote values match (tableoid, in
			 * particular, almost certainly doesn't match).
			 */
			if (var->varattno < 0)
				return false;

			rte = planner_rt_fetch(var->varno, glob_cxt->root);
			cinfo = chfdw_get_custom_column_info(rte->relid, var->varattno);

			if (cinfo && cinfo->is_AggregateFunction == CF_AGGR_FUNC)
				outer_cxt->found_AggregateFunction = true;
		}
	}
	break;
	case T_Const:
	break;
	case T_Param:
	{
		/* We are not supporting param push down*/
		return false;
	}
	break;
	case T_SubscriptingRef:
	{
		SubscriptingRef   *ar = (SubscriptingRef *) node;

		/* Assignment should not be in restrictions. */
		if (ar->refassgnexpr != NULL)
			return false;

		/*
		 * Recurse to remaining subexpressions.  Since the array
		 * subscripts must yield (noncollatable) integers, they won't
		 * affect the inner_cxt state.
		 */
		if (!foreign_expr_walker((Node *) ar->refupperindexpr,
								 glob_cxt, &inner_cxt))
			return false;

		if (!foreign_expr_walker((Node *) ar->reflowerindexpr,
								 glob_cxt, &inner_cxt))
			return false;

		if (!foreign_expr_walker((Node *) ar->refexpr,
								 glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_FuncExpr:
	{
		CustomObjectDef	*cdef = NULL;
		FuncExpr   *fe = (FuncExpr *) node;

		/* not in ClickHouse */
		if (fe->funcvariadic)
			return false;

		/*
		 * If function used by the expression is not shippable, it
		 * can't be sent to remote because it might have incompatible
		 * semantics on remote side.
		 */
		if (!chfdw_is_shippable(fe->funcid, ProcedureRelationId, fpinfo, &cdef))
			return false;

		/* only simple Var as first argument for accumulate */
		if (cdef && cdef->cf_type == CF_ISTORE_ACCUMULATE && (!IsA(linitial(fe->args), Var)))
			return false;

		/*
		 * Recurse to input subexpressions.
		 */
		if (!foreign_expr_walker((Node *) fe->args,
								 glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_OpExpr:
	case T_NullIfExpr:
	case T_DistinctExpr:	/* struct-equivalent to OpExpr */
	{
		OpExpr	   *oe = (OpExpr *) node;

		/*
		 * Similarly, only shippable operators can be sent to remote.
		 * (If the operator is shippable, we assume its underlying
		 * function is too.)
		 */
		if (!chfdw_is_shippable(oe->opno, OperatorRelationId, fpinfo, NULL))
			return false;

		/*
		 * Recurse to input subexpressions.
		 */
		if (!foreign_expr_walker((Node *) oe->args,
								 glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_ScalarArrayOpExpr:
	{
		ScalarArrayOpExpr *oe = (ScalarArrayOpExpr *) node;

		if (!chfdw_is_equal_op(oe->opno))
			return false;

		/*
		 * Recurse to input subexpressions.
		 */
		if (!foreign_expr_walker((Node *) oe->args,
								 glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_RelabelType:
	{
		RelabelType *r = (RelabelType *) node;

		/*
		 * Recurse to input subexpression.
		 */
		if (!foreign_expr_walker((Node *) r->arg,
								 glob_cxt, &inner_cxt))
		{
			return false;
		}
	}
	break;
	case T_BoolExpr:
	{
		BoolExpr   *b = (BoolExpr *) node;

		/*
		 * Recurse to input subexpressions.
		 */
		if (!foreign_expr_walker((Node *) b->args,
								 glob_cxt, &inner_cxt))
		{
			return false;
		}
	}
	break;
	case T_NullTest:
	{
		NullTest   *nt = (NullTest *) node;

		/*
		 * Recurse to input subexpressions.
		 */
		if (!foreign_expr_walker((Node *) nt->arg,
								 glob_cxt, &inner_cxt))
		{
			return false;
		}
	}
	break;
	case T_ArrayExpr:
	{
		ArrayExpr  *a = (ArrayExpr *) node;

		/*
		 * Recurse to input subexpressions.
		 */
		if (!foreign_expr_walker((Node *) a->elements,
								 glob_cxt, &inner_cxt))
		{
			return false;
		}
	}
	break;
	case T_List:
	{
		List	   *l = (List *) node;
		ListCell   *lc;

		/*
		 * Recurse to component subexpressions.
		 */
		foreach (lc, l)
		{
			if (!foreign_expr_walker((Node *) lfirst(lc),
									 glob_cxt, &inner_cxt))
			{
				return false;
			}
		}

		/* Don't apply exprType() to the list. */
		check_type = false;
	}
	break;
	case T_Aggref:
	{
		Aggref	   *agg = (Aggref *) node;
		ListCell   *lc;

		/* Not safe to pushdown when not in grouping context */
		if (!IS_UPPER_REL(glob_cxt->foreignrel))
			return false;

		/* Only non-split aggregates are pushable. */
		if (agg->aggsplit != AGGSPLIT_SIMPLE)
			return false;

		/* As usual, it must be shippable. */
		if (!chfdw_is_shippable(agg->aggfnoid, ProcedureRelationId, fpinfo, NULL))
			return false;

		/* Features that ClickHouse doesn't support */
		if (agg->aggorder)
			return false;

		if (agg->aggdistinct && agg->aggfilter)
			return false;

		/*
		 * Recurse to input args. aggdirectargs, aggorder and
		 * aggdistinct are all present in args, so no need to check
		 * their shippability explicitly.
		 */
		foreach (lc, agg->args)
		{
			Node	   *n = (Node *) lfirst(lc);

			/* If TargetEntry, extract the expression from it */
			if (IsA(n, TargetEntry))
			{
				TargetEntry *tle = (TargetEntry *) n;

				n = (Node *) tle->expr;
			}

			inner_cxt.found_AggregateFunction = false;
			if (!foreign_expr_walker(n, glob_cxt, &inner_cxt))
				return false;

			if (inner_cxt.found_AggregateFunction)
				agg->location = -2;
		}

		/* Check aggregate filter */
		if (!foreign_expr_walker((Node *) agg->aggfilter,
								 glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_CaseExpr:
	{
		CaseExpr   *caseexpr = (CaseExpr *) node;
		ListCell   *lc;

		if (!foreign_expr_walker((Node *) caseexpr->arg, glob_cxt, &inner_cxt))
			return true;

		foreach(lc, caseexpr->args)
		{
			CaseWhen   *when = lfirst_node(CaseWhen, lc);

			if (!foreign_expr_walker((Node *) when->expr, glob_cxt, &inner_cxt))
				return false;
			if (!foreign_expr_walker((Node *) when->result, glob_cxt, &inner_cxt))
				return false;
		}
		if (!foreign_expr_walker((Node *) caseexpr->defresult, glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_CoalesceExpr:
	{
		CoalesceExpr   *ce = (CoalesceExpr *) node;

		if (!foreign_expr_walker((Node *) ce->args, glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_MinMaxExpr:
	{
		MinMaxExpr	*me = (MinMaxExpr *) node;
		if (!foreign_expr_walker((Node *) me->args, glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_CoerceViaIO:
	{
		CoerceViaIO	*me = (CoerceViaIO *) node;
		if (!foreign_expr_walker((Node *) me->arg, glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_RowExpr:
	{
		RowExpr	*me = (RowExpr *) node;
		if (!foreign_expr_walker((Node *) me->args, glob_cxt, &inner_cxt))
			return false;
	}
	break;
	case T_CaseTestExpr:
	break;
	default:

		/*
		 * If it's anything else, assume it's unsafe.  This list can be
		 * expanded later, but don't forget to add deparse support below.
		 */
		return false;
	}

	/*
	 * If result type of given expression is not shippable, it can't be sent
	 * to remote because it might have incompatible semantics on remote side.
	 */
	if (check_type && !chfdw_is_shippable(exprType(node), TypeRelationId, fpinfo, NULL))
	{
		return false;
	}

	/* It looks OK */
	return true;
}

/*
 * Add typmod decoration to the basic type name
 */
static char *
printTypmod(const char *typname, int32 typmod, Oid typmodout)
{
	char	   *res;

	/* Shouldn't be called if typmod is -1 */
	Assert(typmod >= 0);

	if (typmodout == InvalidOid)
	{
		/* Default behavior: just print the integer typmod with parens */
		res = psprintf("%s(%d)", typname, (int) typmod);
	}
	else
	{
		/* Use the type-specific typmodout procedure */
		char	   *tmstr;

		tmstr = DatumGetCString(OidFunctionCall1(typmodout,
												 Int32GetDatum(typmod)));
		res = psprintf("%s%s", typname, tmstr);
	}

	return res;
}

static char *
ch_format_type_extended(Oid type_oid, int32 typemod, bits16 flags)
{
	HeapTuple	tuple;
	Form_pg_type typeform;
	Oid			array_base_type;
	bool		is_array;
	char	   *buf;
	bool		with_typemod;

	tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "clickhouse_fdw: cache lookup failed for type %u", type_oid);

	typeform = (Form_pg_type) GETSTRUCT(tuple);

	/*
	 * Check if it's a regular (variable length) array type.  Fixed-length
	 * array types such as "name" shouldn't get deconstructed.  As of Postgres
	 * 8.1, rather than checking typlen we check the toast property, and don't
	 * deconstruct "plain storage" array types --- this is because we don't
	 * want to show oidvector as oid[].
	 */
	array_base_type = typeform->typelem;

	if (array_base_type != InvalidOid && typeform->typstorage != 'p')
	{
		/* Switch our attention to the array element type */
		ReleaseSysCache(tuple);
		tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(array_base_type));
		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "clickhouse_fdw: cache lookup failed for type %u", type_oid);

		typeform = (Form_pg_type) GETSTRUCT(tuple);
		type_oid = array_base_type;
		is_array = true;
	}
	else
		is_array = false;

	with_typemod = (flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0 && (typemod >= 0);

	/*
	 * See if we want to special-case the output for certain built-in types.
	 * Note that these special cases should all correspond to special
	 * productions in gram.y, to ensure that the type name will be taken as a
	 * system type, not a user type of the same name.
	 *
	 * If we do not provide a special-case output here, the type name will be
	 * handled the same way as a user type name --- in particular, it will be
	 * double-quoted if it matches any lexer keyword.  This behavior is
	 * essential for some cases, such as types "bit" and "char".
	 */
	buf = NULL;					/* flag for no special case */

	switch (type_oid)
	{
		case BOOLOID:
			buf = pstrdup("Boolean");
			break;

		case BPCHAROID:
			if (with_typemod)
				buf = printTypmod("FixedString", typemod, typeform->typmodout);
			else if ((flags & FORMAT_TYPE_TYPEMOD_GIVEN) != 0)
			{
				/*
				 * bpchar with typmod -1 is not the same as CHARACTER, which
				 * means CHARACTER(1) per SQL spec.  Report it as bpchar so
				 * that parser will not assign a bogus typmod.
				 */
			}
			else
				buf = pstrdup("String");
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
			if (with_typemod)
				buf = printTypmod("Decimal", typemod, typeform->typmodout);
			else
				buf = pstrdup("Decimal");
			break;

		case INTERVALOID:
			if (with_typemod)
				buf = printTypmod("UInt64", typemod, typeform->typmodout);
			else
				buf = pstrdup("UInt64");
			break;

		case TIMESTAMPTZOID:
		case TIMESTAMPOID:
			buf = pstrdup("DateTime");
			break;
		case DATEOID:
			buf = pstrdup("Date");
			break;

		case VARCHAROID:
			if (with_typemod)
				buf = printTypmod("FixedString", typemod, typeform->typmodout);
			else
				buf = pstrdup("String");
			break;
		case TEXTOID:
			buf = pstrdup("String");
			break;
	}

	if (buf == NULL)
	{
		CustomObjectDef	*cdef;
		char	   *typname;

		cdef = chfdw_check_for_custom_type(type_oid);
		if (cdef && cdef->custom_name[0] != '\0')
			buf = pstrdup(cdef->custom_name);
		else
		{
			typname = NameStr(typeform->typname);
			buf = quote_qualified_identifier(NULL, typname);

			if (with_typemod)
				buf = printTypmod(buf, typemod, typeform->typmodout);
		}
	}

	if (is_array)
		buf = psprintf("Array(%s)", buf);

	ReleaseSysCache(tuple);

	return buf;
}

/*
 * Convert type OID + typmod info into a type name we can ship to the remote
 * server.  Someplace else had better have verified that this type name is
 * expected to be known on the remote end.
 *
 * This is almost just format_type_with_typemod(), except that if left to its
 * own devices, that function will make schema-qualification decisions based
 * on the local search_path, which is wrong.  We must schema-qualify all
 * type names that are not in pg_catalog.  We assume here that built-in types
 * are all in pg_catalog and need not be qualified; otherwise, qualify.
 */
static char *
deparse_type_name(Oid type_oid, int32 typemod)
{
	bits16		flags = FORMAT_TYPE_TYPEMOD_GIVEN;

	if (!chfdw_is_builtin(type_oid))
		flags |= FORMAT_TYPE_FORCE_QUALIFY;

	return ch_format_type_extended(type_oid, typemod, flags);
}

/*
 * Build the targetlist for given relation to be deparsed as SELECT clause.
 *
 * The output targetlist contains the columns that need to be fetched from the
 * foreign server for the given relation.  If foreignrel is an upper relation,
 * then the output targetlist can also contain expressions to be evaluated on
 * foreign server.
 */
List *
chfdw_build_tlist_to_deparse(RelOptInfo *foreignrel)
{
	List	   *tlist = NIL;
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;
	ListCell   *lc;

	/*
	 * For an upper relation, we have already built the target list while
	 * checking shippability, so just return that.
	 */
	if (IS_UPPER_REL(foreignrel))
		return fpinfo->grouped_tlist;

	/*
	 * We require columns specified in foreignrel->reltarget->exprs and those
	 * required for evaluating the local conditions.
	 */
	tlist = add_to_flat_tlist(tlist,
							  pull_var_clause((Node *) foreignrel->reltarget->exprs,
											  PVC_RECURSE_PLACEHOLDERS));
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		tlist = add_to_flat_tlist(tlist,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_RECURSE_PLACEHOLDERS));
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
chfdw_deparse_select_stmt_for_rel(StringInfo buf, PlannerInfo *root, RelOptInfo *rel,
						List *tlist, List *remote_conds, List *pathkeys,
						bool has_final_sort, bool has_limit, bool is_subquery,
						List **retrieved_attrs, List **params_list)
{
	deparse_expr_cxt context;
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) rel->fdw_private;
	List	   *quals;

	elog(DEBUG2, "> %s:%d", __FUNCTION__, __LINE__);
	/*
	 * We handle relations for foreign tables, joins between those and upper
	 * relations.
	 */
	Assert(IS_JOIN_REL(rel) || IS_SIMPLE_REL(rel) || IS_UPPER_REL(rel));

	/* Fill portions of context common to upper, join and base relation */
	context.buf = buf;
	context.root = root;
	context.foreignrel = rel;
	context.scanrel = IS_UPPER_REL(rel) ? fpinfo->outerrel : rel;
	context.params_list = params_list;
	context.func = NULL;
	context.interval_op = false;
	context.array_as_tuple = false;

	/* Construct SELECT clause */
	deparseSelectSql(tlist, is_subquery, retrieved_attrs, &context);

	/*
	 * For upper relations, the WHERE clause is built from the remote
	 * conditions of the underlying scan relation; otherwise, we can use the
	 * supplied list of remote conditions directly.
	 */
	if (IS_UPPER_REL(rel))
	{
		CHFdwRelationInfo *ofpinfo;

		ofpinfo = (CHFdwRelationInfo *) fpinfo->outerrel->fdw_private;
		quals = ofpinfo->remote_conds;
	}
	else
		quals = remote_conds;

	/* Construct FROM and WHERE clauses */
	deparseFromExpr(quals, &context);

	if (IS_UPPER_REL(rel))
	{
		/* Append GROUP BY clause */
		appendGroupByClause(tlist, &context);

		/* Append HAVING clause */
		if (remote_conds)
		{
			appendStringInfoString(buf, " HAVING ");
			appendConditions(remote_conds, &context);
		}
	}

	/* Add ORDER BY clause if we found any useful pathkeys */
	if (pathkeys)
		appendOrderByClause(pathkeys, has_final_sort, &context);

	/* Add LIMIT clause if necessary */
	if (has_limit)
		appendLimitClause(&context);
}

/*
 * Construct a simple SELECT statement that retrieves desired columns
 * of the specified foreign table, and append it to "buf".  The output
 * contains just "SELECT ... ".
 *
 * We also create an integer List of the columns being retrieved, which is
 * returned to *retrieved_attrs, unless we deparse the specified relation
 * as a subquery.
 *
 * tlist is the list of desired columns.  is_subquery is the flag to
 * indicate whether to deparse the specified relation as a subquery.
 * Read prologue of chfdw_deparse_select_stmt_for_rel() for details.
 */
static void
deparseSelectSql(List *tlist, bool is_subquery, List **retrieved_attrs,
				 deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	RelOptInfo *foreignrel = context->foreignrel;
	PlannerInfo *root = context->root;
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;

	/* reset var counter */
	var_counter = 0;
	elog(DEBUG2, "> %s:%d", __FUNCTION__, __LINE__);
	/*
	 * Construct SELECT list
	 */
	appendStringInfoString(buf, "SELECT ");

	if (is_subquery)
	{
		/*
		 * For a relation that is deparsed as a subquery, emit expressions
		 * specified in the relation's reltarget.  Note that since this is for
		 * the subquery, no need to care about *retrieved_attrs.
		 */
		deparseSubqueryTargetList(context);
	}
	else if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
	{
		/*
		 * For a join or upper relation the input tlist gives the list of
		 * columns required to be fetched from the foreign server.
		 */
		deparseExplicitTargetList(tlist, retrieved_attrs, context);
	}
	else
	{
		/*
		 * For a base relation fpinfo->attrs_used gives the list of columns
		 * required to be fetched from the foreign server.
		 */
		RangeTblEntry *rte = planner_rt_fetch(foreignrel->relid, root);

		/*
		 * Core code already has some lock on each rel being planned, so we
		 * can use NoLock here.
		 */
		Relation	rel = table_open_compat(rte->relid, NoLock);

		deparseTargetList(buf, rte, foreignrel->relid, rel,
						  fpinfo->attrs_used, false, retrieved_attrs);
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
deparseFromExpr(List *quals, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	RelOptInfo *scanrel = context->scanrel;

	/* For upper relations, scanrel must be either a joinrel or a baserel */
	Assert(!IS_UPPER_REL(context->foreignrel) ||
		   IS_JOIN_REL(scanrel) || IS_SIMPLE_REL(scanrel));

	/* Construct FROM clause */
	appendStringInfoString(buf, " FROM ");
	deparseFromExprForRel(buf, context->root, scanrel,
						  (bms_num_members(scanrel->relids) > 1),
						  (Index) 0, NULL, context->params_list);

	/* Construct WHERE clause */
	if (quals != NIL)
	{
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
deparseTargetList(StringInfo buf,
				  RangeTblEntry *rte,
				  Index rtindex,
				  Relation rel,
				  Bitmapset *attrs_used,
				  bool qualify_col,
				  List **retrieved_attrs)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	bool		have_wholerow;
	bool		first;
	int			i;

	*retrieved_attrs = NIL;

	/* If there's a whole-row reference, we'll need all the columns. */
	have_wholerow = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
								  attrs_used);

	first = true;
	for (i = 1; i <= tupdesc->natts; i++)
	{
		CustomObjectDef	*cdef;
		Form_pg_attribute attr = TupleDescAttr(tupdesc, i - 1);

		/* Ignore dropped attributes. */
		if (attr->attisdropped)
			continue;

		if (have_wholerow ||
			bms_is_member(i - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
		{
			if (!first)
				appendStringInfoString(buf, ", ");

			first = false;

			cdef = chfdw_check_for_custom_type(attr->atttypid);
			deparseColumnRef(buf, cdef, rtindex, i, rte, qualify_col);

			*retrieved_attrs = lappend_int(*retrieved_attrs, i);
		}
	}

	/*
	 * check for ctid and oid
	 */
	if (bms_is_member(SelfItemPointerAttributeNumber -
					  FirstLowInvalidHeapAttributeNumber,
					  attrs_used))
		elog(ERROR, "clickhouse does not support system columns");

	/* Don't generate bad syntax if no undropped columns */
	if (first)
	{
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
appendConditions(List *exprs, deparse_expr_cxt *context)
{
	ListCell   *lc;
	bool		is_first = true;
	StringInfo	buf = context->buf;

	foreach (lc, exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/* Extract clause from RestrictInfo, if required */
		if (IsA(expr, RestrictInfo))
		{
			expr = ((RestrictInfo *) expr)->clause;
		}

		/* Connect expressions with "AND" and parenthesize each condition. */
		if (!is_first)
		{
			appendStringInfoString(buf, " AND ");
		}

		appendStringInfoChar(buf, '(');
		deparseExpr(expr, context);
		appendStringInfoChar(buf, ')');

		is_first = false;
	}
}

/* Output join name for given join type */
const char *
chfdw_get_jointype_name(JoinType jointype)
{
	switch (jointype)
	{
	case JOIN_INNER:
		return "INNER";

	case JOIN_LEFT:
		return "LEFT";

	case JOIN_RIGHT:
		return "RIGHT";

	case JOIN_FULL:
		return "FULL";

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
deparseExplicitTargetList(List *tlist,
                          List **retrieved_attrs,
                          deparse_expr_cxt *context)
{
	ListCell   *lc;
	StringInfo	buf = context->buf;
	int			i = 0;

	*retrieved_attrs = NIL;

	foreach (lc, tlist)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);

		if (i > 0)
			appendStringInfoString(buf, ", ");

		deparseExpr((Expr *) tle->expr, context);

		*retrieved_attrs = lappend_int(*retrieved_attrs, i + 1);
		i++;
	}

	if (i == 0)
		appendStringInfoString(buf, "NULL");
}

/*
 * Emit expressions specified in the given relation's reltarget.
 *
 * This is used for deparsing the given relation as a subquery.
 */
static void
deparseSubqueryTargetList(deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	RelOptInfo *foreignrel = context->foreignrel;
	bool		first;
	ListCell   *lc;

	/* Should only be called in these cases. */
	Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

	first = true;
	foreach(lc, foreignrel->reltarget->exprs)
	{
		Node	   *node = (Node *) lfirst(lc);

		if (!first)
			appendStringInfoString(buf, ", ");

		first = false;

		deparseExpr((Expr *) node, context);
	}

	/* Don't generate bad syntax if no expressions */
	if (first)
		appendStringInfoString(buf, "NULL");
}

/*
 * Construct FROM clause for given relation
 *
 * The function constructs ... JOIN ... ON ... for join relation. For a base
 * relation it just returns schema-qualified tablename, with the appropriate
 * alias if so requested.
 *
 * 'ignore_rel' is either zero or the RT index of a target relation.  In the
 * latter case the function constructs FROM clause of UPDATE or USING clause
 * of DELETE; it deparses the join relation as if the relation never contained
 * the target relation, and creates a List of conditions to be deparsed into
 * the top-level WHERE clause, which is returned to *ignore_conds.
 */
static void
deparseFromExprForRel(StringInfo buf, PlannerInfo *root, RelOptInfo *foreignrel,
                      bool use_alias, Index ignore_rel, List **ignore_conds,
                      List **params_list)
{
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;

	if (IS_JOIN_REL(foreignrel))
	{
		StringInfoData join_sql_o;
		StringInfoData join_sql_i;
		RelOptInfo *outerrel = fpinfo->outerrel;
		RelOptInfo *innerrel = fpinfo->innerrel;
		bool		outerrel_is_target = false;
		bool		innerrel_is_target = false;

		if (ignore_rel > 0 && bms_is_member(ignore_rel, foreignrel->relids))
		{
			/*
			 * If this is an inner join, add joinclauses to *ignore_conds and
			 * set it to empty so that those can be deparsed into the WHERE
			 * clause.  Note that since the target relation can never be
			 * within the nullable side of an outer join, those could safely
			 * be pulled up into the WHERE clause (see foreign_join_ok()).
			 * Note also that since the target relation is only inner-joined
			 * to any other relation in the query, all conditions in the join
			 * tree mentioning the target relation could be deparsed into the
			 * WHERE clause by doing this recursively.
			 */
			if (fpinfo->jointype == JOIN_INNER)
			{
				*ignore_conds = list_concat(*ignore_conds,
											list_copy(fpinfo->joinclauses));
				fpinfo->joinclauses = NIL;
			}

			/*
			 * Check if either of the input relations is the target relation.
			 */
			if (outerrel->relid == ignore_rel)
			{
				outerrel_is_target = true;
			}
			else if (innerrel->relid == ignore_rel)
			{
				innerrel_is_target = true;
			}
		}

		/* Deparse outer relation if not the target relation. */
		if (!outerrel_is_target)
		{
			initStringInfo(&join_sql_o);
			deparseRangeTblRef(&join_sql_o, root, outerrel,
							   fpinfo->make_outerrel_subquery,
							   ignore_rel, ignore_conds, params_list);

			/*
			 * If inner relation is the target relation, skip deparsing it.
			 * Note that since the join of the target relation with any other
			 * relation in the query is an inner join and can never be within
			 * the nullable side of an outer join, the join could be
			 * interchanged with higher-level joins (cf. identity 1 on outer
			 * join reordering shown in src/backend/optimizer/README), which
			 * means it's safe to skip the target-relation deparsing here.
			 */
			if (innerrel_is_target)
			{
				Assert(fpinfo->jointype == JOIN_INNER);
				Assert(fpinfo->joinclauses == NIL);
				appendStringInfo(buf, "%s", join_sql_o.data);
				return;
			}
		}

		/* Deparse inner relation if not the target relation. */
		if (!innerrel_is_target)
		{
			initStringInfo(&join_sql_i);
			deparseRangeTblRef(&join_sql_i, root, innerrel,
							   fpinfo->make_innerrel_subquery,
							   ignore_rel, ignore_conds, params_list);

			/*
			 * If outer relation is the target relation, skip deparsing it.
			 * See the above note about safety.
			 */
			if (outerrel_is_target)
			{
				Assert(fpinfo->jointype == JOIN_INNER);
				Assert(fpinfo->joinclauses == NIL);
				appendStringInfo(buf, "%s", join_sql_i.data);
				return;
			}
		}

		/* Neither of the relations is the target relation. */
		Assert(!outerrel_is_target && !innerrel_is_target);

		/*
		 * For a join relation FROM clause entry is deparsed as
		 *
		 * ((outer relation) <join type> (inner relation) ON (joinclauses))
		 */
		appendStringInfo(buf, " %s ALL %s JOIN %s ON ", join_sql_o.data,
						 chfdw_get_jointype_name(fpinfo->jointype), join_sql_i.data);

		/* Append join clause; (TRUE) if no join clause */
		if (fpinfo->joinclauses)
		{
			deparse_expr_cxt context;

			context.buf = buf;
			context.foreignrel = foreignrel;
			context.scanrel = foreignrel;
			context.root = root;
			context.params_list = params_list;
			context.func = NULL;
			context.interval_op = false;
			context.array_as_tuple = false;

			appendStringInfoChar(buf, '(');
			appendConditions(fpinfo->joinclauses, &context);
			appendStringInfoChar(buf, ')');
		}
		else
		{
			appendStringInfoString(buf, "(TRUE)");
		}
	}
	else
	{
		RangeTblEntry *rte = planner_rt_fetch(foreignrel->relid, root);

		/*
		 * Core code already has some lock on each rel being planned, so we
		 * can use NoLock here.
		 */
		Relation	rel = table_open_compat(rte->relid, NoLock);

		deparseRelation(buf, rel);

		/*
		 * Add a unique alias to avoid any conflict in relation names due to
		 * pulled up subqueries in the query being built for a pushed down
		 * join.
		 */
		if (use_alias)
		{
			appendStringInfo(buf, " %s%d", REL_ALIAS_PREFIX, foreignrel->relid);
		}

		table_close_compat(rel, NoLock);
	}
}

/*
 * Append FROM clause entry for the given relation into buf.
 */
static void
deparseRangeTblRef(StringInfo buf, PlannerInfo *root, RelOptInfo *foreignrel,
                   bool make_subquery, Index ignore_rel, List **ignore_conds,
                   List **params_list)
{
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;

	/* Should only be called in these cases. */
	Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

	Assert(fpinfo->local_conds == NIL);

	/* If make_subquery is true, deparse the relation as a subquery. */
	if (make_subquery)
	{
		List	   *retrieved_attrs;
		int			ncols;

		/*
		 * The given relation shouldn't contain the target relation, because
		 * this should only happen for input relations for a full join, and
		 * such relations can never contain an UPDATE/DELETE target.
		 */
		Assert(ignore_rel == 0 ||
			   !bms_is_member(ignore_rel, foreignrel->relids));

		/* Deparse the subquery representing the relation. */
		appendStringInfoChar(buf, '(');
		chfdw_deparse_select_stmt_for_rel(buf, root, foreignrel, NIL,
								fpinfo->remote_conds, NIL,
                                false, false, true,
								&retrieved_attrs, params_list);
		appendStringInfoChar(buf, ')');

		/* Append the relation alias. */
		appendStringInfo(buf, " %s%d", SUBQUERY_REL_ALIAS_PREFIX,
						 fpinfo->relation_index);

		/*
		 * Append the column aliases if needed.  Note that the subquery emits
		 * expressions specified in the relation's reltarget (see
		 * deparseSubqueryTargetList).
		 */
		ncols = list_length(foreignrel->reltarget->exprs);
		if (ncols > 0)
		{
			int			i;

			appendStringInfoChar(buf, '(');
			for (i = 1; i <= ncols; i++)
			{
				if (i > 1)
				{
					appendStringInfoString(buf, ", ");
				}

				appendStringInfo(buf, "%s%d", SUBQUERY_COL_ALIAS_PREFIX, i);
			}
			appendStringInfoChar(buf, ')');
		}
	}
	else
		deparseFromExprForRel(buf, root, foreignrel, true, ignore_rel,
							  ignore_conds, params_list);
}

/*
 * deparse remote INSERT statement
 */
char *
chfdw_deparse_insert_sql(StringInfo buf, RangeTblEntry *rte,
                 Index rtindex, Relation rel,
                 List *targetAttrs)
{
	bool    first;
	ListCell   *lc;
	StringInfoData	table_name;

	initStringInfo(&table_name);
	appendStringInfoString(buf, "INSERT INTO ");
	deparseRelation(&table_name, rel);
	appendStringInfoString(buf, table_name.data);

	if (targetAttrs)
	{
		appendStringInfoChar(buf, '(');

		first = true;
		foreach (lc, targetAttrs)
		{
			int			attnum = lfirst_int(lc);

			if (!first)
				appendStringInfoString(buf, ", ");

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
deparseColumnRef(StringInfo buf, CustomObjectDef *cdef,
				 int varno, int varattno, RangeTblEntry *rte,
                 bool qualify_col)
{
	char	   *colname = NULL;
	CustomColumnInfo	*cinfo;

	/* varno must not be any of OUTER_VAR, INNER_VAR and INDEX_VAR. */
	Assert(!IS_SPECIAL_VARNO(varno));

	if (varattno <= 0)
		elog(ERROR, "ClickHouse does not support system attributes");

	/* Get FDW specific options for this column */
	cinfo = chfdw_get_custom_column_info(rte->relid, varattno);
	if (cinfo)
		colname = cinfo->colname;

	/*
	 * If it's a column of a regular table or it doesn't have column_name
	 * FDW option, use attribute name.
	 */
	if (colname == NULL)
		colname = get_attname(rte->relid, varattno, false);

	if (cinfo && cinfo->coltype == CF_ISTORE_ARR)
	{
		char *colkey = psprintf("%s_keys", colname);
		char *colval = psprintf("%s_values", colname);

		if (cdef && cdef->cf_type == CF_ISTORE_SUM)
		{
			/* SUM context */
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colkey));
			appendStringInfoChar(buf, ',');

			if (cinfo->table_engine == CH_COLLAPSING_MERGE_TREE)
			{
				appendStringInfoString(buf, "arrayMap(x -> x * ");
				appendStringInfoString(buf, cinfo->signfield);
				appendStringInfoChar(buf, ',');
				if (qualify_col)
					ADD_REL_QUALIFIER(buf, varno);
				appendStringInfoString(buf, quote_identifier(colval));
				appendStringInfoChar(buf, ')');
			}
			else
			{
				if (qualify_col)
					ADD_REL_QUALIFIER(buf, varno);
				appendStringInfoString(buf, quote_identifier(colval));
			}
		}
		else if (cdef && cdef->cf_type == CF_ISTORE_FETCHVAL)
		{
			/* values[indexOf(ids, */
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colval));
			appendStringInfoString(buf, "[nullif(indexOf(");
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colkey));
			appendStringInfoChar(buf, ',');
		}
		else if (cdef && cdef->cf_type == CF_ISTORE_SUM_UP)
		{
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colval));

			if (cdef->cf_context && list_length(cdef->cf_context) == 2)
			{
				appendStringInfoChar(buf, ',');
				if (qualify_col)
					ADD_REL_QUALIFIER(buf, varno);
				appendStringInfoString(buf, quote_identifier(colkey));
			}
		}
		else
		{
			appendStringInfoChar(buf, '(');
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colkey));
			appendStringInfoChar(buf, ',');
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colval));
			appendStringInfoChar(buf, ')');
		}

		pfree(colkey);
		pfree(colval);
	}
	else if (cinfo && cinfo->coltype == CF_ISTORE_COL)
	{
		char *alias_name = get_alias_name();

		if (cdef && cdef->cf_type == CF_ISTORE_FETCHVAL)
		{
			/* ((finalizeAggregation(colname) as _tmp).2)[indexOf(_tmp.1, - we fill this part
			 * <key>)] - should be filled later */

			appendStringInfoString(buf, "((");

			if (cinfo->is_AggregateFunction == CF_AGGR_FUNC)
				appendStringInfoString(buf, "finalizeAggregation(");
			else
				appendStringInfoChar(buf, '(');

			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);

			appendStringInfoString(buf, quote_identifier(colname));
			appendStringInfo(buf, ") as %s).2)[nullif(indexOf(%s.1, ", alias_name, alias_name);
		}
		else if (cdef && cdef->cf_type == CF_ISTORE_SUM_UP)
		{
			appendStringInfoChar(buf, '(');

			if (cinfo->is_AggregateFunction == CF_AGGR_FUNC)
				appendStringInfoString(buf, "finalizeAggregation(");
			else
				appendStringInfoChar(buf, '(');

			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colname));
			appendStringInfo(buf, ") as %s).2", alias_name);

			if (cdef->cf_context && list_length(cdef->cf_context) == 2)
				appendStringInfo(buf, ", %s.1", alias_name);
		}
		else
		{
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, varno);
			appendStringInfoString(buf, quote_identifier(colname));
		}

		pfree(alias_name);
	}
	else
	{
		if (qualify_col)
			ADD_REL_QUALIFIER(buf, varno);
		appendStringInfoString(buf, quote_identifier(colname));
	}
}

/*
 * Append remote name of specified foreign table to buf.
 * Use value of table_name FDW option (if any) instead of relation's name.
 * Similarly, schema_name FDW option overrides schema name.
 */
static void
deparseRelation(StringInfo buf, Relation rel)
{
	ForeignTable *table;
	const char *relname = NULL;
	char       *dbname = "default";
	ForeignServer *server = chfdw_get_foreign_server(rel);
	ListCell    *lc;

	chfdw_extract_options(server->options, NULL, NULL, NULL, &dbname, NULL, NULL);

	/* obtain additional catalog information. */
	table = GetForeignTable(RelationGetRelid(rel));

	/*
	 * Use value of FDW options if any, instead of the name of object itself.
	 */
	foreach (lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "table_name") == 0)
		{
			relname = defGetString(def);
		}
		else if (strcmp(def->defname, "database") == 0)
        {
			dbname = defGetString(def);
        }
	}
	if (relname == NULL)
	{
		relname = RelationGetRelationName(rel);
	}

	appendStringInfo(buf, "%s.%s", quote_identifier(dbname),
					 quote_identifier(relname));
}

/*
 * Append a SQL string literal representing "val" to buf.
 */
static void
deparseStringLiteral(StringInfo buf, const char *val, bool quote)
{
	const char *valptr;

	if (quote)
		appendStringInfoChar(buf, '\'');
	for (valptr = val; *valptr; valptr++)
	{
		char		ch = *valptr;

		if (SQL_STR_DOUBLE(ch, true))
		{
			appendStringInfoChar(buf, ch);
		}
		appendStringInfoChar(buf, ch);
	}
	if (quote)
		appendStringInfoChar(buf, '\'');
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
deparseExpr(Expr *node, deparse_expr_cxt *context)
{
	if (node == NULL)
	{
		return;
	}

	switch (nodeTag(node))
	{
	case T_Var:
		deparseVar((Var *) node, context);
		break;
	case T_Const:
		deparseConst((Const *) node, context, 0);
		break;
	case T_SubscriptingRef:
		deparseSubscriptingRef((SubscriptingRef *) node, context);
		break;
	case T_FuncExpr:
		deparseFuncExpr((FuncExpr *) node, context);
		break;
	case T_OpExpr:
		deparseOpExpr((OpExpr *) node, context);
		break;
	case T_DistinctExpr:
		deparseDistinctExpr((DistinctExpr *) node, context);
		break;
	case T_NullIfExpr:
		deparseNullIfExpr((NullIfExpr *) node, context);
		break;
	case T_ScalarArrayOpExpr:
		deparseScalarArrayOpExpr((ScalarArrayOpExpr *) node, context);
		break;
	case T_RelabelType:
		deparseRelabelType((RelabelType *) node, context);
		break;
	case T_BoolExpr:
		deparseBoolExpr((BoolExpr *) node, context);
		break;
	case T_NullTest:
		deparseNullTest((NullTest *) node, context);
		break;
	case T_ArrayExpr:
		deparseArrayExpr((ArrayExpr *) node, context);
		break;
	case T_Aggref:
		deparseAggref((Aggref *) node, context);
		break;
	case T_CaseExpr:
		deparseCaseExpr((CaseExpr *) node, context);
		break;
	case T_CaseWhen:
		deparseCaseWhen((CaseWhen *) node, context);
		break;
	case T_CoalesceExpr:
		deparseCoalesceExpr((CoalesceExpr *) node, context);
		break;
	case T_MinMaxExpr:
		deparseMinMaxExpr((MinMaxExpr *) node, context);
		break;
	case T_CoerceViaIO:
		deparseCoerceViaIO((CoerceViaIO *) node, context);
		break;
	case T_RowExpr:
		deparseRowExpr((RowExpr *) node, context);
		break;
	default:
		elog(ERROR, "unsupported expression type for deparse: %d",
			 (int) nodeTag(node));
		break;
	}
}

/*
 * Deparse given Var node into context->buf.
 *
 * If the Var belongs to the foreign relation, just print its remote name.
 * Otherwise, it's effectively a Param (and will in fact be a Param at
 * run time).  Handle it the same way we handle plain Params.
 */
static void
deparseVar(Var *node, deparse_expr_cxt *context)
{
	CustomObjectDef	*cdef;
	Relids		relids = context->scanrel->relids;
	int			relno;
	int			colno;

	/* Qualify columns when multiple relations are involved. */
	bool		qualify_col = (bms_num_members(relids) > 1);

	/*
	 * If the Var belongs to the foreign relation that is deparsed as a
	 * subquery, use the relation and column alias to the Var provided by the
	 * subquery, instead of the remote name.
	 */
	if (is_subquery_var(node, context->scanrel, &relno, &colno))
	{
		appendStringInfo(context->buf, "%s%d.%s%d",
						 SUBQUERY_REL_ALIAS_PREFIX, relno,
						 SUBQUERY_COL_ALIAS_PREFIX, colno);
		return;
	}

	cdef = context->func;
	if (!cdef)
		cdef = chfdw_check_for_custom_type(node->vartype);

	if (bms_is_member(node->varno, relids) && node->varlevelsup == 0)
		deparseColumnRef(context->buf, cdef,
						 node->varno, node->varattno,
						 planner_rt_fetch(node->varno, context->root),
						 qualify_col);
	else
		elog(ERROR, "clickhouse_fdw does not support params");
}

#define USE_ISO_DATES			1

Datum
ch_time_out(PG_FUNCTION_ARGS)
{
	TimeADT		time = PG_GETARG_TIMEADT(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	time2tm(time, tm, &fsec);
	EncodeTimeOnly(tm, fsec, false, 0, USE_ISO_DATES, buf);

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

/* date_out()
 * Given internal format date, convert to text string.
 */
Datum
ch_date_out(PG_FUNCTION_ARGS)
{
	DateADT		date = PG_GETARG_DATEADT(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	char		buf[MAXDATELEN + 1];

	if (DATE_NOT_FINITE(date))
		EncodeSpecialDate(date, buf);
	else
	{
		j2date(date + POSTGRES_EPOCH_JDATE,
			   &(tm->tm_year), &(tm->tm_mon), &(tm->tm_mday));
		EncodeDateOnly(tm, USE_ISO_DATES, buf);
	}

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

Datum
ch_timestamp_out(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(0);
	char	   *result;
	struct pg_tm tt,
			   *tm = &tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];

	if (TIMESTAMP_NOT_FINITE(timestamp))
		EncodeSpecialTimestamp(timestamp, buf);
	else if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) == 0)
		/* we ignore ftractional seconds */
		EncodeDateTime(tm, 0, false, 0, NULL, USE_ISO_DATES, buf);
	else
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	result = pstrdup(buf);
	PG_RETURN_CSTRING(result);
}

static void
deparseArray(Datum arr, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	AnyArrayType *array = DatumGetAnyArrayP(arr);
	int			ndims = AARR_NDIM(array);
	int		   *dims = AARR_DIMS(array);
	Oid			element_type = AARR_ELEMTYPE(array);

	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typdelim;
	Oid			typioparam;
	Oid			typiofunc;
	int			nitems;
	array_iter	iter;
	int			i;
	bool		first;

	if (ndims > 1)
		elog(ERROR, "only one dimension of arrays supported by clickhouse_fdw");

	get_type_io_data(element_type, IOFunc_output,
					 &typlen, &typbyval,
					 &typalign, &typdelim,
					 &typioparam, &typiofunc);

	/* Loop over source data */
	nitems = ArrayGetNItems(ndims, dims);
	array_iter_setup(&iter, array);
	first = true;

	if (context->array_as_tuple)
		appendStringInfoChar(buf, '(');
	else
		appendStringInfoChar(buf, '[');

	for (i = 0; i < nitems; i++)
	{
		Datum		elt;
		bool		isnull;

		if (!first)
			appendStringInfoChar(buf, ',');
		first = false;

		/* Get element, checking for NULL */
		elt = array_iter_next(&iter, &isnull, i, typlen, typbyval, typalign);

		if (isnull)
		{
			appendStringInfoString(buf, "NULL");
		}
		else
		{
			char *extval = OidOutputFunctionCall(typiofunc, elt);
			switch (element_type)
			{
			case INT2OID:
			case INT4OID:
			case INT8OID:
			case OIDOID:
			case FLOAT4OID:
			case FLOAT8OID:
			case NUMERICOID:
			{
				/*
				 * No need to quote unless it's a special value such as 'NaN'.
				 * See comments in get_const_expr().
				 */
				if (strspn(extval, "0123456789+-eE.") == strlen(extval))
				{
					if (extval[0] == '+' || extval[0] == '-')
						appendStringInfo(buf, "(%s)", extval);
					else
						appendStringInfoString(buf, extval);
				}
				else
					appendStringInfo(buf, "'%s'", extval);
			}
			break;
			case BOOLOID:
				if (strcmp(extval, "t") == 0)
					appendStringInfoString(buf, "true");
				else
					appendStringInfoString(buf, "false");
				break;
			default:
				deparseStringLiteral(buf, extval, true);
				break;
			}
			pfree(extval);
		}
	}
	if (context->array_as_tuple)
		appendStringInfoChar(buf, ')');
	else
		appendStringInfoChar(buf, ']');
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
deparseConst(Const *node, deparse_expr_cxt *context, int showtype)
{
	StringInfo	buf = context->buf;
	Oid			typoutput;
	bool		typIsVarlena;
	char	   *extval = NULL;
	bool		closebr = false;

	if (node->constisnull)
	{
		appendStringInfoString(buf, "NULL");
		return;
	}

	if (showtype > 0)
		appendStringInfoString(buf, "cast(");

	getTypeOutputInfo(node->consttype,
					  &typoutput, &typIsVarlena);

	if (typoutput == F_TIMESTAMPTZ_OUT || typoutput == F_TIMESTAMP_OUT)
	{
		/*
		 * We use our own function here, that removes fractional seconds since
		 * there are not supported in clickhouse
		 * */
		extval = DatumGetCString(DirectFunctionCall1(ch_timestamp_out, node->constvalue));
	}
	else if (typoutput == F_INTERVAL_OUT)
	{
		/* basicly we can't convert month part since we should know about
		 * related timestamp first.
		 *
		 * for other types we just convert to seconds.
		 * */
		uint64		sec;
		Interval   *ival = DatumGetIntervalP(node->constvalue);
		char		bufint8[MAXINT8LEN + 1];

		if (ival->month != 0)
			elog(ERROR, "we can't convert interval with months into clickhouse");

		sec = 86400 /* sec in day */ * ival->day + (int64) (ival->time / 1000000);
		pg_lltoa(sec, bufint8);
		appendStringInfoString(buf, bufint8);
		goto cleanup;
	}
	else if (typoutput == F_ARRAY_OUT)
	{
		deparseArray(node->constvalue, context);
		goto cleanup;
	}
	else
	{
		CustomObjectDef *cdef = chfdw_check_for_custom_function(typoutput);

		extval = OidOutputFunctionCall(typoutput, node->constvalue);
		if (cdef && cdef->cf_type == CF_AJBOOL_OUT)
		{
			/* ajbool:
				'f' => 0
				't' => 1
				'u' => -1
			*/
			if (extval[0] == 'f')
				appendStringInfoChar(buf, '0');
			else if (extval[0] == 't')
				appendStringInfoChar(buf, '1');
			else if (extval[0] == 'u')
				appendStringInfoString(buf, "-1");
			else
				elog(ERROR, "unexpected output of ajbool");

			goto cleanup;
		}
		else if (cdef && cdef->cf_type == CF_AJTIME_OUT)
		{
			closebr = true;
			appendStringInfoString(buf, "toDateTime(");
		}
	}

	switch (node->consttype)
	{
	case INT2OID:
	case INT4OID:
	case INT8OID:
	case OIDOID:
	case FLOAT4OID:
	case FLOAT8OID:
	case NUMERICOID:
	{
		/*
		 * No need to quote unless it's a special value such as 'NaN'.
		 * See comments in get_const_expr().
		 */
		if (strspn(extval, "0123456789+-eE.") == strlen(extval))
		{
			if (extval[0] == '+' || extval[0] == '-')
				appendStringInfo(buf, "(%s)", extval);
			else
				appendStringInfoString(buf, extval);
		}
		else
			appendStringInfo(buf, "'%s'", extval);
	}
	break;
	case BITOID:
	case VARBITOID:
		appendStringInfo(buf, "B'%s'", extval);
		break;
	case BOOLOID:
		if (strcmp(extval, "t") == 0)
			appendStringInfoString(buf, "1");
		else
			appendStringInfoString(buf, "0");
		break;
	default:
		deparseStringLiteral(buf, extval, true);
		break;
	}

	if (closebr)
		appendStringInfoChar(buf, ')');

cleanup:
	if (showtype > 0)
		appendStringInfo(buf, " as %s)",
						 deparse_type_name(node->consttype,
										   node->consttypmod));
	if (extval)
		pfree(extval);

}

/*
 * Deparse an array subscript expression.
 */
static void
deparseSubscriptingRef(SubscriptingRef *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lowlist_item;
	ListCell   *uplist_item;

	/* Always parenthesize the expression. */
	appendStringInfoChar(buf, '(');

	/*
	 * Deparse referenced array expression first.  If that expression includes
	 * a cast, we have to parenthesize to prevent the array subscript from
	 * being taken as typename decoration.  We can avoid that in the typical
	 * case of subscripting a Var, but otherwise do it.
	 */
	if (IsA(node->refexpr, Var))
	{
		deparseExpr(node->refexpr, context);
	}
	else
	{
		appendStringInfoChar(buf, '(');
		deparseExpr(node->refexpr, context);
		appendStringInfoChar(buf, ')');
	}

	/* Deparse subscript expressions. */
	lowlist_item = list_head(node->reflowerindexpr);	/* could be NULL */
	foreach (uplist_item, node->refupperindexpr)
	{
		appendStringInfoChar(buf, '[');
		if (lowlist_item)
		{
			deparseExpr(lfirst(lowlist_item), context);
			appendStringInfoChar(buf, ':');
			lowlist_item = lnext_compat(node->reflowerindexpr, lowlist_item);
		}
		deparseExpr(lfirst(uplist_item), context);
		appendStringInfoChar(buf, ']');
	}

	appendStringInfoChar(buf, ')');
}

/*
 * Deparse a function call.
 */
static void
deparseFuncExpr(FuncExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	bool		first;
	ListCell   *arg;
	CustomObjectDef	*cdef,
					*old_cdef;
	CustomObjectDef	 funcdef;
	CHFdwRelationInfo *fpinfo = context->scanrel->fdw_private;

	/*
	 * If the function call came from an implicit coercion, then just show the
	 * first argument.
	 */
	if (node->funcformat == COERCE_IMPLICIT_CAST)
	{
		deparseExpr((Expr *) linitial(node->args), context);
		return;
	}

	/*
	 * If the function call came from a cast, then show the first argument
	 * plus an explicit cast operation.
	 */
	if (node->funcformat == COERCE_EXPLICIT_CAST)
	{
		Oid			rettype = node->funcresulttype;
		int32		coercedTypmod;

		/* Get the typmod if this is a length-coercion function */
		(void) exprIsLengthCoercion((Node *) node, &coercedTypmod);

		appendStringInfoString(buf, "cast(");
		deparseExpr((Expr *) linitial(node->args), context);
		appendStringInfo(buf, ", 'Nullable(%s)')",
						 deparse_type_name(rettype, coercedTypmod));
		return;
	}

	/*
	 * Normal function: display as proname(args).
	 */
	cdef = appendFunctionName(node->funcid, context);
	if (cdef && cdef->cf_type == CF_DATE_TRUNC)
	{
		Const *arg = (Const *) linitial(node->args);
		char *trunctype = TextDatumGetCString(arg->constvalue);
		CSTRING_TOLOWER(trunctype);
		int cast_to_datetime64 = 0;

		if (strcmp(trunctype, "week") == 0)
		{
			appendStringInfoString(buf, "toMonday");
		}
		else if (strcmp(trunctype, "second") == 0)
		{
			cast_to_datetime64 = 1;
			appendStringInfoString(buf, "toStartOfSecond");
		}
		else if (strcmp(trunctype, "minute") == 0)
		{
			appendStringInfoString(buf, "toStartOfMinute");
		}
		else if (strcmp(trunctype, "hour") == 0)
		{
			appendStringInfoString(buf, "toStartOfHour");
		}
		else if (strcmp(trunctype, "day") == 0)
		{
			appendStringInfoString(buf, "toStartOfDay");
		}
		else if (strcmp(trunctype, "month") == 0)
		{
			appendStringInfoString(buf, "toStartOfMonth");
		}
		else if (strcmp(trunctype, "quarter") == 0)
		{
			appendStringInfoString(buf, "toStartOfQuarter");
		}
		else if (strcmp(trunctype, "year") == 0)
		{
			appendStringInfoString(buf, "toStartOfYear");
		}
		else
		{
			elog(ERROR, "date_trunc cannot be exported for: %s", trunctype);
		}

		pfree(trunctype);
		if (cast_to_datetime64)
		{
			appendStringInfoString(buf, "(toDateTime64(");
			deparseExpr(list_nth(node->args, 1), context);
			appendStringInfoString(buf, ", 1))");
		}
		else
		{
			appendStringInfoChar(buf, '(');
			deparseExpr(list_nth(node->args, 1), context);
			appendStringInfoChar(buf, ')');
		}
		return;
	}
	else if (cdef && cdef->cf_type == CF_DATE_PART)
	{
		Const *arg = (Const *) linitial(node->args);
		char *parttype = TextDatumGetCString(arg->constvalue);
		CSTRING_TOLOWER(parttype);

		if (strcmp(parttype, "day") == 0)
			appendStringInfoString(buf, "toDayOfMonth");
		else if (strcmp(parttype, "doy") == 0)
			appendStringInfoString(buf, "toDayOfYear");
		else if (strcmp(parttype, "dow") == 0)
			appendStringInfoString(buf, "toDayOfWeek");
		else if (strcmp(parttype, "year") == 0)
			appendStringInfoString(buf, "toYear");
		else if (strcmp(parttype, "month") == 0)
			appendStringInfoString(buf, "toMonth");
		else if (strcmp(parttype, "hour") == 0)
			appendStringInfoString(buf, "toHour");
		else if (strcmp(parttype, "minute") == 0)
			appendStringInfoString(buf, "toMinute");
		else if (strcmp(parttype, "second") == 0)
			appendStringInfoString(buf, "toSecond");
		else if (strcmp(parttype, "quarter") == 0)
			appendStringInfoString(buf, "toQuarter");
		else if (strcmp(parttype, "isoyear") == 0)
			appendStringInfoString(buf, "toISOYear");
		else if (strcmp(parttype, "week") == 0)
			appendStringInfoString(buf, "toISOWeek");
		else if (strcmp(parttype, "epoch") == 0)
			appendStringInfoString(buf, "toUnixTimestamp");
		else
			elog(ERROR, "date_part cannot be exported for: %s", parttype);

		pfree(parttype);
		appendStringInfoChar(buf, '(');
		deparseExpr(list_nth(node->args, 1), context);
		appendStringInfoChar(buf, ')');
		return;
	}
	else if (cdef && cdef->cf_type == CF_ISTORE_SEED)
	{
		if (!context->func)
		{
			/* not aggregation */
			appendStringInfoChar(buf, '(');
		}

		appendStringInfoString(buf, "range(toUInt16(");
		deparseExpr(list_nth(node->args, 1), context);
		appendStringInfoString(buf, ") + 1), arrayResize(emptyArrayInt64(),toUInt16(");
		deparseExpr(list_nth(node->args, 1), context);
		appendStringInfoString(buf, ") + 1, coalesce(");
		deparseExpr(list_nth(node->args, 2), context);
		appendStringInfoString(buf, ", 0)");

		if (context->func && context->func->cf_type == CF_ISTORE_SUM
			&& fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE)
			appendStringInfo(buf, " * %s", fpinfo->ch_table_sign_field);

		appendStringInfoChar(buf, ')');

		if (!context->func)
			appendStringInfoChar(buf, ')');

		return;
	}
	else if (cdef && cdef->cf_type == CF_ISTORE_ACCUMULATE)
	{
		Relids		relids = context->scanrel->relids;
		Var		   *var = linitial(node->args);
		bool		qualify_col = (bms_num_members(relids) > 1);
		char	   *colname = NULL;
		RangeTblEntry *rte;
		CustomColumnInfo *cinfo;

		if (!IsA(linitial(node->args), Var))
			elog(ERROR, "clickhouse_fdw supports simple accumulate with column as first parameter");

		if (bms_is_member(var->varno, relids) && var->varlevelsup == 0)
			rte = planner_rt_fetch(var->varno, context->root);
		else
			elog(ERROR, "unidentified first parameter in accumulate");

		/* Get FDW specific options for this column */
		cinfo = chfdw_get_custom_column_info(rte->relid, var->varattno);
		if (!cinfo)
			elog(ERROR, "unidentified first parameter in accumulate");

		if (!context->func)
		{
			/* not aggregation */
			appendStringInfoChar(buf, '(');
		}

		colname = cinfo->colname;
		if (cinfo->coltype == CF_ISTORE_ARR)
		{
			char *max_key_alias = get_alias_name();
			char *colkey = psprintf("%s_keys", colname);
			char *colval = psprintf("%s_values", colname);

			if (list_length(node->args) == 1)
				elog(ERROR, "clickhouse_fdw supports accumulate only with max_key parameter");

			/* first block
			 *	if(a_keys[1] <= max_key, arrayMap(x -> a_keys[1] + x - 1,
			 *		arrayEnumerate(arrayResize(emptyArrayInt32(), (max_key) - a_keys[1] + 1))), []),
			 */
			appendStringInfoString(buf, "if(");
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);

			appendStringInfoString(buf, colkey);
			appendStringInfoString(buf, "[1] <= (coalesce(");
			deparseExpr((Expr *) list_nth(node->args, 1), context);
			appendStringInfo(buf, ", 0) as %s), arrayMap(x -> ", max_key_alias);
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, colkey);
			appendStringInfoString(buf, "[1] + x - 1, arrayEnumerate(arrayResize(emptyArrayInt32(), (");
			appendStringInfo(buf, "%s) - ", max_key_alias);
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, colkey);
			appendStringInfoString(buf, "[1] + 1))), []),");

			/* second block:
			 *	if(a_keys[1] <= max_key, arrayCumSum(arrayMap(x -> sign * (a_values[indexOf(a_keys, a_keys[1] + x - 1)]),
			 *		arrayEnumerate(arrayResize(emptyArrayInt32(), (max_key) -a_keys[1] + 1)))), [])
			 */
			appendStringInfoString(buf, "if(");
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, colkey);
			appendStringInfo(buf, "[1] <= (%s", max_key_alias);
			appendStringInfoString(buf, "), arrayCumSum(arrayMap(x ->");

			if (context->func && context->func->cf_type == CF_ISTORE_SUM
				&& fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE)
				appendStringInfo(buf, "%s * ", fpinfo->ch_table_sign_field);

			appendStringInfoChar(buf, '(');
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, colval);
			appendStringInfoString(buf, "[indexOf(");
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, colkey);
			appendStringInfoChar(buf, ',');
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, colkey);
			appendStringInfoString(buf, "[1] + x - 1)]), arrayEnumerate(arrayResize(emptyArrayInt32(), (");
			appendStringInfo(buf, "%s) - ", max_key_alias);
			appendStringInfoString(buf, colkey);
			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);
			appendStringInfoString(buf, "[1] + 1)))), [])");

			pfree(colkey);
			pfree(colval);

			return;
		}
		else if (cinfo->coltype == CF_ISTORE_COL)
		{
			/* (mapFill((finalizeAggregation(col) as t1).1, t1.2, <max_key>) as t2).1,
			 * arrayCumSum(t2.2)
			 *
			 * simple:
			 * (mapFill((col as t1).1, t1.2, <max_key>) as t2).1,
			 * arrayCumSum(t2.2) */

			char *alias1 = get_alias_name();
			char *alias2 = get_alias_name();

			appendStringInfoString(buf, "(mapFill((");
			if (cinfo->is_AggregateFunction == CF_AGGR_FUNC)
				appendStringInfoString(buf, "finalizeAggregation(");
			else
				appendStringInfoChar(buf, '(');

			if (qualify_col)
				ADD_REL_QUALIFIER(buf, var->varno);

			appendStringInfoString(buf, colname);
			appendStringInfo(buf, ") as %s).1, %s.2", alias1, alias1);

			if (list_length(node->args) > 1)
			{
				/* max key, for now just assume it keys are always int32 */
				appendStringInfoString(buf, ", toInt32(assumeNotNull(");
				deparseExpr((Expr *) list_nth(node->args, 1), context);
				appendStringInfoString(buf, "))");
			}
			appendStringInfo(buf, ") as %s).1, arrayCumSum(%s.2)", alias2, alias2);

			return;
		}
		else
			elog(ERROR, "accumulate for this kind of istore not implemented");

		if (!context->func)
			appendStringInfoChar(buf, ')');
	}

	appendStringInfoChar(buf, '(');
	if (cdef && cdef->cf_type == CF_AJTIME_DAY_DIFF)
	{
		appendStringInfoString(buf, "(cast(");
		deparseExpr((Expr *) list_nth(node->args, 1), context);
		appendStringInfoString(buf, ", 'DateTime') - ");
		deparseExpr((Expr *) linitial(node->args), context);
		appendStringInfoString(buf, ") / 86400)");
		return;
	}
	else if (cdef && (cdef->cf_type == CF_TIMEZONE || cdef->cf_type == CF_MATCH))
	{
		deparseExpr((Expr *) list_nth(node->args, 1), context);
		appendStringInfoString(buf, ", ");
		deparseExpr((Expr *) linitial(node->args), context);
		appendStringInfoChar(buf, ')');
		return;
	}

	old_cdef = context->func;

	/* sup_up requires only values part of array */
	if (cdef && cdef->cf_type == CF_ISTORE_SUM_UP)
	{
		/* sum_up(daily_time_spent_cohort, 12)
			=>
			arraySum(arrayFilter((v, k) -> k <= 12,
			daily_time_spent_cohort_values,
			daily_time_spent_cohort_keys))

			sum_up(daily_time_spent_cohort)
			=>
			arraySum(daily_time_spent_cohort_values)

			!!!! or in case of AggregateFunction:
			sum_up(daily_time_spent_cohort, 12)
			=>
			arraySum(arrayFilter((v, k) -> k <= 12,
			(finalizeAggregation(daily_time_spent_cohort) as _tmp).2,
			_tmp.1))

			sum_up(daily_time_spent_cohort)
			=>
			arraySum(finalizeAggregation(daily_time_spent_cohort).2)
		*/
		funcdef = *cdef;
		funcdef.cf_context = node->args;
		context->func = &funcdef;

		if (list_length(node->args) == 2)
		{
			appendStringInfoString(buf, "arrayFilter((v, k) -> k <= ");
			appendStringInfoChar(buf, '(');
			deparseExpr((Expr *) list_nth(node->args, 1), context);
			appendStringInfoChar(buf, ')');
			appendStringInfoChar(buf, ',');
			deparseExpr((Expr *) linitial(node->args), context);
			appendStringInfoChar(buf, ')');
		}
		else
			deparseExpr((Expr *) linitial(node->args), context);

		goto end;
	}

	/* ... and all the arguments */
	first = true;
	foreach (arg, node->args)
	{
		if (!first)
			appendStringInfoString(buf, ", ");

		deparseExpr((Expr *) lfirst(arg), context);
		first = false;
	}

end:
	context->func = old_cdef;
	appendStringInfoChar(buf, ')');
}

static void
deparseIntervalOp(Node *first, Node *second, deparse_expr_cxt *context, bool plus)
{
	StringInfo	buf = context->buf;
	Const	   *constval;
	Interval   *span;
	char		ibuf[MAXINT8LEN + 1];

	if (!IsA(second, Const))
	{
		bool old_op = context->interval_op;
		/* first argument */
		deparseExpr((Expr *) first, context);

		if (plus)
			appendStringInfoString(buf, " + ");
		else
			appendStringInfoString(buf, " - ");

		appendStringInfoString(buf, "INTERVAL ");

		/* second */
		context->interval_op = true;
		deparseExpr((Expr *) second, context);
		context->interval_op = old_op;
		return;
	}

	constval = (Const *) second;
	span = DatumGetIntervalP(constval->constvalue);

	/* top function is always addSeconds */
	appendStringInfoString(buf, "addSeconds(");

	if (span->day)
		appendStringInfoString(buf, "addDays(");

	if (span->month)
		appendStringInfoString(buf, "addMonths(");

	/* first argument here */
	deparseExpr((Expr *) first, context);

	if (span->month)
	{
		/* addMonths arg */
		appendStringInfoChar(buf, ',');
		snprintf(ibuf, sizeof(ibuf), "%d", span->month);
		if (!plus)
		{
			appendStringInfoString(buf, "-(");
			appendStringInfoString(buf, ibuf);
			appendStringInfoChar(buf, ')');
		}
		else
			appendStringInfoString(buf, ibuf);
		appendStringInfoChar(buf, ')');
	}

	if (span->day)
	{
		/* addDays arg */
		appendStringInfoChar(buf, ',');
		snprintf(ibuf, sizeof(ibuf), "%d", span->day);
		if (!plus)
		{
			appendStringInfoString(buf, "-(");
			appendStringInfoString(buf, ibuf);
			appendStringInfoChar(buf, ')');
		}
		else
			appendStringInfoString(buf, ibuf);
		appendStringInfoChar(buf, ')');
	}

	/* addSeconds arg */
	appendStringInfoChar(buf, ',');
	pg_lltoa((int64)(span->time / 1000000), ibuf);
	if (!plus)
	{
		appendStringInfoString(buf, "-(");
		appendStringInfoString(buf, ibuf);
		appendStringInfoChar(buf, ')');
	}
	else
		appendStringInfoString(buf, ibuf);
	appendStringInfoChar(buf, ')');
}

static Oid
findFunction(Oid typoid, char *name)
{
	int			i;
	Oid			result = InvalidOid;
	HeapTuple	proctup;
	Form_pg_proc procform;
	CatCList   *catlist;
	catlist = SearchSysCacheList1(PROCNAMEARGSNSP,
			CStringGetDatum(name));

	if (catlist->n_members == 0)
		elog(ERROR, "clickhouse_fdw requires istore extension to parse istore values");

	for (i = 0; i < catlist->n_members; i++)
	{
		proctup = &catlist->members[i]->tuple;
		procform = (Form_pg_proc) GETSTRUCT(proctup);
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

/*
 * Deparse given operator expression.   To avoid problems around
 * priority of operations, we always parenthesize the arguments.
 */
static void
deparseOpExpr(OpExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	tuple;
	Form_pg_operator form;
	char		oprkind;
	ListCell   *arg;
	CustomObjectDef	*cdef;

	/* Retrieve information about the operator from system catalog. */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);

	form = (Form_pg_operator) GETSTRUCT(tuple);
	oprkind = form->oprkind;

	/* Sanity check. */
	Assert((oprkind == 'r' && list_length(node->args) == 1) ||
		   (oprkind == 'l' && list_length(node->args) == 1) ||
		   (oprkind == 'b' && list_length(node->args) == 2));

	cdef = chfdw_check_for_custom_operator(node->opno, form);
	if (cdef)
	{
		switch (cdef->cf_type)
		{
			case CF_AJTIME_OPERATOR:
			{
				/* intervals with ajtime */
				CustomObjectDef	*fdef = chfdw_check_for_custom_function(form->oprcode);
				if (fdef && fdef->cf_type == CF_AJTIME_PL_INTERVAL)
				{
					deparseIntervalOp(linitial(node->args),
						list_nth(node->args, 1), context, true);
					goto cleanup;
				}
				else if (fdef && fdef->cf_type == CF_AJTIME_MI_INTERVAL)
				{
					deparseIntervalOp(linitial(node->args),
						list_nth(node->args, 1), context, false);
					goto cleanup;
				}
			}
			break;
			case CF_TIMESTAMPTZ_PL_INTERVAL:
			{
				deparseIntervalOp(linitial(node->args),
					list_nth(node->args, 1), context, true);
				goto cleanup;
			}
			break;
			case CF_HSTORE_FETCHVAL:
			{
				Expr *arg1 = linitial(node->args);
				Expr *arg2 = list_nth(node->args, 1);

				if (IsA(arg1, Const))
				{
					Const	   *constval = (Const *) arg1;
					Oid			akeys = findFunction(constval->consttype, "akeys");
					Oid			avalues = findFunction(constval->consttype, "avals");

					/* vals[nullif(indexOf(keys,toString(arg1)), 0)] */
					appendStringInfoChar(buf, '(');
					deparseArray(OidFunctionCall1(avalues, constval->constvalue), context);
					appendStringInfoString(buf, "[nullif(indexOf(");
					deparseArray(OidFunctionCall1(akeys, constval->constvalue), context);
					appendStringInfoChar(buf, ',');
					deparseExpr(arg2, context);
					appendStringInfoString(buf, "), 0)])");
				}
				else
					elog(ERROR, "clickhouse_fdw supports hstore fetchval "
							"only for scalars");

				goto cleanup;
			}
			break;
			case CF_ISTORE_FETCHVAL:
			{
				Node *arg = linitial(node->args);

				if (IsA(arg, Var))
				{
					CustomObjectDef	*temp;

					temp = context->func;
					context->func = cdef;

					/* values[nullif(indexOf(ids, ...  in deparseColumnRef */
					deparseExpr((Expr *) arg, context);
					deparseExpr((Expr *) list_nth(node->args, 1), context);
					/* ... 0)] */
					appendStringInfoString(buf, "), 0)]");

					context->func = temp;
				}
				else if (IsA(arg, Const))
				{
					Const	   *constval = (Const *) arg;
					Oid			akeys = findFunction(constval->consttype, "akeys");
					Oid			avalues = findFunction(constval->consttype, "avals");

					/* ([val1, val2][nullif(indexOf([key1, key2], arg), 0]) */
					appendStringInfoChar(buf, '(');
					deparseArray(OidFunctionCall1(avalues, constval->constvalue), context);
					appendStringInfoString(buf, "[nullif(indexOf(");
					deparseArray(OidFunctionCall1(akeys, constval->constvalue), context);
					appendStringInfoString(buf, ", ");
					deparseExpr((Expr *) list_nth(node->args, 1), context);
					appendStringInfoString(buf, "), 0)])");
				}
				else
					elog(ERROR, "clickhouse_fdw supports fetchval only for columns and consts");

				goto cleanup;
			}
			break;
			default: /* keep compiler quiet */ ;
		}
	}

	if ((node->opresulttype == INT2OID ||
		 node->opresulttype == INT4OID ||
		 node->opresulttype == INT8OID) &&
			strcmp(NameStr(form->oprname), "/") == 0)
	{
		char *s = ch_format_type_extended(node->opresulttype, 0, 0);
		appendStringInfo(buf, "to%s", s);
	}

	/* Always parenthesize the expression. */
	appendStringInfoChar(buf, '(');

	/* Deparse left operand. */
	if (oprkind == 'r' || oprkind == 'b')
	{
		arg = list_head(node->args);

		/*
		 * Check for TestCaseExpr, in statements like CASE expr WHEN <val>.
		 * Basically they would look like, OpExpr->(TestCaseExpr, Const).
		 * We should just skip first arg and deparse second.
		 */
		if (IsA(lfirst(arg), CaseTestExpr))
		{
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
	if (context->interval_op && strcmp(NameStr(form->oprname), "||") == 0)
	{
		Const *right = lfirst(list_tail(node->args));
		if (IsA(right, Const) && right->consttype == TEXTOID)
		{
			char *s = TextDatumGetCString(right->constvalue);
			if (strstr(s, "day") != NULL)
				appendStringInfoString(buf, ") day");
			else if (strstr(s, "year") != NULL)
				appendStringInfoString(buf, ") year");
			else if (strstr(s, "month") != NULL)
				appendStringInfoString(buf, ") month");
			else
				elog(ERROR, "unsupported type of interval");
			pfree(s);

			goto cleanup;
		}
	}

	/* Deparse operator name. */
	deparseOperatorName(buf, form);

	/* Deparse right operand. */
	if (oprkind == 'l' || oprkind == 'b')
	{
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
deparseOperatorName(StringInfo buf, Form_pg_operator opform)
{
	char	   *opname;

	opname = NameStr(opform->oprname);

	if (strcmp(opname, "~~") == 0)
		appendStringInfoString(buf, "LIKE");
	else if (strcmp(opname, "~~*") == 0)
		appendStringInfoString(buf, "LIKE");
	else if (strcmp(opname, "!~~") == 0)
		appendStringInfoString(buf, "NOT LIKE");
	else if (strcmp(opname, "!~~*") == 0)
		appendStringInfoString(buf, "NOT LIKE");
	else
		appendStringInfoString(buf, opname);
}

/*
 * Deparse IS DISTINCT FROM.
 */
static void
deparseDistinctExpr(DistinctExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	Assert(list_length(node->args) == 2);

	appendStringInfoChar(buf, '(');
	deparseExpr(linitial(node->args), context);
	appendStringInfoString(buf, " IS DISTINCT FROM ");
	deparseExpr(lsecond(node->args), context);
	appendStringInfoChar(buf, ')');
}

static void
deparseNullIfExpr(NullIfExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	Assert(list_length(node->args) == 2);

	appendStringInfoString(buf, "NULLIF(");
	deparseExpr(linitial(node->args), context);
	appendStringInfoChar(buf, ',');
	deparseExpr(lsecond(node->args), context);
	appendStringInfoChar(buf, ')');
}

static void deparseAsIn(ScalarArrayOpExpr *node, deparse_expr_cxt *context, int optype)
{
	StringInfo	buf = context->buf;
	Expr *arg1 = linitial(node->args);
	Expr *arg2 = lsecond(node->args);

	deparseExpr(arg1, context);
	if (optype == 1)
		appendStringInfoString(buf, " IN ");
	else
		appendStringInfoString(buf, " NOT IN ");

	Assert(IsA(arg2, Const));
	context->array_as_tuple = true;
	deparseExpr(arg2, context);
	context->array_as_tuple = false;
}

/*
 * Deparse given ScalarArrayOpExpr expression.  To avoid problems
 * around priority of operations, we always parenthesize the arguments.
 */
static void
deparseScalarArrayOpExpr(ScalarArrayOpExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	Expr	   *arg1;
	Expr	   *arg2;

	/* Retrieve information about the operator from system catalog. */
	int			optype = chfdw_is_equal_op(node->opno);

	if (optype == 0)
		elog(ERROR, "clickhouse_fdw supports only equal (not equal) operations on ANY/ALL functions");

	/* Sanity check. */
	Assert(list_length(node->args) == 2);

	appendStringInfoChar(buf, '(');
	if (node->useOr)
	{
		arg2 = lsecond(node->args);

		/* very narrow case for = ANY(ARRAY) */
		if (optype == 1 && IsA(arg2, Const))
			deparseAsIn(node, context, optype);
		else
		{
			if (optype == 1)
				appendStringInfoString(buf, "has(");
			else
				appendStringInfoString(buf, "not has(");

			/* Deparse right operand. */
			deparseExpr(arg2, context);
			appendStringInfoChar(buf, ',');

			/* Deparse left operand. */
			arg1 = linitial(node->args);
			deparseExpr(arg1, context);

			/* Close function call */
			appendStringInfoChar(buf, ')');
		}
	}
	else
	{
		arg2 = lsecond(node->args);

		/* very narrow case for <> ALL(ARRAY) */
		if (optype == 2 && IsA(arg2, Const))
			deparseAsIn(node, context, optype);
		else
		{
			appendStringInfoString(buf, "countEqual(");

			/* Deparse right operand. */
			arg2 = lsecond(node->args);
			deparseExpr(arg2, context);
			appendStringInfoChar(buf, ',');

			/* Deparse left operand. */
			arg1 = linitial(node->args);
			deparseExpr(arg1, context);

			/* Close function call */
			if (optype == 1)
			{
				appendStringInfoString(buf, ") = length(");

				/* Deparse right operand again */
				deparseExpr(arg2, context);
				appendStringInfoChar(buf, ')');
			} else {
				appendStringInfoString(buf, ") = 0");
			}
		}
	}

	appendStringInfoChar(buf, ')');
}

/*
 * Deparse a RelabelType (binary-compatible cast) node.
 */
static void
deparseRelabelType(RelabelType *node, deparse_expr_cxt *context)
{
	deparseExpr(node->arg, context);
}

/*
 * Deparse a BoolExpr node.
 */
static void
deparseBoolExpr(BoolExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	const char *op = NULL;		/* keep compiler quiet */
	bool		first;
	ListCell   *lc;

	switch (node->boolop)
	{
	case AND_EXPR:
		op = "AND";
		break;
	case OR_EXPR:
		op = "OR";
		break;
	case NOT_EXPR:
		appendStringInfoString(buf, "(NOT ");
		deparseExpr(linitial(node->args), context);
		appendStringInfoChar(buf, ')');
		return;
	}

	appendStringInfoChar(buf, '(');
	first = true;
	foreach (lc, node->args)
	{
		if (!first)
		{
			appendStringInfo(buf, " %s ", op);
		}
		deparseExpr((Expr *) lfirst(lc), context);
		first = false;
	}
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse IS [NOT] NULL expression.
 */
static void
deparseNullTest(NullTest *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	appendStringInfoChar(buf, '(');
	deparseExpr(node->arg, context);

	/*
	 * For scalar inputs, we prefer to print as IS [NOT] NULL, which is
	 * shorter and traditional.  If it's a rowtype input but we're applying a
	 * scalar test, must print IS [NOT] DISTINCT FROM NULL to be semantically
	 * correct.
	 */
	if (node->argisrow || !type_is_rowtype(exprType((Node *) node->arg)))
	{
		if (node->nulltesttype == IS_NULL)
			appendStringInfoString(buf, " IS NULL)");
		else
			appendStringInfoString(buf, " IS NOT NULL)");
	}
	else
	{
		if (node->nulltesttype == IS_NULL)
			appendStringInfoString(buf, " IS NOT DISTINCT FROM NULL)");
		else
			appendStringInfoString(buf, " IS DISTINCT FROM NULL)");
	}
}

/*
 * Deparse ARRAY[...] construct.
 */
static void
deparseArrayExpr(ArrayExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	if (node->elements == NIL)
		appendStringInfoString(buf, "CAST(");

	appendStringInfoString(buf, "[");
	deparseArrayList(node, context);
	appendStringInfoChar(buf, ']');

	/* If the array is empty, we need an explicit cast to the array type. */
	if (node->elements == NIL)
		appendStringInfo(buf, ", '%s')",
			deparse_type_name(node->array_typeid, -1));
}

/*
 * Deparse an array to a list, as in converting a variadic function call's
 * array to the list of individual values.
*/
static void
deparseArrayList(ArrayExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	bool		first = true;
	ListCell   *lc;

	foreach(lc, node->elements)
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		deparseExpr(lfirst(lc), context);
		first = false;
	}
}

/*
 * Deparse an Aggref node.
 */
static void
deparseAggref(Aggref *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	CustomObjectDef	*cdef;
	CHFdwRelationInfo *fpinfo = context->scanrel->fdw_private;
	bool	aggfilter = false;
	bool	sign_count_filter = false;
	uint8	brcount = 1;
	bool	use_variadic;
	bool 	omit_star;

	/* Only basic, non-split aggregation accepted. */
	Assert(node->aggsplit == AGGSPLIT_SIMPLE);

	/* Check if need to print expand VARIADIC (cf. ruleutils.c) */
	use_variadic = node->aggvariadic;

	/* Find aggregate name from aggfnoid which is a pg_proc entry */
	cdef = context->func;
	context->func = appendFunctionName(node->aggfnoid, context);

	/* Omit * for COUNT(*) but not COUNT(DISTINCT *)
	 * https://github.com/ClickHouse/clickhouse_fdw/issues/25
	 * To be fixed in ClickHouse 25.11, so can be omitted once relased.
	 * https://github.com/ClickHouse/ClickHouse/pull/89373
	 *
	 * XXX Once ClickHouse has made a release fixing this issue, consider
	 * adding the Client::ServerInfo struct returned from
	 * Client::GetServerInfo() to deparse_expr_cxt so we can allow * to be
	 * passed through for the fixed version.
	 */
	omit_star = node->aggstar && node->aggfilter && node->aggdistinct == NIL && strcmp(buf->data, "count");

	/* 'If' part */
	if (context->func && context->func->cf_type == CF_SIGN_COUNT && !node->aggstar)
		sign_count_filter = true;

	/* We use this field as indicator of aggregate functions */
	if (node->location == -2)
		appendStringInfoString(buf, "Merge");

	if (node->aggfilter || sign_count_filter)
	{
		aggfilter = true;
		appendStringInfoString(buf, "If");
	}

	appendStringInfoChar(buf, '(');

	/* Add DISTINCT */
	appendStringInfoString(buf, (node->aggdistinct != NIL) ? "DISTINCT " : "");

	/* aggstar can be set only in zero-argument aggregates */
	if (node->aggstar)
	{
		if (context->func && context->func->cf_type == CF_SIGN_COUNT)
		{
			Assert(fpinfo && fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE);
			appendStringInfoString(buf, fpinfo->ch_table_sign_field);
		}
		// Omit * for COUNT(*) but not COUNT(DISTINCT *).
		else if (!omit_star)
			appendStringInfoChar(buf, '*');
	}
	else
	{
		ListCell   *arg;
		bool		first = true;
		bool		signMultiply = (context->func &&
						(context->func->cf_type == CF_SIGN_AVG ||
						 context->func->cf_type == CF_SIGN_SUM));

		/* Add all the arguments */
		if (sign_count_filter)
			/* in case if COUNT(col) we should get countIf(sign, col is not null) */
			appendStringInfoString(buf, fpinfo->ch_table_sign_field);
		else
		{
			/* default arguments output */
			foreach (arg, node->args)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(arg);
				Node	   *n = (Node *) tle->expr;

				if (tle->resjunk)
					continue;

				if (!first)
					appendStringInfoString(buf, ", ");

				first = false;

				if (use_variadic && lnext(node->args, arg) == NULL)
				{
					// Convert variadic array to list of arguments.
					Assert(nodeTag(node) == T_ArrayExpr);
					deparseArrayList((ArrayExpr *) n, context);
				}
				else
				{
					deparseExpr((Expr *) n, context);
				}
			}

			if (signMultiply)
			{
				Assert(fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE);
				appendStringInfoString(buf, " * ");
				appendStringInfoString(buf, fpinfo->ch_table_sign_field);
			}
		}
	}

	/* Add 'If' part condition */
	if (aggfilter)
	{
		// No argument output for COUNT(*).
		if (!omit_star)
			appendStringInfoChar(buf, ',');

		if (node->aggfilter)
		{
			appendStringInfoString(buf, "((");
			deparseExpr((Expr *) node->aggfilter, context);
			appendStringInfoString(buf, ") > 0)");
		}

		if (sign_count_filter)
		{
			if (node->aggfilter)
				appendStringInfoString(buf, " AND ");

			appendStringInfoChar(buf, '(');
			deparseExpr((Expr *) ((TargetEntry *) linitial(node->args))->expr, context);
			appendStringInfoString(buf, ") IS NOT NULL");
		}
	}

	while (brcount--)
		appendStringInfoChar(buf, ')');

	/* AVG stuff */
	if (context->func && context->func->cf_type == CF_SIGN_AVG)
	{
		appendStringInfoString(buf, " / sumIf(");
		appendStringInfoString(buf, fpinfo->ch_table_sign_field);
		appendStringInfoChar(buf, ',');
		if (node->aggfilter)
		{
			deparseExpr((Expr *) node->aggfilter, context);
			appendStringInfoString(buf, " AND ");
		}
		deparseExpr((Expr *) ((TargetEntry *) linitial(node->args))->expr, context);
		appendStringInfoString(buf, " IS NOT NULL");
		appendStringInfoChar(buf, ')');
	}

	/* original */
	context->func = cdef;
}

static void
deparseCaseExpr(CaseExpr *node, deparse_expr_cxt *context)
{
#define DEPARSE_WRAPPED(node) \
do { \
	bool isnull = (IsA(node, Const) && ((Const *) (node))->constisnull); \
	if (conv && !isnull) \
		appendStringInfoString(buf, conv); \
	deparseExpr(node, context); \
	if (conv && !isnull) \
		appendStringInfoChar(buf, ')'); \
} while (0)

	StringInfo	buf = context->buf;
	ListCell   *lc;
	char	   *conv = NULL;

	if (node->casetype == INT2OID ||
		node->casetype == INT4OID ||
		node->casetype == INT8OID)
	{
		conv = ch_format_type_extended(node->casetype, 0, 0);
		conv = psprintf("to%s(", conv);
	}

	appendStringInfoString(buf, "CASE");
	if (node->arg)
	{
		appendStringInfoChar(buf, ' ');
		deparseExpr(node->arg, context);
	}

	foreach(lc, node->args)
	{
		CaseWhen	*arg = lfirst(lc);

		Assert(IsA(arg, CaseWhen));
		appendStringInfoString(buf, " WHEN ");
		deparseExpr(arg->expr, context);

		/* in simple cases like WHEN val THEN we should extend the condition
		 * for WHEN val = 1 since there is no bool type in ClickHouse */
		if (IsA(arg->expr, Var))
			appendStringInfoString(buf, " = 1");

		appendStringInfoString(buf, " THEN ");
		DEPARSE_WRAPPED(arg->result);
	}

	if (node->defresult)
	{
		appendStringInfoString(buf, " ELSE ");
		DEPARSE_WRAPPED(node->defresult);
	}

	if (conv)
		pfree(conv);
	appendStringInfoString(buf, " END");
}

static void
deparseCaseWhen(CaseWhen *node, deparse_expr_cxt *context)
{
	// XXX Needs implementation.
	// StringInfo	buf = context->buf;
	// ListCell   *lc;
}

static void
deparseRowExpr(RowExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lc;

	bool		first = true;

	appendStringInfoChar(buf, '(');
	foreach(lc, node->args)
	{
		if (!first)
			appendStringInfoChar(buf, ',');

		first = false;
		if (IsA(lfirst(lc), Const))
			deparseConst((Const *) lfirst(lc), context, 1);
		else
			deparseExpr(lfirst(lc), context);
	}
	appendStringInfoChar(buf, ')');
}

static void
deparseCoerceViaIO(CoerceViaIO *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;

	if (node->resulttype == INTERVALOID)
		deparseExpr(node->arg, context);
	else
	{
		appendStringInfoString(buf, "CAST(");
		deparseExpr(node->arg, context);
		appendStringInfoString(buf, " AS ");

		if (node->resultcollid == 1)
			appendStringInfoString(buf, "Nullable(");

		appendStringInfoString(buf, deparse_type_name(node->resulttype, 0));

		if (node->resultcollid == 1)
			appendStringInfoChar(buf, ')');

		appendStringInfoChar(buf, ')');
	}
}

static void
deparseCoalesceExpr(CoalesceExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lc;
	bool		first;

	appendStringInfoString(buf, "COALESCE(");

	first = true;
	foreach (lc, node->args)
	{
		Expr *arg = lfirst(lc);

		if (!first)
			appendStringInfoString(buf, ", ");

		/* first arg should be nullable */
		if (IsA(arg, CoerceViaIO))
		{
			CoerceViaIO *vio = (CoerceViaIO *) arg;

			if (arg != llast(node->args))
				vio->resultcollid = 1;
			else
				vio->resultcollid = InvalidOid;
		}

		first = false;
		deparseExpr(arg, context);
	}
	appendStringInfoChar(buf, ')');
}

static void
deparseMinMaxExpr(MinMaxExpr *node, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	ListCell   *lc;
	bool		first;

	if (node->op == IS_GREATEST)
		appendStringInfoString(buf, "greatest");
	else
		appendStringInfoString(buf, "least");

	appendStringInfoChar(buf, '(');
	first = true;
	foreach (lc, node->args)
	{
		if (!first)
			appendStringInfoString(buf, ", ");

		first = false;

		deparseExpr(lfirst(lc), context);
	}
	appendStringInfoChar(buf, ')');
}

/*
 * Deparse GROUP BY clause.
 */
static void
appendGroupByClause(List *tlist, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	Query	   *query = context->root->parse;
	ListCell   *lc;
	bool		first = true;

	/* Nothing to be done, if there's no GROUP BY clause in the query. */
	if (!query->groupClause)
		return;

	appendStringInfoString(buf, " GROUP BY ");

	/*
	 * Queries with grouping sets are not pushed down, so we don't expect
	 * grouping sets here.
	 */
	Assert(!query->groupingSets);

	foreach (lc, query->groupClause)
	{
		SortGroupClause *grp = (SortGroupClause *) lfirst(lc);

		if (!first)
			appendStringInfoString(buf, ", ");

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
appendOrderByClause(List *pathkeys, bool has_final_sort,
					deparse_expr_cxt *context)
{
	ListCell   *lcell;
	char	   *delim = " ";
	RelOptInfo *baserel = context->scanrel;
	StringInfo	buf = context->buf;

	appendStringInfoString(buf, " ORDER BY");
	foreach(lcell, pathkeys)
	{
		PathKey    *pathkey = lfirst(lcell);
		Expr	   *em_expr;

		if (has_final_sort)
		{
			/*
			 * By construction, context->foreignrel is the input relation to
			 * the final sort.
			 */
			em_expr = chfdw_find_em_expr_for_input_target(context->root,
													pathkey->pk_eclass,
													context->foreignrel->reltarget);
		}
		else
			em_expr = chfdw_find_em_expr_for_rel(pathkey->pk_eclass, baserel);

		Assert(em_expr != NULL);

		appendStringInfoString(buf, delim);
		deparseExpr(em_expr, context);
#if PG_VERSION_NUM >= 180000
		if (pathkey->pk_cmptype == BTLessStrategyNumber)
#else
		if (pathkey->pk_strategy == BTLessStrategyNumber)
#endif
			appendStringInfoString(buf, " ASC");
		else
			appendStringInfoString(buf, " DESC");

		if (pathkey->pk_nulls_first)
			appendStringInfoString(buf, " NULLS FIRST");
		//else
		//	appendStringInfoString(buf, " NULLS LAST");

		delim = ", ";
	}
}

/*
 * Deparse LIMIT/OFFSET clause.
 */
static void
appendLimitClause(deparse_expr_cxt *context)
{
	PlannerInfo *root = context->root;
	StringInfo	buf = context->buf;

	if (root->parse->limitCount)
	{
		appendStringInfoString(buf, " LIMIT ");
		deparseExpr((Expr *) root->parse->limitCount, context);
	}
	if (root->parse->limitOffset)
	{
		appendStringInfoString(buf, " OFFSET ");
		deparseExpr((Expr *) root->parse->limitOffset, context);
	}
}

/*
 * appendFunctionName
 *		Deparses function name from given function oid.
 *		Returns was custom or not.
 */
static CustomObjectDef *
appendFunctionName(Oid funcid, deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	HeapTuple	proctup;
	Form_pg_proc procform;
	const char *proname;
	CustomObjectDef	*cdef;
	CHFdwRelationInfo *fpinfo = context->scanrel->fdw_private;

	cdef = chfdw_check_for_custom_function(funcid);
	if (cdef && cdef->custom_name[0] != '\0')
	{
		if (cdef->custom_name[0] != '\1')
			appendStringInfoString(buf, cdef->custom_name);
		return cdef;
	}

	proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(proctup))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	procform = (Form_pg_proc) GETSTRUCT(proctup);
	proname = NameStr(procform->proname);

	/* we have some additional conditions on aggregation functions */
	if (chfdw_is_builtin(funcid) && procform->prokind == PROKIND_AGGREGATE
			&& fpinfo->ch_table_engine == CH_COLLAPSING_MERGE_TREE)
	{
		cdef = palloc(sizeof(CustomObjectDef));
		cdef->cf_oid = funcid;

		if (strcmp(proname, "sum") == 0)
			cdef->cf_type = CF_SIGN_SUM;
		else if (strcmp(proname, "avg") == 0)
		{
			cdef->cf_type = CF_SIGN_AVG;;
			proname = "sum";
		}
		else if (strcmp(proname, "count") == 0)
		{
			cdef->cf_type = CF_SIGN_COUNT;
			proname = "sum";
		}
		else
		{
			pfree(cdef);
			cdef = NULL;
		}
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
static Node *
deparseSortGroupClause(Index ref, List *tlist, bool force_colno,
                       deparse_expr_cxt *context)
{
	StringInfo	buf = context->buf;
	TargetEntry *tle;
	Expr	   *expr;

	tle = get_sortgroupref_tle(ref, tlist);
	expr = tle->expr;

	if (expr && IsA(expr, Const))
	{
		/*
		 * Force a typecast here so that we don't emit something like "GROUP
		 * BY 2", which will be misconstrued as a column position rather than
		 * a constant.
		 */
		deparseConst((Const *) expr, context, 1);
	}
	else if (!expr || IsA(expr, Var))
	{
		deparseExpr(expr, context);
	}
	else
	{
		/* Always parenthesize the expression. */
		appendStringInfoChar(buf, '(');
		deparseExpr(expr, context);
		appendStringInfoChar(buf, ')');
	}

	return (Node *) expr;
}


/*
 * Returns true if given Var is deparsed as a subquery output column, in
 * which case, *relno and *colno are set to the IDs for the relation and
 * column alias to the Var provided by the subquery.
 */
static bool
is_subquery_var(Var *node, RelOptInfo *foreignrel, int *relno, int *colno)
{
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;
	RelOptInfo *outerrel = fpinfo->outerrel;
	RelOptInfo *innerrel = fpinfo->innerrel;

	/* Should only be called in these cases. */
	Assert(IS_SIMPLE_REL(foreignrel) || IS_JOIN_REL(foreignrel));

	/*
	 * If the given relation isn't a join relation, it doesn't have any lower
	 * subqueries, so the Var isn't a subquery output column.
	 */
	if (!IS_JOIN_REL(foreignrel))
		return false;

	/*
	 * If the Var doesn't belong to any lower subqueries, it isn't a subquery
	 * output column.
	 */
	if (!bms_is_member(node->varno, fpinfo->lower_subquery_rels))
		return false;

	if (bms_is_member(node->varno, outerrel->relids))
	{
		/*
		 * If outer relation is deparsed as a subquery, the Var is an output
		 * column of the subquery; get the IDs for the relation/column alias.
		 */
		if (fpinfo->make_outerrel_subquery)
		{
			get_relation_column_alias_ids(node, outerrel, relno, colno);
			return true;
		}

		/* Otherwise, recurse into the outer relation. */
		return is_subquery_var(node, outerrel, relno, colno);
	}
	else
	{
		Assert(bms_is_member(node->varno, innerrel->relids));

		/*
		 * If inner relation is deparsed as a subquery, the Var is an output
		 * column of the subquery; get the IDs for the relation/column alias.
		 */
		if (fpinfo->make_innerrel_subquery)
		{
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
get_relation_column_alias_ids(Var *node, RelOptInfo *foreignrel,
                              int *relno, int *colno)
{
	CHFdwRelationInfo *fpinfo = (CHFdwRelationInfo *) foreignrel->fdw_private;
	int			i;
	ListCell   *lc;

	/* Get the relation alias ID */
	*relno = fpinfo->relation_index;

	/* Get the column alias ID */
	i = 1;
	foreach (lc, foreignrel->reltarget->exprs)
	{
		if (equal(lfirst(lc), (Node *) node))
		{
			*colno = i;
			return;
		}
		i++;
	}

	/* Shouldn't get here */
	elog(ERROR, "unexpected expression in subquery output");
}
