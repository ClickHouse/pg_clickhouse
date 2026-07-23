/*
    A PostgreSQL extension for connecting to ClickHouse servers.
*/

#include <sys/time.h>

/* PostgreSQL includes. */
#include "postgres.h"

#include "catalog/pg_class_d.h"
#include "commands/defrem.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "utils/lsyscache.h"
#include "utils/palloc.h"
#include "utils/rel.h"
#include "utils/tuplestore.h"
#if PG_VERSION_NUM >= 140000
#include "optimizer/appendinfo.h"
#endif
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif
#if PG_VERSION_NUM >= 180000
#include "commands/explain_format.h"
#include "commands/explain_state.h"
#else
#include "commands/explain.h"
#endif

/* extension includes. */
#include "fdw.h"
#include "setop.h"
#include "utils/builtins.h"
#include "version.h"

/* Extension metadata for the server. */
#ifdef PG_MODULE_MAGIC_EXT
PG_MODULE_MAGIC_EXT(.name = "pg_clickhouse", .version = PGCH_VERSION);
#else
PG_MODULE_MAGIC;
#endif

/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST 100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST 0.01

/* If no remote estimates, assume a sort costs 20% extra */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.2

/* Approximate batch size in bytes for HTTP streaming (50 MB). */
#define DEFAULT_FETCH_SIZE (50 * 1000 * 1000)

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth(). For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex {
    /* SQL statement to execute remotely (as a String node) */
    FdwScanPrivateSelectSql,
    /* Integer list of attribute numbers retrieved by the SELECT */
    FdwScanPrivateRetrievedAttrs,
    /* Approximate batch size in bytes for HTTP streaming */
    FdwScanPrivateFetchSize,

    /*
     * String describing join i.e. names of relations being joined and types
     * of join, added when the scan is join
     */
    FdwScanPrivateRelations,
    /* Effective user OID for scans without a representative foreign-table RTE */
    FdwScanPrivateUserId
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a pg_clickhouse foreign table. We store:
 *
 * 0) INSERT statement text to be sent to the remote server
 * 1) Integer list of target attribute numbers for INSERT
 * 2) String table name.
 */
enum FdwModifyPrivateIndex {
    /* SQL statement to execute remotely (as a String node) */
    FdwModifyPrivateInsertSQL,
    /* Integer list of target attribute numbers for INSERT/UPDATE */
    FdwModifyPrivateTargetAttnums,
    /* Deparsed name of the result table */
    FdwModifyPrivateTableName,
};

/*
 * Execution state of a foreign scan using pg_clickhouse.
 */
typedef struct ChFdwScanState {
    Relation rel;             /* relcache entry for the foreign table. NULL
                               * for a foreign join scan. */
    TupleDesc tupdesc;        /* tuple descriptor of scan */
    AttInMetadata* attinmeta; /* attribute datatype conversion metadata */

    /* extracted fdw_private data */
    char* query;           /* text of SELECT command */
    List* retrieved_attrs; /* list of retrieved attribute numbers */

    /* for remote query execution */
    ch_scan_connection scan_conn; /* connection leased for this scan */
    UserMapping* user;            /* user mapping, for releasing scan_conn */
    int numParams;                /* number of parameters passed to query */
    Oid* param_oids;              /* output parameter type OIDs them */
    List* param_exprs;            /* executable expressions for param values */
    const char** param_values;    /* textual values of query parameters */
    ch_cursor* ch_cursor;         /* result of query from clickhouse */

    /* for storing result tuple */
    HeapTuple tuple; /* array of currently-retrieved tuples */

    /* working memory contexts */
    MemoryContext batch_cxt; /* context holding current batch of tuples */
    MemoryContext temp_cxt;  /* context for per-tuple temporary data */

    int32 fetch_size;  /* approximate batch size in bytes */
    bool is_streaming; /* true when using HTTP streaming */
} ChFdwScanState;

/*
 * Execution state of a foreign insert.
 */
typedef struct CHFdwModifyState {
    Relation rel;             /* relcache entry for the foreign table */
    AttInMetadata* attinmeta; /* attribute datatype conversion metadata */

    /* for remote query execution */
    ch_connection conn; /* connection for the scan */

    /* extracted fdw_private data */
    char* query; /* text of INSERT/UPDATE/DELETE command */
    void* state; /* internal state for a connection */

    /* working memory context */
    MemoryContext temp_cxt; /* context for per-tuple temporary data */
} CHFdwModifyState;

/*
 * This enum describes what's kept in the fdw_private list for a ForeignPath.
 * We store:
 *
 * 1) Boolean flag showing if the remote query has the final sort
 * 2) Boolean flag showing if the remote query has the LIMIT clause
 */
enum FdwPathPrivateIndex {
    /* has-final-sort flag (as an integer Value node) */
    FdwPathPrivateHasFinalSort,
    /* has-limit flag (as an integer Value node) */
    FdwPathPrivateHasLimit
};

/* Struct for extra information passed to estimate_path_cost_size() */
typedef struct {
    PathTarget* target;
    bool has_final_sort;
    bool has_limit;
    double limit_tuples;
    int64 count_est;
    int64 offset_est;
} ChFdwPathExtraData;

typedef struct ChFdwSetopValidationState {
    int32 fetch_size;
} ChFdwSetopValidationState;

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(clickhouse_fdw_handler);
PG_FUNCTION_INFO_V1(clickhouse_raw_query);
PG_FUNCTION_INFO_V1(clickhouse_query);
PG_FUNCTION_INFO_V1(clickhouse_op_push_fail);
PG_FUNCTION_INFO_V1(clickhouse_push_fail);
PG_FUNCTION_INFO_V1(clickhouse_noop);
PG_FUNCTION_INFO_V1(pgch_version);
PG_FUNCTION_INFO_V1(clickhouse_server_version);
static double time_used = 0;

/*
 * FDW callback routines
 */
static void
clickhouseGetForeignRelSize(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid);
static ForeignScan*
clickhouseGetForeignPlan(
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    Oid foreigntableid,
    ForeignPath* best_path,
    List* tlist,
    List* scan_clauses,
    Plan* outer_plan
);
static int
clickhouseAcquireSampleRowsFunc(
    Relation relation,
    int elevel,
    HeapTuple* rows,
    int targrows,
    double* totalrows,
    double* totaldeadrows
);
static void
clickhouseBeginForeignScan(ForeignScanState* node, int eflags);
static TupleTableSlot*
clickhouseIterateForeignScan(ForeignScanState* node);
static void
clickhouseReScanForeignScan(ForeignScanState* node);
static void
clickhouseEndForeignScan(ForeignScanState* node);
static List*
clickhousePlanForeignModify(
    PlannerInfo* root,
    ModifyTable* plan,
    Index resultRelation,
    int subplan_index
);
static void
clickhouseBeginForeignModify(
    ModifyTableState* mtstate,
    ResultRelInfo* resultRelInfo,
    List* fdw_private,
    int subplan_index,
    int eflags
);
static TupleTableSlot*
clickhouseExecForeignInsert(
    EState* estate,
    ResultRelInfo* resultRelInfo,
    TupleTableSlot* slot,
    TupleTableSlot* planSlot
);
static void
clickhouseBeginForeignInsert(ModifyTableState* mtstate, ResultRelInfo* resultRelInfo);
static void
clickhouseEndForeignInsert(EState* estate, ResultRelInfo* resultRelInfo);
static void
clickhouseExplainForeignScan(ForeignScanState* node, ExplainState* es);
static void
clickhouseGetForeignUpperPaths(
    PlannerInfo* root,
    UpperRelationKind stage,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    void* extra
);
static bool
clickhouseAnalyzeForeignTable(
    Relation relation,
    AcquireSampleRowsFunc* func,
    BlockNumber* totalpages
);
static bool
clickhouseRecheckForeignScan(ForeignScanState* node, TupleTableSlot* slot);

/*
 * Helper functions
 */
static void
estimate_path_cost_size(
    double* p_rows,
    int* p_width,
    Cost* p_startup_cost,
    Cost* p_total_cost,
    double coef
);
static CHFdwModifyState*
create_foreign_modify(
    EState* estate,
    RangeTblEntry* rte,
    ResultRelInfo* resultRelInfo,
    CmdType operation,
    Plan* subplan,
    char* query,
    List* target_attrs,
    char* table_name
);
static void
finish_foreign_modify(CHFdwModifyState* fmstate);
static void
prepare_query_params(
    PlanState* node,
    List* fdw_exprs,
    int numParams,
    Oid** param_oids,
    List** param_exprs,
    const char*** param_values
);
static void
process_query_params(
    ExprContext* econtext,
    Oid* param_oids,
    List* param_exprs,
    const char** param_values
);
static bool
foreign_join_ok(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    JoinType jointype,
    RelOptInfo* outerrel,
    RelOptInfo* innerrel,
    JoinPathExtraData* extra
);
static bool
foreign_grouping_ok(PlannerInfo* root, RelOptInfo* grouped_rel, Node* havingQual);
static List*
get_useful_pathkeys_for_relation(PlannerInfo* root, RelOptInfo* rel);
static void
add_paths_with_pathkeys_for_rel(PlannerInfo* root, RelOptInfo* rel, Path* epq_path);
static void
add_foreign_grouping_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* grouped_rel,
    GroupPathExtraData* extra
);
static void
add_foreign_distinct_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* distinct_rel
);
static void
add_foreign_window_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* window_rel
);
static bool
foreign_window_ok(PlannerInfo* root, RelOptInfo* window_rel);
static void
add_foreign_ordered_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* ordered_rel
);
static void
add_foreign_final_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* final_rel,
    void* fextra
);
static void
merge_fdw_options(
    CHFdwRelationInfo* fpinfo,
    const CHFdwRelationInfo* fpinfo_o,
    const CHFdwRelationInfo* fpinfo_i
);
static int
get_fetch_size_option(DefElem* def);
static DefElem*
ch_get_table_or_server_option(CHFdwRelationInfo* fpinfo, char* name);
static bool
setop_exec_param_walker(Node* node, void* context);
static bool
setop_restrictinfos_require_local_gate(List* conditions);
static bool
setop_relation_requires_local_gate(RelOptInfo* rel);
static bool
distinct_expr_is_safe(Node* expr);
static bool
distinct_target_is_safe(PathTarget* target);
static bool
setop_grouping_target_is_safe(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* grouped_rel,
    List* setop_tlist
);
static bool
setop_relation_uses_default_engine(RelOptInfo* rel);
static bool
setop_foreign_path_ok(const ForeignPath* path, void* arg);
static bool
is_flattened_union_all(PlannerInfo* root, RelOptInfo* input_rel);
static bool
is_flattened_union_all_grouping(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    GroupPathExtraData* extra
);
static AppendPath*
make_setop_append_path(PlannerInfo* root, AppendPath* append_path);
static bool
find_setop_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    AppendPath** append_path,
    bool* is_distinct,
    ChFdwSetopPathInfo* path_info,
    int32* fetch_size,
    Cost* startup_cost,
    Cost* total_cost
);
static void
add_foreign_setop_path(
    PlannerInfo* root,
    RelOptInfo* output_rel,
    AppendPath* append_path,
    bool is_distinct,
    const ChFdwSetopPathInfo* path_info,
    int32 fetch_size,
    Cost startup_cost,
    Cost total_cost
);
static void
add_foreign_setop_grouping_path(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* grouped_rel,
    AppendPath* append_path,
    const ChFdwSetopPathInfo* path_info,
    int32 fetch_size,
    Cost startup_cost,
    Cost total_cost
);

/* Make one query and close the connection */
Datum
clickhouse_raw_query(PG_FUNCTION_ARGS) {
    char* connstring = text_to_cstring(PG_GETARG_TEXT_P(1));
    ch_query query =
        new_query(text_to_cstring(PG_GETARG_TEXT_P(0)), 0, NULL, NULL, NULL);

    ch_connection_details* details = connstring_parse(connstring);
    ch_connection conn;
    text* res;

    if (strcmp(details->driver, "http") == 0) {
        conn = chfdw_http_connect(details);
    } else if (strcmp(details->driver, "binary") == 0) {
        conn = chfdw_binary_connect(details);
    } else {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
            errmsg(
                "pg_clickhouse: invalid ClickHouse connection driver \"%s\"",
                details->driver
            )
        );
    }

    PG_TRY();
    {
        ch_cursor* cursor = conn.methods->simple_query(conn.conn, &query);

        res = conn.is_binary ? chfdw_binary_fetch_raw_data(cursor)
                             : chfdw_http_fetch_raw_data(cursor);
        MemoryContextDelete(cursor->memcxt);
    }
    PG_CATCH();
    {
        conn.methods->disconnect(conn.conn);
        PG_RE_THROW();
    }
    PG_END_TRY();

    conn.methods->disconnect(conn.conn);

    if (res) {
        PG_RETURN_TEXT_P(res);
    }

    PG_RETURN_NULL();
}

/*
 * Report the ClickHouse server version for a foreign server as
 * "major.minor.patch", connecting if necessary.
 */
Datum
clickhouse_server_version(PG_FUNCTION_ARGS) {
    char* servername      = text_to_cstring(PG_GETARG_TEXT_P(0));
    ForeignServer* server = GetForeignServerByName(servername, false);
    UserMapping* user     = GetUserMapping(GetUserId(), server->serverid);
    ch_server_version v   = chfdw_get_server_version(user);

    PG_RETURN_TEXT_P(cstring_to_text(psprintf("%d.%d.%d", v.major, v.minor, v.patch)));
}

/*
 * Execute a query against a configured foreign server and return its rows,
 * mapping ClickHouse values to the Postgres types named in the caller's column
 * definition list. Reuses the server's driver, credentials and connection
 * cache, unlike clickhouse_raw_query which takes an ad-hoc connection string.
 */
Datum
clickhouse_query(PG_FUNCTION_ARGS) {
    char* server_name     = text_to_cstring(PG_GETARG_TEXT_P(0));
    char* sql             = text_to_cstring(PG_GETARG_TEXT_P(1));
    ReturnSetInfo* rsinfo = (ReturnSetInfo*)fcinfo->resultinfo;
    ForeignServer* server;
    UserMapping* user;
    ch_scan_connection sconn;
    ch_cursor* cursor;
    TupleDesc tupdesc;
    Tuplestorestate* tupstore;
    AttInMetadata* attinmeta;
    List* retrieved_attrs = NIL;
    MemoryContext per_query_ctx, oldcontext, row_cxt;
    Datum* values;
    bool* nulls;

    /* set-returning-function protocol: caller must accept a materialized set */
    if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("set-valued function called in context that cannot accept a set")
        );
    }
    if (!(rsinfo->allowedModes & SFRM_Materialize)) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("materialize mode required, but it is not allowed in this context")
        );
    }

    /* result shape comes from the caller's column definition list */
    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE) {
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("a column definition list is required for clickhouse_query")
        );
    }

    /*
     * Build result info in per-query context so it outlives this call. tupdesc
     * supplies column names; attinmeta caches fetch_row to coerce ClickHouse
     * values to declared type. retrieved_attrs lists 1-based attributes to
     * populate, skipping any column definition list left dropped.
     */
    per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
    oldcontext    = MemoryContextSwitchTo(per_query_ctx);
    tupdesc       = CreateTupleDescCopy(tupdesc);
    tupstore      = tuplestore_begin_heap(
        rsinfo->allowedModes & SFRM_Materialize_Random, false, work_mem
    );
    rsinfo->returnMode = SFRM_Materialize;
    rsinfo->setResult  = tupstore;
    rsinfo->setDesc    = tupdesc;
    attinmeta          = TupleDescGetAttInMetadata(tupdesc);
    for (int i = 1; i <= tupdesc->natts; i++) {
        if (!TupleDescAttr(tupdesc, i - 1)->attisdropped) {
            retrieved_attrs = lappend_int(retrieved_attrs, i);
        }
    }
    MemoryContextSwitchTo(oldcontext);

    /* resolve mappings, borrow a cached scan connection to run query */
    server = GetForeignServerByName(server_name, false);
    user   = GetUserMapping(GetUserId(), server->serverid);
    sconn  = chfdw_get_scan_connection(user);

    ch_query query = new_query(sql, 0, NULL, tupdesc, retrieved_attrs);

    cursor = sconn.gate.methods->simple_query(sconn.gate.conn, &query);

    values = palloc(tupdesc->natts * sizeof(Datum));
    nulls  = palloc(tupdesc->natts * sizeof(bool));

    /* per-row values are copied into tuplestore; reset between rows */
    row_cxt = AllocSetContextCreate(
        CurrentMemoryContext, "clickhouse_query row", ALLOCSET_DEFAULT_SIZES
    );

    for (;;) {
        ChFdwScanRowContext ctx = { tupdesc, retrieved_attrs, attinmeta,
                                    cursor,  values,          nulls };

        memset(values, 0, tupdesc->natts * sizeof(Datum));
        memset(nulls, true, tupdesc->natts * sizeof(bool));

        oldcontext = MemoryContextSwitchTo(row_cxt);
        if (sconn.gate.methods->fetch_row(&ctx) == NULL) {
            MemoryContextSwitchTo(oldcontext);
            break;
        }
        MemoryContextSwitchTo(oldcontext);

        tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        MemoryContextReset(row_cxt);
        CHECK_FOR_INTERRUPTS();
    }

    MemoryContextDelete(row_cxt);
    MemoryContextDelete(cursor->memcxt);
    chfdw_release_scan_connection(user, sconn);

    return (Datum)0;
}

/* calculate difference */
double
time_diff(struct timeval* prior, struct timeval* latter) {
    double x = (double)(latter->tv_usec - prior->tv_usec) / 1000.0L +
               (double)(latter->tv_sec - prior->tv_sec) * 1000.0L;

    return x;
}

static int
get_fetch_size_option(DefElem* def) {
    int fetch_size = pg_strtoint32(defGetString(def));

    if (fetch_size < 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
            errmsg(
                "invalid value for option \"%s\": %s", def->defname, defGetString(def)
            ),
            errhint("fetch_size must be greater than or equal to 0")
        );
    }

    return fetch_size;
}

/*
 * Utility function to fetch a cascading option definition from `fpinfo`.
 * Returns the first option that matches in `fpinfo->table->options` or, if
 * none, then the first found in `fpinfo->server->options`. Returns `NULL`
 * when none found.
 */
static DefElem*
ch_get_table_or_server_option(CHFdwRelationInfo* fpinfo, char* name) {
    ListCell* lc;

    foreach (lc, fpinfo->table->options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (strcmp(def->defname, name) == 0) {
            return def;
        }
    }

    foreach (lc, fpinfo->server->options) {
        DefElem* def = (DefElem*)lfirst(lc);

        if (strcmp(def->defname, name) == 0) {
            return def;
        }
    }

    return NULL;
}

/*
 * clickhouseGetForeignRelSize
 *		Estimate # of rows and width of the result of the scan
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 */
static void
clickhouseGetForeignRelSize(
    PlannerInfo* root,
    RelOptInfo* baserel,
    Oid foreigntableid
) {
    CHFdwRelationInfo* fpinfo;
    ListCell* lc;
    RangeTblEntry* rte = planner_rt_fetch(baserel->relid, root);
    char *relname, *refname;

    /*
     * We use CHFdwRelationInfo to pass various information to subsequent
     * functions.
     */
    fpinfo               = (CHFdwRelationInfo*)palloc0(sizeof(CHFdwRelationInfo));
    baserel->fdw_private = (void*)fpinfo;

    /* Base foreign tables need to be pushed down always. */
    fpinfo->pushdown_safe = true;

    /* Look up foreign-table catalog info. */
    fpinfo->table  = GetForeignTable(foreigntableid);
    fpinfo->server = GetForeignServer(fpinfo->table->serverid);

    /*
     * Extract user-settable option values. Note that per-table setting of
     * use_remote_estimate overrides per-server setting.
     */
    fpinfo->fdw_startup_cost     = DEFAULT_FDW_STARTUP_COST;
    fpinfo->fdw_tuple_cost       = DEFAULT_FDW_TUPLE_COST;
    fpinfo->shippable_extensions = NIL;

    /*
     * Extract fetch_size: table option overrides server option, default
     * DEFAULT_FETCH_SIZE. Value is approximate batch size in bytes; 0 means
     * buffer entire response (disable HTTP streaming).
     */
    DefElem* def = ch_get_table_or_server_option(fpinfo, "fetch_size");

    fpinfo->fetch_size = def ? get_fetch_size_option(def) : DEFAULT_FETCH_SIZE;

    chfdw_apply_custom_table_options(fpinfo, foreigntableid);

    fpinfo->user = NULL;

    /*
     * Identify which baserestrictinfo clauses can be sent to the remote
     * server and which can't.
     */
    chfdw_classify_conditions(
        root,
        baserel,
        baserel->baserestrictinfo,
        &fpinfo->remote_conds,
        &fpinfo->local_conds
    );

    /*
     * Identify which attributes will need to be retrieved from the remote
     * server. These include all attrs needed for joins or final output, plus
     * all attrs used in the local_conds. (Note: if we end up using a
     * parameterized scan, it's possible that some of the join clauses will be
     * sent to the remote and thus we wouldn't really need to retrieve the
     * columns used in them. Doesn't seem worth detecting that case though.)
     */
    fpinfo->attrs_used = NULL;
    pull_varattnos(
        (Node*)baserel->reltarget->exprs, baserel->relid, &fpinfo->attrs_used
    );
    foreach (lc, fpinfo->local_conds) {
        RestrictInfo* rinfo = lfirst_node(RestrictInfo, lc);

        pull_varattnos((Node*)rinfo->clause, baserel->relid, &fpinfo->attrs_used);
    }

    /*
     * Compute the selectivity and cost of the local_conds, so we don't have
     * to do it over again for each path. The best we can do for these
     * conditions is to estimate selectivity on the basis of local statistics.
     */
    fpinfo->local_conds_sel = clauselist_selectivity(
        root, fpinfo->local_conds, baserel->relid, JOIN_INNER, NULL
    );

    cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

    /*
     * Set cached relation costs to some negative value, so that we can detect
     * when they are set to some sensible costs during one (usually the first)
     * of the calls to estimate_path_cost_size().
     */
    fpinfo->rel_startup_cost = -1;
    fpinfo->rel_total_cost   = -1;

    /* Make base scans more expensive than join pushdowns */
    fpinfo->rows         = baserel->rows;
    fpinfo->startup_cost = 10.0;
    fpinfo->total_cost   = 10.0 + baserel->rows * 0.01;

    /*
     * Set the name of relation in fpinfo, while we are constructing it here.
     * It will be used to build the string describing the join relation in
     * EXPLAIN output. We can't know whether VERBOSE option is specified or
     * not, so always schema-qualify the foreign table name.
     */
    fpinfo->relation_name = makeStringInfo();
    relname               = get_rel_name(foreigntableid);
    refname               = rte->eref->aliasname;
    appendStringInfoString(fpinfo->relation_name, quote_identifier(relname));
    if (*refname && strcmp(refname, relname) != 0) {
        appendStringInfoChar(fpinfo->relation_name, ' ');
        appendStringInfoString(
            fpinfo->relation_name, quote_identifier(rte->eref->aliasname)
        );
    }

    /* No outer and inner relations. */
    fpinfo->make_outerrel_subquery = false;
    fpinfo->make_innerrel_subquery = false;
    fpinfo->lower_subquery_rels    = NULL;
    /* Set the relation index. */
    fpinfo->relation_index = baserel->relid;
}

/*
 * get_useful_pathkeys_for_relation
 *		Determine which orderings of a relation might be useful.
 *
 * Getting data in sorted order can be useful either because the requested
 * order matches the final output ordering for the overall query we're
 * planning, or because it enables an efficient merge join. Here, we try
 * to figure out which pathkeys to consider.
 */
static List*
get_useful_pathkeys_for_relation(PlannerInfo* root, RelOptInfo* rel) {
    List* useful_pathkeys_list = NIL;
    CHFdwRelationInfo* fpinfo  = (CHFdwRelationInfo*)rel->fdw_private;
    ListCell* lc;

    /*
     * Pushing the query_pathkeys to the remote server is always worth
     * considering, because it might let us avoid a local sort.
     */
    fpinfo->qp_is_pushdown_safe = false;
    if (root->query_pathkeys) {
        bool query_pathkeys_ok = true;

        foreach (lc, root->query_pathkeys) {
            PathKey* pathkey             = (PathKey*)lfirst(lc);
            EquivalenceClass* pathkey_ec = pathkey->pk_eclass;
            Expr* em_expr;

            /*
             * The planner and executor don't have any clever strategy for
             * taking data sorted by a prefix of the query's pathkeys and
             * getting it to be sorted by all of those pathkeys. We'll just
             * end up resorting the entire data set. So, unless we can push
             * down all of the query pathkeys, forget it.
             *
             * chfdw_is_foreign_expr would detect volatile expressions as
             * well, but checking ec_has_volatile here saves some cycles.
             */
            if (pathkey_ec->ec_has_volatile ||
                !(em_expr = chfdw_find_em_expr_for_rel(pathkey_ec, rel)) ||
                !chfdw_is_foreign_expr(root, rel, em_expr, true)) {
                query_pathkeys_ok = false;
                break;
            }
        }

        if (query_pathkeys_ok) {
            useful_pathkeys_list        = list_make1(list_copy(root->query_pathkeys));
            fpinfo->qp_is_pushdown_safe = true;
        }
    }

    return useful_pathkeys_list;
}

/*
 * clickhouseGetForeignPaths
 *		Create possible scan paths for a scan on the foreign table
 */
static void
clickhouseGetForeignPaths(PlannerInfo* root, RelOptInfo* baserel, Oid foreigntableid) {
    ForeignPath* path;
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)baserel->fdw_private;

    path = create_foreignscan_path(
        root,
        baserel,
        NULL,
        fpinfo->rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        fpinfo->startup_cost,
        fpinfo->total_cost,
        NULL,
        NULL,
        NULL,
        NIL
#if PG_VERSION_NUM >= 170000
        ,
        NIL
#endif
    );

    add_path(baserel, (Path*)path);
    add_paths_with_pathkeys_for_rel(root, baserel, NULL);
}

/*
 * Substitute WindowFunc nodes with FuncExpr placeholders in both
 * fdw_scan_tlist and the outer targetlist of a ForeignScan. Needed because
 * EXPLAIN VERBOSE deparses WindowFunc via a WindowClause/WindowAgg lookup
 * that is not available when windowing is pushed down to ClickHouse
 * (ruleutils.c:get_windowfunc_expr_helper).
 *
 * Placeholder must be non-Const/Var/Param so setrefs' fix_upper_expr will
 * rewrite outer tlist entries to INDEX_VAR references against fdw_scan_tlist
 * (search_indexed_tlist_for_non_var explicitly skips Consts). An un-rewritten
 * Const would emit its literal at execution time instead of reading the
 * computed column from the scan tuple.
 *
 * FuncExpr on wf->winfnoid deparses as "funcname(args)", close to the
 * original minus the OVER clause (which the Remote SQL line already prints).
 *
 * Two WindowFuncs with identical args but different winref refer to distinct
 * windows, so their placeholders must stay distinguishable to equal(),
 * otherwise setrefs collapses both outer refs to the first fdw_scan_tlist
 * slot and execution reads the wrong column. inputcollid is abused as a
 * counter: equal() compares it, while the executor only consults when
 * function is actually invoked; this never happens as placeholder
 * lives only as schema in fdw_scan_tlist or is rewritten to INDEX_VAR in
 * outer tlist.
 */
typedef struct WindowFuncSubstState {
    List* originals;    /* WindowFunc nodes seen so far */
    List* placeholders; /* matching placeholder nodes (same index) */
    int counter;
} WindowFuncSubstState;

static Node*
replace_windowfuncs_mutator(Node* node, WindowFuncSubstState* state);
static ForeignScan*
clickhouseGetForeignSetopPlan(RelOptInfo* foreignrel, List* tlist, Plan* outer_plan);
static ForeignScan*
clickhouseGetForeignSetopGroupingPlan(
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    List* tlist,
    Plan* outer_plan
);
static void
build_foreign_setop_query(
    RelOptInfo* foreignrel,
    CHFdwRelationInfo* fpinfo,
    Plan* outer_plan,
    List* setop_tlist,
    StringInfo sql,
    List** params_list
);
static void
append_setop_query(
    StringInfo buf,
    ForeignScan* scan,
    int arm_number,
    int param_offset,
    int expected_columns
);
static void
append_renumbered_query(StringInfo buf, const char* query, int param_offset);
static List*
setop_projection_positions(
    ForeignScan* scan,
    int expected_columns,
    int* source_columns
);
static List*
make_setop_scan_tlist(List* source_tlist);

static Node*
replace_windowfuncs_mutator_callback(Node* node, void* state) {
    return replace_windowfuncs_mutator(node, state);
}

static Node*
replace_windowfuncs_mutator(Node* node, WindowFuncSubstState* state) {
    if (node == NULL) {
        return NULL;
    }
    if (IsA(node, WindowFunc)) {
        WindowFunc* wf = (WindowFunc*)node;
        ListCell *lo, *lp;
        FuncExpr* ph;

        forboth(lo, state->originals, lp, state->placeholders) {
            if (equal(lfirst(lo), wf)) {
                return (Node*)copyObject(lfirst(lp));
            }
        }

        ph                 = makeNode(FuncExpr);
        ph->funcid         = wf->winfnoid;
        ph->funcresulttype = wf->wintype;
        ph->funcretset     = false;
        ph->funcvariadic   = false;
        ph->funcformat     = COERCE_EXPLICIT_CALL;
        ph->funccollid     = wf->wincollid;
        ph->inputcollid    = ++state->counter;
        ph->args           = (List*)copyObject(wf->args);
        ph->location       = -1;

        state->originals    = lappend(state->originals, copyObject(wf));
        state->placeholders = lappend(state->placeholders, ph);
        return (Node*)ph;
    }
    return expression_tree_mutator(node, replace_windowfuncs_mutator_callback, state);
}

/*
 * Append query while shifting ClickHouse parameter indexes. Each arm was
 * planned independently and therefore starts numbering at p1.
 */
static void
append_renumbered_query(StringInfo buf, const char* query, int param_offset) {
    const char* cursor = query;
    char quote         = '\0';

    while (*cursor) {
        if (quote != '\0') {
            appendStringInfoChar(buf, *cursor);

            if (*cursor == '\\' && cursor[1] != '\0') {
                appendStringInfoChar(buf, cursor[1]);
                cursor += 2;
                continue;
            }
            if (*cursor == quote) {
                if (cursor[1] == quote) {
                    appendStringInfoChar(buf, cursor[1]);
                    cursor += 2;
                    continue;
                }
                quote = '\0';
            }
            cursor++;
            continue;
        }

        if (*cursor == '\'' || *cursor == '"' || *cursor == '`') {
            quote = *cursor;
            appendStringInfoChar(buf, *cursor++);
            continue;
        }

        if (cursor[0] == '{' && cursor[1] == 'p' && cursor[2] >= '0' &&
            cursor[2] <= '9') {
            const char* number = cursor + 2;
            const char* end    = number;
            unsigned long index;

            while (*end >= '0' && *end <= '9') {
                end++;
            }
            if (*end == ':') {
                index = strtoul(number, NULL, 10);
                appendStringInfo(buf, "{p%lu", index + param_offset);
                cursor = end;
                continue;
            }
        }

        appendStringInfoChar(buf, *cursor++);
    }
}

/*
 * Map a ForeignScan's projected output to positions in its remote result.
 * A base scan reconstructs a table row according to retrieved_attrs before
 * applying its plan target list. Join and upper scans expose fdw_scan_tlist
 * directly. Set-operation arms may only use those already-remote columns;
 * any local expression keeps the UNION local.
 */
static List*
setop_projection_positions(
    ForeignScan* scan,
    int expected_columns,
    int* source_columns
) {
    List* positions = NIL;
    ListCell* lc;

    if (scan->scan.plan.qual != NIL || scan->scan.plan.initPlan != NIL) {
        return NIL;
    }

    if (scan->scan.scanrelid > 0) {
        List* retrieved_attrs =
            (List*)list_nth(scan->fdw_private, FdwScanPrivateRetrievedAttrs);

        *source_columns = list_length(retrieved_attrs);
        foreach (lc, scan->scan.plan.targetlist) {
            TargetEntry* tle = lfirst_node(TargetEntry, lc);
            Var* var;
            ListCell* attr_lc;
            int position = 0;
            bool found   = false;

            if (tle->resjunk || !IsA(tle->expr, Var)) {
                return NIL;
            }
            var = castNode(Var, tle->expr);
            if ((int64)var->varno != (int64)scan->scan.scanrelid ||
                var->varlevelsup != 0 || var->varattno <= 0) {
                return NIL;
            }

            foreach (attr_lc, retrieved_attrs) {
                position++;
                if (lfirst_int(attr_lc) == var->varattno) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return NIL;
            }
            positions = lappend_int(positions, position);
        }
    } else {
        List* remote_tlist = scan->fdw_scan_tlist;

        *source_columns = list_length(remote_tlist);
        foreach (lc, scan->scan.plan.targetlist) {
            TargetEntry* tle = lfirst_node(TargetEntry, lc);
            TargetEntry* remote_tle;

            if (tle->resjunk) {
                return NIL;
            }
            remote_tle = tlist_member(tle->expr, remote_tlist);
            if (remote_tle == NULL || remote_tle->resjunk) {
                return NIL;
            }
            positions = lappend_int(positions, remote_tle->resno);
        }
    }

    if (*source_columns <= 0 || list_length(positions) != expected_columns) {
        return NIL;
    }
    return positions;
}

/*
 * Normalize one independently-planned remote arm to positional cN columns.
 * tuple(*) avoids depending on source column names, while tupleElement keeps
 * this compatible with ClickHouse versions that predate derived-table column
 * alias lists. The explicit output aliases make names deterministic across
 * reordered base columns, joins, nested UNIONs, and expressions.
 */
static void
append_setop_query(
    StringInfo buf,
    ForeignScan* scan,
    int arm_number,
    int param_offset,
    int expected_columns
) {
    List* positions;
    ListCell* lc;
    int source_columns;
    int output_column = 0;
    const char* query = strVal(list_nth(scan->fdw_private, FdwScanPrivateSelectSql));

    positions = setop_projection_positions(scan, expected_columns, &source_columns);
    if (positions == NIL) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("pg_clickhouse: UNION arm requires local projection")
        );
    }

    appendStringInfoChar(buf, '(');
    appendStringInfoString(buf, "SELECT ");
    foreach (lc, positions) {
        if (output_column > 0) {
            appendStringInfoString(buf, ", ");
        }
        output_column++;
        appendStringInfo(
            buf, "tupleElement(tuple(*), %d) AS c%d", lfirst_int(lc), output_column
        );
    }

    appendStringInfoString(buf, " FROM (");
    append_renumbered_query(buf, query, param_offset);
    appendStringInfo(buf, ") AS u%d)", arm_number);
}

/*
 * Set-operation targets use planner-internal Vars with varno 0, which cannot
 * appear in a finished ForeignScan. Replace them with unique, typed
 * placeholders in both plan and scan target lists; setrefs then rewrites the
 * plan target to INDEX_VAR references to the remote scan tuple.
 */
static List*
make_setop_scan_tlist(List* source_tlist) {
    List* result = NIL;
    ListCell* lc;
    int column = 0;

    foreach (lc, source_tlist) {
        TargetEntry* source_tle = lfirst_node(TargetEntry, lc);
        CoerceViaIO* placeholder;
        RelabelType* relabel;
        TargetEntry* tle;
        Oid type;
        int32 typmod;
        Oid collation;

        if (source_tle->resjunk) {
            continue;
        }

        column++;
        type      = exprType((Node*)source_tle->expr);
        typmod    = exprTypmod((Node*)source_tle->expr);
        collation = exprCollation((Node*)source_tle->expr);

        placeholder      = makeNode(CoerceViaIO);
        placeholder->arg = (Expr*)makeConst(
            INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(column), false, true
        );
        placeholder->resulttype   = type;
        placeholder->resultcollid = collation;
        placeholder->coerceformat = COERCE_IMPLICIT_CAST;
        placeholder->location     = -1;

        relabel = makeRelabelType(
            (Expr*)placeholder, type, typmod, collation, COERCE_IMPLICIT_CAST
        );
        tle = makeTargetEntry(
            (Expr*)relabel,
            column,
            source_tle->resname ? pstrdup(source_tle->resname) : NULL,
            false
        );
        result = lappend(result, tle);
    }

    return result;
}

/*
 * Build a UNION query from the independently planned pg_clickhouse scans
 * under the planner's Append path.
 */
static void
build_foreign_setop_query(
    RelOptInfo* foreignrel,
    CHFdwRelationInfo* fpinfo,
    Plan* outer_plan,
    List* setop_tlist,
    StringInfo sql,
    List** params_list
) {
    List* foreign_scans = NIL;
    ListCell* lc;
    int arm_number = 0;
    int expected_columns;
    int param_offset = 0;

    if (outer_plan == NULL ||
        !chfdw_setop_extract_foreign_scans(
            outer_plan, foreignrel->serverid, &foreign_scans
        ) ||
        list_length(foreign_scans) < 2) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("pg_clickhouse: invalid UNION pushdown plan")
        );
    }

    expected_columns = list_length(setop_tlist);
    if (expected_columns <= 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg("pg_clickhouse: UNION has no remote output columns")
        );
    }

    initStringInfo(sql);
    foreach (lc, foreign_scans) {
        ForeignScan* scan = lfirst_node(ForeignScan, lc);
        ListCell* param_lc;

        if (arm_number > 0) {
            appendStringInfoString(
                sql, fpinfo->setop_all ? " UNION ALL " : " UNION DISTINCT "
            );
        }
        arm_number++;
        append_setop_query(sql, scan, arm_number, param_offset, expected_columns);

        foreach (param_lc, scan->fdw_exprs) {
            *params_list = lappend(*params_list, copyObject(lfirst(param_lc)));
        }
        param_offset += list_length(scan->fdw_exprs);
    }
}

/*
 * Build a single ForeignScan for a UNION from the independently planned
 * pg_clickhouse scans under the planner's Append path.
 */
static ForeignScan*
clickhouseGetForeignSetopPlan(RelOptInfo* foreignrel, List* tlist, Plan* outer_plan) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;
    List* params_list         = NIL;
    List* retrieved_attrs     = NIL;
    List* fdw_private;
    StringInfoData sql;
    List* scan_tlist;

    (void)tlist;

    build_foreign_setop_query(
        foreignrel, fpinfo, outer_plan, fpinfo->grouped_tlist, &sql, &params_list
    );

    for (int column = 1; column <= list_length(fpinfo->grouped_tlist); column++) {
        retrieved_attrs = lappend_int(retrieved_attrs, column);
    }

    fdw_private = list_make3(
        makeString(sql.data), retrieved_attrs, makeInteger(fpinfo->fetch_size)
    );
    fdw_private = lappend(fdw_private, makeString(fpinfo->relation_name->data));
    fdw_private =
        lappend(fdw_private, makeString(psprintf("%u", fpinfo->setop_userid)));

    scan_tlist = make_setop_scan_tlist(fpinfo->grouped_tlist);

    return make_foreignscan(
        copyObject(scan_tlist), NIL, 0, params_list, fdw_private, scan_tlist, NIL, NULL
    );
}

/*
 * Build grouping or aggregation as a wrapper around a remote UNION ALL.  The
 * Append input plan is used only to obtain the independently deparsed arm
 * queries and parameters; the returned scan replaces that input completely.
 */
static ForeignScan*
clickhouseGetForeignSetopGroupingPlan(
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    List* tlist,
    Plan* outer_plan
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;
    List* params_list         = NIL;
    List* retrieved_attrs     = NIL;
    List* fdw_private;
    List* scan_tlist;
    StringInfoData setop_sql;
    StringInfoData sql;

    build_foreign_setop_query(
        foreignrel, fpinfo, outer_plan, fpinfo->setop_tlist, &setop_sql, &params_list
    );

    initStringInfo(&sql);
    chfdw_deparse_setop_grouping_stmt(
        &sql,
        root,
        foreignrel,
        fpinfo->grouped_tlist,
        fpinfo->setop_tlist,
        setop_sql.data,
        &retrieved_attrs,
        &params_list
    );

    fdw_private = list_make3(
        makeString(sql.data), retrieved_attrs, makeInteger(fpinfo->fetch_size)
    );
    fdw_private = lappend(fdw_private, makeString(fpinfo->relation_name->data));
    fdw_private =
        lappend(fdw_private, makeString(psprintf("%u", fpinfo->setop_userid)));
    scan_tlist = copyObject(fpinfo->grouped_tlist);

    return make_foreignscan(
        tlist, NIL, 0, params_list, fdw_private, scan_tlist, NIL, NULL
    );
}

/*
 * clickhouseGetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan*
clickhouseGetForeignPlan(
    PlannerInfo* root,
    RelOptInfo* foreignrel,
    Oid foreigntableid,
    ForeignPath* best_path,
    List* tlist,
    List* scan_clauses,
    Plan* outer_plan
) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)foreignrel->fdw_private;
    Index scan_relid;
    List* fdw_private;
    List* remote_exprs      = NIL;
    List* local_exprs       = NIL;
    List* params_list       = NIL;
    List* fdw_scan_tlist    = NIL;
    List* fdw_recheck_quals = NIL;
    List* retrieved_attrs;
    StringInfoData sql;
    bool has_final_sort = false;
    bool has_limit      = false;
    ListCell* lc;
    struct timeval time1, time2;

    gettimeofday(&time1, NULL);

    if (fpinfo->is_setop_grouping) {
        ForeignScan* plan =
            clickhouseGetForeignSetopGroupingPlan(root, foreignrel, tlist, outer_plan);

        gettimeofday(&time2, NULL);
        time_used += time_diff(&time1, &time2);
        return plan;
    }

    if (fpinfo->is_setop) {
        ForeignScan* plan =
            clickhouseGetForeignSetopPlan(foreignrel, tlist, outer_plan);

        gettimeofday(&time2, NULL);
        time_used += time_diff(&time1, &time2);
        return plan;
    }

    /*
     * Get FDW private data created by clickhouseGetForeignUpperPaths(), if
     * any.
     */
    if (best_path->fdw_private) {
        has_final_sort =
            intVal(list_nth(best_path->fdw_private, FdwPathPrivateHasFinalSort));
        has_limit = intVal(list_nth(best_path->fdw_private, FdwPathPrivateHasLimit));
    }

    if (IS_SIMPLE_REL(foreignrel)) {
        /*
         * For base relations, set scan_relid as the relid of the relation.
         */
        scan_relid = foreignrel->relid;

        /*
         * In a base-relation scan, we must apply the given scan_clauses.
         *
         * Separate the scan_clauses into those that can be executed remotely
         * and those that can't. baserestrictinfo clauses that were previously
         * determined to be safe or unsafe by chfdw_classify_conditions are
         * found in fpinfo->remote_conds and fpinfo->local_conds. Anything
         * else in the scan_clauses list will be a join clause, which we have
         * to check for remote-safety.
         *
         * Note: the join clauses we see here should be the exact same ones
         * previously examined by clickhouseGetForeignPaths. Possibly it'd be
         * worth passing forward the classification work done then, rather
         * than repeating it here.
         *
         * This code must match "extract_actual_clauses(scan_clauses, false)"
         * except for the additional decision about remote versus local
         * execution.
         */
        foreach (lc, scan_clauses) {
            RestrictInfo* rinfo = lfirst_node(RestrictInfo, lc);

            /* Ignore any pseudoconstants, they're dealt with elsewhere */
            if (rinfo->pseudoconstant) {
                continue;
            }

            if (list_member_ptr(fpinfo->remote_conds, rinfo)) {
                remote_exprs = lappend(remote_exprs, rinfo->clause);
            } else if (list_member_ptr(fpinfo->local_conds, rinfo)) {
                local_exprs = lappend(local_exprs, rinfo->clause);
            } else if (chfdw_is_foreign_expr(root, foreignrel, rinfo->clause, false)) {
                remote_exprs = lappend(remote_exprs, rinfo->clause);
            } else {
                local_exprs = lappend(local_exprs, rinfo->clause);
            }
        }

        /*
         * For a base-relation scan, we have to support EPQ recheck, which
         * should recheck all the remote quals.
         */
        fdw_recheck_quals = remote_exprs;
    } else {
        /*
         * Join relation or upper relation - set scan_relid to 0.
         */
        scan_relid = 0;

        /*
         * For a join rel, baserestrictinfo is NIL and we are not considering
         * parameterization right now, so there should be no scan_clauses for
         * a joinrel or an upper rel either.
         */
        Assert(!scan_clauses);

        /*
         * Instead we get the conditions to apply from the fdw_private
         * structure.
         */
        remote_exprs = extract_actual_clauses(fpinfo->remote_conds, false);
        local_exprs  = extract_actual_clauses(fpinfo->local_conds, false);

        /*
         * We leave fdw_recheck_quals empty in this case, since we never need
         * to apply EPQ recheck clauses. In the case of a joinrel, EPQ recheck
         * is handled elsewhere --- see clickhouseGetForeignJoinPaths(). If
         * we're planning an upperrel (ie, remote grouping or aggregation)
         * then there's no EPQ to do because SELECT FOR UPDATE wouldn't be
         * allowed, and indeed we *can't* put the remote clauses into
         * fdw_recheck_quals because the unaggregated Vars won't be available
         * locally.
         */

        /* Build the list of columns to be fetched from the foreign server. */
        fdw_scan_tlist = chfdw_build_tlist_to_deparse(foreignrel);

        /*
         * Ensure that the outer plan produces a tuple whose descriptor
         * matches our scan tuple slot. This is safe because all scans and
         * joins support projection, so we never need to insert a Result node.
         * Also, remove the local conditions from outer plan's quals, lest
         * they be evaluated twice, once by the local plan and once by the
         * scan.
         */
        if (outer_plan) {
            ListCell* outer_lc;

            /*
             * Right now, we only consider grouping and aggregation beyond
             * joins. Queries involving aggregates or grouping do not require
             * EPQ mechanism, hence should not have an outer plan here.
             */
            Assert(!IS_UPPER_REL(foreignrel));

            outer_plan->targetlist = fdw_scan_tlist;

            foreach (outer_lc, local_exprs) {
                Join* join_plan = (Join*)outer_plan;
                Node* qual      = lfirst(outer_lc);

                outer_plan->qual = list_delete(outer_plan->qual, qual);

                /*
                 * For an inner join the local conditions of foreign scan plan
                 * can be part of the joinquals as well.
                 */
                if (join_plan->jointype == JOIN_INNER) {
                    join_plan->joinqual = list_delete(join_plan->joinqual, qual);
                }
            }
        }
    }

    /*
     * Build the query string to be sent for execution, and identify
     * expressions to be sent as parameters.
     */
    initStringInfo(&sql);
    chfdw_deparse_select_stmt_for_rel(
        &sql,
        root,
        foreignrel,
        fdw_scan_tlist,
        remote_exprs,
        best_path->path.pathkeys,
        has_final_sort,
        has_limit,
        false,
        &retrieved_attrs,
        &params_list
    );

    /* Remember remote_exprs for possible use by clickhousePlanDirectModify */
    fpinfo->final_remote_exprs = remote_exprs;

    /*
     * When window functions are pushed down, ForeignScan takes the place of
     * what would otherwise be a WindowAgg plan node. Core EXPLAIN VERBOSE
     * deparses WindowFunc via a WindowClause (query context) or WindowAgg in
     * the plan; neither exists here, so deparse would error with "could not
     * find window clause for winref N". Remote SQL has already been deparsed
     * above, so swap WindowFunc for FuncExpr placeholders in both
     * fdw_scan_tlist and the outer tlist. setrefs' equal() match rewrites the
     * outer tlist to INDEX_VAR references against fdw_scan_tlist so execution
     * reads real values from the scan tuple; VERBOSE deparse sees the
     * placeholder instead of a WindowFunc.
     */
    if (contain_window_function((Node*)fdw_scan_tlist)) {
        WindowFuncSubstState state = { NIL, NIL, 0 };

        fdw_scan_tlist =
            (List*)replace_windowfuncs_mutator((Node*)fdw_scan_tlist, &state);
        tlist = (List*)replace_windowfuncs_mutator((Node*)tlist, &state);
    }

    /*
     * Build the fdw_private list that will be available to the executor.
     * Items in the list must match order in enum FdwScanPrivateIndex.
     */
    fdw_private = list_make3(
        makeString(sql.data), retrieved_attrs, makeInteger(fpinfo->fetch_size)
    );
    if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel)) {
        fdw_private = lappend(fdw_private, makeString(fpinfo->relation_name->data));
    }

    gettimeofday(&time2, NULL);
    time_used += time_diff(&time1, &time2);

    /*
     * Create the ForeignScan node for the given relation.
     *
     * Note that the remote parameter expressions are stored in the fdw_exprs
     * field of the finished plan node; we can't keep them in private state
     * because then they wouldn't be subject to later planner processing.
     */
    return make_foreignscan(
        tlist,
        local_exprs,
        scan_relid,
        params_list,
        fdw_private,
        fdw_scan_tlist,
        fdw_recheck_quals,
        outer_plan
    );
}

/*
 * clickhouseBeginForeignScan
 *		Initiate an executor scan of a foreign PostgreSQL table.
 */
static void
clickhouseBeginForeignScan(ForeignScanState* node, int eflags) {
    ForeignScan* fsplan = (ForeignScan*)node->ss.ps.plan;
    EState* estate      = node->ss.ps.state;
    ChFdwScanState* fsstate;
#if PG_VERSION_NUM < 160000
    RangeTblEntry* rte;
    int rtindex;
#endif
    Oid userid = InvalidOid;
    UserMapping* user;
    int numParams;

    /*
     * Do nothing in EXPLAIN (no ANALYZE) case. node->fdw_state stays NULL.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    /*
     * We'll save private state in node->fdw_state.
     */
    fsstate         = (ChFdwScanState*)palloc0(sizeof(ChFdwScanState));
    node->fdw_state = (void*)fsstate;

    /* Identify which user to do the remote access as. */
#if PG_VERSION_NUM >= 160000
    userid = OidIsValid(fsplan->checkAsUser) ? fsplan->checkAsUser : GetUserId();
#else
    /*
     * This should match what ExecCheckRTEPerms() does. In case of a join or
     * aggregate, use a foreign-table member RTE as a representative.
     * fs_relids can also contain synthetic join RTEs, which do not have a
     * relation OID.
     */
    if (list_length(fsplan->fdw_private) > FdwScanPrivateUserId) {
        userid = (Oid)strtoul(
            strVal(list_nth(fsplan->fdw_private, FdwScanPrivateUserId)), NULL, 10
        );
        if (!OidIsValid(userid)) {
            userid = GetUserId();
        }
    } else if (fsplan->scan.scanrelid > 0) {
        rtindex = fsplan->scan.scanrelid;
    } else {
        rtindex = -1;
        while ((rtindex = bms_next_member(fsplan->fs_relids, rtindex)) >= 0) {
            rte = rt_fetch(rtindex, estate->es_range_table);
            if (rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_FOREIGN_TABLE) {
                break;
            }
        }
        if (rtindex < 0) {
            elog(ERROR, "could not find foreign table for pg_clickhouse scan");
        }
    }
    if (list_length(fsplan->fdw_private) <= FdwScanPrivateUserId) {
        rte    = rt_fetch(rtindex, estate->es_range_table);
        userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
    }
#endif

    Assert(OidIsValid(userid));
    user = GetUserMapping(userid, fsplan->fs_server);

    /*
     * Get connection to the foreign server. Connection manager will establish
     * a new connection if necessary. A scan whose ancestor already holds the
     * cached connection gets a private one, since ClickHouse permits only one
     * query in flight per connection.
     */
    fsstate->user      = user;
    fsstate->scan_conn = chfdw_get_scan_connection(user);

    /* Get private info created by planner functions. */
    fsstate->query = strVal(list_nth(fsplan->fdw_private, FdwScanPrivateSelectSql));
    fsstate->retrieved_attrs =
        (List*)list_nth(fsplan->fdw_private, FdwScanPrivateRetrievedAttrs);
    fsstate->fetch_size =
        intVal(list_nth(fsplan->fdw_private, FdwScanPrivateFetchSize));

    /* Create contexts for batches of tuples and per-tuple temp workspace. */
    fsstate->batch_cxt = AllocSetContextCreate(
        estate->es_query_cxt, "pg_clickhouse tuple data", ALLOCSET_DEFAULT_SIZES
    );
    fsstate->temp_cxt = AllocSetContextCreate(
        estate->es_query_cxt, "pg_clickhouse temporary data", ALLOCSET_SMALL_SIZES
    );

    /*
     * Get info we'll need for converting data fetched from the foreign server
     * into local representation and error reporting during that process.
     */
    if (fsplan->scan.scanrelid > 0) {
        fsstate->rel     = node->ss.ss_currentRelation;
        fsstate->tupdesc = RelationGetDescr(fsstate->rel);
    } else {
        fsstate->rel     = NULL;
        fsstate->tupdesc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
    }

    fsstate->attinmeta = TupleDescGetAttInMetadata(fsstate->tupdesc);

    /*
     * Prepare for processing of parameters used in remote query, if any.
     */
    numParams          = list_length(fsplan->fdw_exprs);
    fsstate->numParams = numParams;
    if (numParams > 0) {
        prepare_query_params(
            (PlanState*)node,
            fsplan->fdw_exprs,
            numParams,
            &fsstate->param_oids,
            &fsstate->param_exprs,
            &fsstate->param_values
        );
    }
}

/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * temp_context is a working context that can be reset after each tuple.
 */
static HeapTuple
fetch_tuple(ChFdwScanState* fsstate, TupleDesc tupdesc) {
    Datum* values;
    HeapTuple tuple  = NULL;
    ItemPointer ctid = NULL;
    MemoryContext oldcontext;
    bool* nulls;

    oldcontext = MemoryContextSwitchTo(fsstate->temp_cxt);

    /*
     * Create enough slots for every attribute in the tuple definition
     * (tupdesc->natts), without regard to how many we'll actually fetch
     * (fsstate->retrieved_attrs), because we need all the slots to create the
     * Tuple. Those we don't fetch will simply be zeros.
     */
    values = (Datum*)palloc0(tupdesc->natts * sizeof(Datum));
    nulls  = (bool*)palloc(tupdesc->natts * sizeof(bool));

    /* Initialize to nulls for any columns not present in result */
    memset(nulls, true, tupdesc->natts * sizeof(bool));
    ChFdwScanRowContext ctx = { tupdesc,
                                fsstate->retrieved_attrs,
                                fsstate->attinmeta,
                                fsstate->ch_cursor,
                                values,
                                nulls };

    /* In both cases (binary and non binary), NULL means end of tuples. */
    {
        cursor_fetch_row_method fetch_fn = fsstate->scan_conn.gate.methods->fetch_row;

        if (fsstate->is_streaming) {
            Assert(fsstate->scan_conn.gate.methods->streaming_fetch_row != NULL);
            fetch_fn = fsstate->scan_conn.gate.methods->streaming_fetch_row;
        }

        if (fetch_fn(&ctx) == NULL) {
            MemoryContextSwitchTo(oldcontext);
            MemoryContextReset(fsstate->temp_cxt);
            return NULL;
        }
    }

    MemoryContextSwitchTo(oldcontext);

    tuple = heap_form_tuple(tupdesc, values, nulls);

    /*
     * If we have a CTID to return, install it in both t_self and t_ctid.
     * t_self is the normal place, but if the tuple is converted to a
     * composite Datum, t_self will be lost; setting t_ctid allows CTID to be
     * preserved during EvalPlanQual re-evaluations (see ROW_MARK_COPY code).
     */
    if (ctid) {
        tuple->t_self = tuple->t_data->t_ctid = *ctid;
    }

    /*
     * Stomp on the xmin, xmax, and cmin fields from the tuple created by
     * heap_form_tuple. heap_form_tuple actually creates the tuple with
     * DatumTupleFields, not HeapTupleFields, but the executor expects
     * HeapTupleFields and will happily extract system columns on that
     * assumption. If we don't do this then, for example, the tuple length
     * ends up in the xmin field, which isn't what we want.
     */
    HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
    HeapTupleHeaderSetXmin(tuple->t_data, InvalidTransactionId);
    HeapTupleHeaderSetCmin(tuple->t_data, InvalidTransactionId);

    MemoryContextReset(fsstate->temp_cxt);
    return tuple;
}

/*
 * clickhouseIterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 */
static TupleTableSlot*
clickhouseIterateForeignScan(ForeignScanState* node) {
    HeapTuple tup;
    ChFdwScanState* fsstate = (ChFdwScanState*)node->fdw_state;
    ExprContext* econtext   = node->ss.ps.ps_ExprContext;
    int numParams           = fsstate->numParams;
    const char** values     = fsstate->param_values;
    TupleTableSlot* slot    = node->ss.ss_ScanTupleSlot;
    struct timeval time1, time2;

    /* Allow query cancel (e.g. Ctrl+C) between tuple fetches. */
    CHECK_FOR_INTERRUPTS();

    /* make query if needed */
    if (fsstate->ch_cursor == NULL) {
        MemoryContext old = MemoryContextSwitchTo(fsstate->batch_cxt);
        ch_query query    = new_query(
            fsstate->query,
            fsstate->numParams,
            fsstate->param_values,
            fsstate->tupdesc,
            fsstate->retrieved_attrs
        );

        /*
         * Construct array of query parameter values in text format.  We do
         * the conversions in the short-lived per-tuple context, so as not to
         * cause a memory leak over repeated scans.
         */
        if (numParams > 0) {
            process_query_params(
                econtext, fsstate->param_oids, fsstate->param_exprs, values
            );
        }

        fsstate->is_streaming =
            fsstate->fetch_size > 0 &&
            fsstate->scan_conn.gate.methods->streaming_query != NULL;

        if (fsstate->is_streaming) {
            fsstate->ch_cursor = fsstate->scan_conn.gate.methods->streaming_query(
                fsstate->scan_conn.gate.conn, &query, fsstate->fetch_size
            );
        } else {
            /* Binary still falls back to the simple path for now. */
            fsstate->ch_cursor = fsstate->scan_conn.gate.methods->simple_query(
                fsstate->scan_conn.gate.conn, &query
            );
        }

        time_used += fsstate->ch_cursor->request_time;
        MemoryContextSwitchTo(old);
    }

    gettimeofday(&time1, NULL);
    tup = fetch_tuple(fsstate, fsstate->tupdesc);
    gettimeofday(&time2, NULL);
    time_used += time_diff(&time1, &time2);

    if (tup == NULL) {
        return ExecClearTuple(slot);
    }

    /*
     * Return the next tuple.
     */
    ExecStoreHeapTuple(tup, slot, false);
    return slot;
}

/*
 * dispose_scan
 *		Close the current cursor (freeing its response, and for the binary driver
 *		draining the connection so it stays reusable) and dispose of the per-scan
 *		batch context. Shared by the rescan and end callbacks.
 *
 *		When reset_batch_cxt is true (a rescan) the batch context is reset rather
 *		than deleted, because more IterateForeignScan calls follow and rebuild the
 *		cursor and response inside it. The cursor and response contexts are
 *		children of batch_cxt, so deleting it on a rescan would leave the next
 *		iteration allocating in a freed context, corrupting the response context
 *		and hanging its teardown on the following rescan of a correlated subquery.
 *		At end of scan (reset_batch_cxt false) it is deleted.
 */
static void
dispose_scan(ChFdwScanState* fsstate, bool reset_batch_cxt) {
    if (!fsstate) {
        return;
    }

    if (fsstate->ch_cursor) {
        MemoryContextDelete(fsstate->ch_cursor->memcxt);
        fsstate->ch_cursor = NULL;
    }

    if (fsstate->batch_cxt) {
        if (reset_batch_cxt) {
            MemoryContextReset(fsstate->batch_cxt);
        } else {
            MemoryContextDelete(fsstate->batch_cxt);
        }
    }
}

/*
 * clickhouseReScanForeignScan
 *		Dispose of the current cursor and tuple batch so the next iteration
 *		re-runs the query. Keeps the connection: the scan is not finished, and
 *		(for a correlated subquery) will run again with a new parameter value.
 */
static void
clickhouseReScanForeignScan(ForeignScanState* node) {
    ChFdwScanState* fsstate = (ChFdwScanState*)node->fdw_state;

    time_used = 0;
    dispose_scan(fsstate, true); /* reset batch_cxt; more iterations follow */
}

/*
 * clickhouseEndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
clickhouseEndForeignScan(ForeignScanState* node) {
    ChFdwScanState* fsstate = (ChFdwScanState*)node->fdw_state;

    time_used = 0;
    dispose_scan(fsstate, false); /* delete batch_cxt; scan is finished */
    if (fsstate) {
        chfdw_release_scan_connection(fsstate->user, fsstate->scan_conn);
    }
}

/*
 * clickhousePlanForeignModify
 *		Plan an insert operation on a foreign table
 */
static List*
clickhousePlanForeignModify(
    PlannerInfo* root,
    ModifyTable* plan,
    Index resultRelation,
    int subplan_index
) {
    CmdType operation  = plan->operation;
    RangeTblEntry* rte = planner_rt_fetch(resultRelation, root);
    Relation rel;
    StringInfoData sql;
    List* targetAttrs = NIL;

    initStringInfo(&sql);

    /*
     * Core code already has some lock on each rel being planned, so we can
     * use NoLock here.
     */
    rel = table_open_compat(rte->relid, NoLock);
    if (operation == CMD_INSERT) {
        TupleDesc tupdesc = RelationGetDescr(rel);
        int attnum;

        for (attnum = 1; attnum <= tupdesc->natts; attnum++) {
            Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

            if (!attr->attisdropped) {
                targetAttrs = lappend_int(targetAttrs, attnum);
            }
        }
    }

    /*
     * Construct the SQL command string.
     */
    char* table_name;

    switch (operation) {
    case CMD_INSERT:

        /*
         * Write start of INSERT statement to & sql:INSERT INTO table(col
         * list)
         */
        table_name =
            chfdw_deparse_insert_sql(&sql, rte, resultRelation, rel, targetAttrs);
        break;
    case CMD_UPDATE:
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("ClickHouse does not support updates")
        );
        break;
    case CMD_DELETE:
        ereport(
            ERROR,
            errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
            errmsg("ClickHouse does not support deletes")
        );
        break;
    default:
        elog(ERROR, "unexpected operation: %d", (int)operation);
        break;
    }

    table_close_compat(rel, NoLock);

    /*
     * Build the fdw_private list that will be available to the executor.
     * Items in the list must match enum FdwModifyPrivateIndex, above.
     */
    return list_make3(makeString(sql.data), targetAttrs, makeString(table_name));
}

/*
 * clickhouseBeginForeignModify
 *		Begin an insertoperation on a foreign table
 */
static void
clickhouseBeginForeignModify(
    ModifyTableState* mtstate,
    ResultRelInfo* resultRelInfo,
    List* fdw_private,
    int subplan_index,
    int eflags
) {
    CHFdwModifyState* fmstate;
    char* query;
    List* target_attrs = NULL;
    RangeTblEntry* rte;
    char* table_name;

    /*
     * Do nothing in EXPLAIN (no ANALYZE) case. resultRelInfo->ri_FdwState
     * stays NULL.
     */
    if (eflags & EXEC_FLAG_EXPLAIN_ONLY) {
        return;
    }

    /* Deconstruct fdw_private data. */
    query        = strVal(list_nth(fdw_private, FdwModifyPrivateInsertSQL));
    target_attrs = (List*)list_nth(fdw_private, FdwModifyPrivateTargetAttnums);
    table_name   = strVal(list_nth(fdw_private, FdwModifyPrivateTableName));

    /* Find RTE. */
    rte =
        rt_fetch(resultRelInfo->ri_RangeTableIndex, mtstate->ps.state->es_range_table);

    /* Construct an execution state. */
    fmstate = create_foreign_modify(
        mtstate->ps.state,
        rte,
        resultRelInfo,
        mtstate->operation,
#if PG_VERSION_NUM < 140000
        mtstate->mt_plans[subplan_index]->plan,
#else
        outerPlanState(mtstate)->plan,
#endif
        query,
        target_attrs,
        table_name
    );

    resultRelInfo->ri_FdwState = fmstate;
}

ForeignServer*
chfdw_get_foreign_server(Relation rel) {
    ForeignServer* server;
    ForeignTable* table;

    table  = GetForeignTable(RelationGetRelid(rel));
    server = GetForeignServer(table->serverid);
    return server;
}

/*
 * clickhouseBeginForeignInsert
 *		Begin an insert operation on a foreign table
 */
static void
clickhouseBeginForeignInsert(ModifyTableState* mtstate, ResultRelInfo* resultRelInfo) {
    CHFdwModifyState* fmstate;
    EState* estate       = mtstate->ps.state;
    Index resultRelation = resultRelInfo->ri_RangeTableIndex;
    Relation rel         = resultRelInfo->ri_RelationDesc;
    RangeTblEntry* rte;
    TupleDesc tupdesc = RelationGetDescr(rel);
    int attnum;
    List* targetAttrs = NIL;
    StringInfoData sql;
    char* table_name;

    /* We transmit all columns that are defined in the foreign table. */
    for (attnum = 1; attnum <= tupdesc->natts; attnum++) {
        Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

        if (!attr->attisdropped) {
            targetAttrs = lappend_int(targetAttrs, attnum);
        }
    }

    /*
     * If the foreign table is a partition, we need to create a new RTE
     * describing the foreign table for use by chfdw_deparse_insert_sql and
     * create_foreign_modify() below, after first copying the parent's RTE and
     * modifying some fields to describe the foreign partition to work on.
     * However, if this is invoked by UPDATE, the existing RTE may already
     * correspond to this partition if it is one of the UPDATE subplan target
     * rels; in that case, we can just use the existing RTE as-is.
     */
    rte = list_nth(estate->es_range_table, resultRelation - 1);
    if (rte->relid != RelationGetRelid(rel)) {
        rte          = (RangeTblEntry*)copyObjectImpl(rte);
        rte->relid   = RelationGetRelid(rel);
        rte->relkind = RELKIND_FOREIGN_TABLE;
    }

    initStringInfo(&sql);
    table_name = chfdw_deparse_insert_sql(&sql, rte, resultRelation, rel, targetAttrs);

    /* Construct an execution state. */
    fmstate = create_foreign_modify(
        mtstate->ps.state,
        rte,
        resultRelInfo,
        CMD_INSERT,
        NULL,
        sql.data,
        targetAttrs,
        table_name
    );

    resultRelInfo->ri_FdwState = fmstate;
}

/*
 * clickhouseExecForeignInsert
 *		Put one row to buffer, if buffer is big enough push it to ClickHouse
 */
static TupleTableSlot*
clickhouseExecForeignInsert(
    EState* estate,
    ResultRelInfo* resultRelInfo,
    TupleTableSlot* slot,
    TupleTableSlot* planSlot
) {
    MemoryContext oldcontext;

    CHFdwModifyState* fmstate = (CHFdwModifyState*)resultRelInfo->ri_FdwState;

    oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

    fmstate->conn.methods->insert_tuple(fmstate->state, slot);

    MemoryContextSwitchTo(oldcontext);
    MemoryContextReset(fmstate->temp_cxt);

    return slot;
}

/*
 * clickhouseEndForeignInsert
 *		Finish an insert operation on a foreign table
 */
static void
clickhouseEndForeignInsert(EState* estate, ResultRelInfo* resultRelInfo) {
    MemoryContext oldcontext;

    CHFdwModifyState* fmstate = (CHFdwModifyState*)resultRelInfo->ri_FdwState;

    if (fmstate) {
        /* flush */
        oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);
        fmstate->conn.methods->insert_tuple(fmstate->state, NULL);

        /*
         * Finalize on the happy path so the binary driver can ereport on a
         * server-side INSERT exception without raising from inside the
         * MemoryContext reset callback that fires during abort.
         */
        if (fmstate->conn.methods->finalize_insert) {
            fmstate->conn.methods->finalize_insert(fmstate->state);
        }

        MemoryContextSwitchTo(oldcontext);
        MemoryContextReset(fmstate->temp_cxt);

        /* Destroy the execution state */
        finish_foreign_modify(fmstate);
    }
}

/*
 * clickhouseRecheckForeignScan
 *		Execute a local join execution plan for a foreign join
 */
static bool
clickhouseRecheckForeignScan(ForeignScanState* node, TupleTableSlot* slot) {
    Index scanrelid      = ((Scan*)node->ss.ps.plan)->scanrelid;
    PlanState* outerPlan = outerPlanState(node);
    TupleTableSlot* result;

    /* For base foreign relations, it suffices to set fdw_recheck_quals */
    if (scanrelid > 0) {
        return true;
    }

    Assert(outerPlan != NULL);

    /* Execute a local join execution plan */
    result = ExecProcNode(outerPlan);
    if (TupIsNull(result)) {
        return false;
    }

    /* Store result in the given slot */
    ExecCopySlot(slot, result);

    return true;
}

/*
 * clickhouseExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
clickhouseExplainForeignScan(ForeignScanState* node, ExplainState* es) {
    List* fdw_private;
    char* sql;
    char* relations;

    fdw_private = ((ForeignScan*)node->ss.ps.plan)->fdw_private;

    /*
     * Add names of relation handled by the foreign scan when the scan is a
     * join
     */
    if (list_length(fdw_private) > FdwScanPrivateRelations) {
        relations = strVal(list_nth(fdw_private, FdwScanPrivateRelations));
        ExplainPropertyText("Relations", relations, es);
    }

    /*
     * Add remote query, when VERBOSE option is specified.
     */
    if (es->verbose) {
        sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
        ExplainPropertyText("Remote SQL", sql, es);
    }

    if (es->timing && time_used > 0) {
        ExplainPropertyFloat("FDW Time", "ms", time_used, 3, es);
    }
}

/*
 * estimate_path_cost_size
 *		Get cost and size estimates for a foreign scan on given foreign relation
 *		either a base relation or a join between foreign relations or an upper
 *		relation containing foreign relations.
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 *
 * The function returns the cost and size estimates in p_row, p_width,
 * p_startup_cost and p_total_cost variables.
 */
static void
estimate_path_cost_size(
    double* p_rows,
    int* p_width,
    Cost* p_startup_cost,
    Cost* p_total_cost,
    double coef
) {
    /* Make pushdown paths attractive to the planner */
    *p_rows         = 1000;
    *p_width        = 32;
    *p_startup_cost = 1.0;
    *p_total_cost   = 5.0 + coef;
}

/*
 * create_foreign_modify
 *		Construct an execution state of a foreign insert
 *		operation
 */
static CHFdwModifyState*
create_foreign_modify(
    EState* estate,
    RangeTblEntry* rte,
    ResultRelInfo* rri,
    CmdType operation,
    Plan* subplan,
    char* query,
    List* target_attrs,
    char* table_name
) {
    CHFdwModifyState* fmstate;
    Oid userid;
    ForeignTable* table;
    UserMapping* user;
    MemoryContext old_mcxt;
    Relation rel = rri->ri_RelationDesc;
    ch_query q   = new_query(query, 0, NULL, RelationGetDescr(rel), target_attrs);

    /* Begin constructing CHFdwModifyState. */
    fmstate      = (CHFdwModifyState*)palloc0(sizeof(CHFdwModifyState));
    fmstate->rel = rel;

    /*
     * Identify which user to do the remote access as. This should match what
     * ExecCheckRTEPerms() does.
     */
#if PG_VERSION_NUM >= 160000
    userid = ExecGetResultRelCheckAsUser(rri, estate);
#else
    userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
#endif

    /* Get info about foreign table. */
    table = GetForeignTable(RelationGetRelid(rel));
    user  = GetUserMapping(userid, table->serverid);

    /* make a connection and prepare an insertion state */
    fmstate->conn = chfdw_get_connection(user);

    old_mcxt       = MemoryContextSwitchTo(PortalContext);
    fmstate->state = fmstate->conn.methods->prepare_insert(
        fmstate->conn.conn, rri, target_attrs, &q, table_name
    );
    MemoryContextSwitchTo(old_mcxt);

    /* Create context for per-query temp workspace. */
    fmstate->temp_cxt = AllocSetContextCreate(
        estate->es_query_cxt, "pg_clickhouse temporary data", ALLOCSET_SMALL_SIZES
    );

    /* Set up remote query information. */
    fmstate->query = query;
    return fmstate;
}

/*
 * finish_foreign_modify
 *		Release resources for a foreign insert/delete operation
 */
static void
finish_foreign_modify(CHFdwModifyState* fmstate) {
    Assert(fmstate != NULL);
    memset(&fmstate->conn, 0, sizeof(fmstate->conn));
}

/*
 * Prepare for processing of parameters used in remote query.
 */
static void
prepare_query_params(
    PlanState* node,
    List* fdw_exprs,
    int numParams,
    Oid** param_oids,
    List** param_exprs,
    const char*** param_values
) {
    int i;
    ListCell* lc;

    Assert(numParams > 0);

    /* Prepare for output conversion of parameters used in remote query. */
    *param_oids = (Oid*)palloc0(sizeof(Oid) * numParams);

    i = 0;
    foreach (lc, fdw_exprs) {
        Node* param_expr = (Node*)lfirst(lc);

        (*param_oids)[i] = exprType(param_expr);
        i++;
    }

    /*
     * Prepare remote-parameter expressions for evaluation. (Note: in
     * practice, we expect that all these expressions will be just Params, so
     * we could possibly do something more efficient than using the full
     * expression-eval machinery for this. But probably there would be little
     * benefit, and it'd require pg_clickhouse to know more than is desirable
     * about Param evaluation.)
     */
    *param_exprs = ExecInitExprList(fdw_exprs, node);

    /* Allocate buffer for text form of query parameters. */
    *param_values = (const char**)palloc0(numParams * sizeof(char*));
}

/*
 * Construct array of query parameter values in text format.
 */
static void
process_query_params(
    ExprContext* econtext,
    Oid* param_oids,
    List* param_exprs,
    const char** param_values
) {
    int i;
    ListCell* lc;

    i = 0;
    foreach (lc, param_exprs) {
        ExprState* expr_state = (ExprState*)lfirst(lc);
        Datum expr_value;
        bool isNull;

        /* Evaluate the parameter expression */
        expr_value = ExecEvalExpr(expr_state, econtext, &isNull);

        /*
         * Get string representation of each parameter value by invoking
         * type-specific output function, unless the value is null.
         */
        if (isNull) {
            param_values[i] = NULL;
        } else {
            param_values[i] = chfdw_datum_to_ch_literal(expr_value, param_oids[i]);
        }

        i++;
    }
}

/*
 * clickhouseAnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
clickhouseAnalyzeForeignTable(
    Relation relation,
    AcquireSampleRowsFunc* func,
    BlockNumber* totalpages
) {
    *func = clickhouseAcquireSampleRowsFunc;
    return true;
}

/*
 * Acquire a random sample of rows from foreign table managed by pg_clickhouse.
 * Currently a no-op.
 */
static int
clickhouseAcquireSampleRowsFunc(
    Relation relation,
    int elevel,
    HeapTuple* rows,
    int targrows,
    double* totalrows,
    double* totaldeadrows
) {
    /*
     * TODO: Consider using the SAMPLE clause:
     * https://clickhouse.com/docs/sql-reference/statements/select/sample
     */
    return 0;
}

static bool
is_simple_join_clause(Expr* expr) {
    if (IsA(expr, RestrictInfo)) {
        expr = ((RestrictInfo*)expr)->clause;
    }

    if (IsA(expr, OpExpr)) {
        OpExpr* opexpr = (OpExpr*)expr;

        if (chfdw_is_equal_op(opexpr->opno) == CH_OP_EQ &&
            list_length(opexpr->args) == 2 && IsA(list_nth(opexpr->args, 0), Var) &&
            IsA(list_nth(opexpr->args, 1), Var)) {
            return true;
        }
    }
    return false;
}

static List*
extract_join_equals(List* conds, List** to) {
    ListCell* lc;
    List* res = NIL;

    foreach (lc, conds) {
        Expr* expr = (Expr*)lfirst(lc);

        if (is_simple_join_clause(expr)) {
            *to = lappend(*to, expr);
        } else {
            res = lappend(res, expr);
        }
    }
    return res;
}

/*
 * Check if reltarget is safe for semi-join pushdown. Returns false if the
 * target references columns from the inner relation that aren't in outer
 * relation.
 */
static bool
semijoin_target_ok(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    RelOptInfo* outerrel,
    RelOptInfo* innerrel
) {
    List* vars;
    ListCell* lc;
    bool ok = true;

    vars = pull_var_clause(
        (Node*)joinrel->reltarget->exprs,
        PVC_RECURSE_AGGREGATES | PVC_RECURSE_WINDOWFUNCS | PVC_INCLUDE_PLACEHOLDERS
    );

    foreach (lc, vars) {
        Var* var = (Var*)lfirst(lc);

        if (!IsA(var, Var)) {
            continue;
        }

        if (bms_is_member(var->varno, innerrel->relids) &&
            !bms_is_member(var->varno, outerrel->relids)) {
            ok = false;
            break;
        }
    }

    list_free(vars);
    return ok;
}

/*
 * Assess whether the join between inner and outer relations can be pushed down
 * to the foreign server. As a side effect, save information we obtain in this
 * function to CHFdwRelationInfo passed in.
 */
static bool
foreign_join_ok(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    JoinType jointype,
    RelOptInfo* outerrel,
    RelOptInfo* innerrel,
    JoinPathExtraData* extra
) {
    CHFdwRelationInfo* fpinfo;
    CHFdwRelationInfo* fpinfo_o;
    CHFdwRelationInfo* fpinfo_i;
    ListCell* lc;
    List* joinclauses;

    /*
     * We support the most common joins, but list those we don't yet support
     * for clarity.
     */
    switch (jointype) {
    case JOIN_INNER:
    case JOIN_LEFT:
    case JOIN_RIGHT:
    case JOIN_FULL:
        break;
    case JOIN_SEMI: /* deparses to LEFT SEMI JOIN */
    case JOIN_ANTI: /* deparses to LEFT ANTI JOIN */
        /* Semi/anti-join target can only reference the outer relation. */
        if (!semijoin_target_ok(root, joinrel, outerrel, innerrel)) {
            return false;
        }
        break;
    // case JOIN_RIGHT_SEMI: /* Added in Postgres 18. */
    // case JOIN_RIGHT_ANTI: /* Added in Postgres 16. */
    case JOIN_UNIQUE_OUTER:
    case JOIN_UNIQUE_INNER:
    default:
        return false;
    }

    /*
     * If either of the joining relations is marked as unsafe to pushdown, the
     * join cannot be pushed down.
     */
    fpinfo   = (CHFdwRelationInfo*)joinrel->fdw_private;
    fpinfo_o = (CHFdwRelationInfo*)outerrel->fdw_private;
    fpinfo_i = (CHFdwRelationInfo*)innerrel->fdw_private;
    if (!fpinfo_o || !fpinfo_o->pushdown_safe || !fpinfo_i ||
        !fpinfo_i->pushdown_safe) {
        return false;
    }

    /*
     * A SEMI/ANTI joinrel used as the input for a further join would deparse
     * as an inline nested join, which ClickHouse cannot parse, and the
     * subquery-wrapping escape hatch requires reltarget coverage that
     * SEMI/ANTI inputs do not guarantee. Keep such composites local; the
     * SEMI/ANTI join itself can still push down as the scan's top rel.
     */
    if ((IS_JOIN_REL(outerrel) &&
         (fpinfo_o->jointype == JOIN_SEMI || fpinfo_o->jointype == JOIN_ANTI)) ||
        (IS_JOIN_REL(innerrel) &&
         (fpinfo_i->jointype == JOIN_SEMI || fpinfo_i->jointype == JOIN_ANTI))) {
        return false;
    }

    /*
     * If joining relations have local conditions, those conditions are
     * required to be applied before joining the relations. Hence the join
     * cannot be pushed down.
     */
    if (fpinfo_o->local_conds || fpinfo_i->local_conds) {
        return false;
    }

    /*
     * Merge FDW options. We might be tempted to do this after we have deemed
     * the foreign join to be OK. But we must do this beforehand so that we
     * know which quals can be evaluated on the foreign server, which might
     * depend on shippable_extensions.
     */
    fpinfo->server = fpinfo_o->server;
    merge_fdw_options(fpinfo, fpinfo_o, fpinfo_i);

    /*
     * Separate restrict list into join quals and pushed-down (other) quals.
     *
     * Join quals belonging to an outer join must all be shippable, else we
     * cannot execute the join remotely. Add such quals to 'joinclauses'.
     *
     * Add other quals to fpinfo->remote_conds if they are shippable, else to
     * fpinfo->local_conds. In an inner join it's okay to execute conditions
     * either locally or remotely; the same is true for pushed-down conditions
     * at an outer join.
     *
     * Note we might return failure after having already scribbled on
     * fpinfo->remote_conds and fpinfo->local_conds. That's okay because we
     * won't consult those lists again if we deem the join unshippable.
     */
    joinclauses = NIL;
    foreach (lc, extra->restrictlist) {
        RestrictInfo* rinfo = lfirst_node(RestrictInfo, lc);
        bool is_remote_clause =
            chfdw_is_foreign_expr(root, joinrel, rinfo->clause, false);

        if (IS_OUTER_JOIN(jointype) && !RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids)) {
            if (!is_remote_clause) {
                return false;
            }
            joinclauses = lappend(joinclauses, rinfo);
        } else {
            if (is_remote_clause) {
                fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
            } else {
                fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
            }
        }
    }

    /*
     * deparseExplicitTargetList() isn't smart enough to handle anything other
     * than a Var. In particular, if there's some PlaceHolderVar that would
     * need to be evaluated within this join tree (because there's an upper
     * reference to a quantity that may go to NULL as a result of an outer
     * join), then we can't try to push the join down because we'll fail when
     * we get to deparseExplicitTargetList(). However, a PlaceHolderVar that
     * needs to be evaluated *at the top* of this join tree is OK, because we
     * can do that locally after fetching the results from the remote side.
     */
    foreach (lc, root->placeholder_list) {
        PlaceHolderInfo* phinfo = lfirst(lc);
        Relids relids;

        /* PlaceHolderInfo refers to parent relids, not child relids. */
        relids = IS_OTHER_REL(joinrel) ? joinrel->top_parent_relids : joinrel->relids;

        if (bms_is_subset(phinfo->ph_eval_at, relids) &&
            bms_nonempty_difference(relids, phinfo->ph_eval_at)) {
            return false;
        }
    }

    /* Save the join clauses, for later use. */
    fpinfo->joinclauses = joinclauses;

    fpinfo->outerrel = outerrel;
    fpinfo->innerrel = innerrel;
    fpinfo->jointype = jointype;

    /*
     * By default, both the input relations are not required to be deparsed as
     * subqueries, but there might be some relations covered by the input
     * relations that are required to be deparsed as subqueries, so save the
     * relids of those relations for later use by the deparser.
     */
    fpinfo->make_outerrel_subquery = false;
    fpinfo->make_innerrel_subquery = false;
    Assert(bms_is_subset(fpinfo_o->lower_subquery_rels, outerrel->relids));
    Assert(bms_is_subset(fpinfo_i->lower_subquery_rels, innerrel->relids));
    fpinfo->lower_subquery_rels =
        bms_union(fpinfo_o->lower_subquery_rels, fpinfo_i->lower_subquery_rels);

    /*
     * Pull the other remote conditions from the joining relations into join
     * clauses or other remote clauses (remote_conds) of this relation
     * wherever possible. This avoids building subqueries at every join step.
     *
     * For an inner join, clauses from both the relations are added to the
     * other remote clauses. For LEFT and RIGHT OUTER join, the clauses from
     * the outer side are added to remote_conds since those can be evaluated
     * after the join is evaluated. The clauses from inner side are added to
     * the joinclauses, since they need to be evaluated while constructing the
     * join.
     *
     * For a FULL OUTER JOIN, the other clauses from either relation cannot be
     * added to the joinclauses or remote_conds, since each relation acts as
     * an outer relation for the other.
     *
     * The joining sides cannot have local conditions, thus no need to test
     * shippability of the clauses being pulled up.
     */
    switch (jointype) {
    case JOIN_INNER:
        fpinfo->remote_conds =
            list_concat(fpinfo->remote_conds, list_copy(fpinfo_i->remote_conds));
        fpinfo->remote_conds =
            list_concat(fpinfo->remote_conds, list_copy(fpinfo_o->remote_conds));

        /*
         * For an inner join, some restrictions can be treated alike.
         * Treating the pushed down conditions as join conditions allows a
         * top level full outer join to be deparsed without requiring
         * subqueries.
         */
        Assert(!fpinfo->joinclauses);
        fpinfo->remote_conds =
            extract_join_equals(fpinfo->remote_conds, &fpinfo->joinclauses);
        break;

    case JOIN_LEFT:
        fpinfo->joinclauses =
            list_concat(fpinfo->joinclauses, list_copy(fpinfo_i->remote_conds));
        fpinfo->remote_conds =
            list_concat(fpinfo->remote_conds, list_copy(fpinfo_o->remote_conds));
        break;

    case JOIN_RIGHT:
        fpinfo->joinclauses =
            list_concat(fpinfo->joinclauses, list_copy(fpinfo_o->remote_conds));
        fpinfo->remote_conds =
            list_concat(fpinfo->remote_conds, list_copy(fpinfo_i->remote_conds));
        break;

    case JOIN_SEMI:
    case JOIN_ANTI:

        /*
         * For semi/anti-join, inner's conditions go to joinclauses (ON),
         * outer's conditions go to remote_conds (WHERE). Extract join key
         * equalities to joinclauses for the ON clause.
         */
        fpinfo->joinclauses =
            list_concat(fpinfo->joinclauses, list_copy(fpinfo_i->remote_conds));
        fpinfo->remote_conds =
            list_concat(fpinfo->remote_conds, list_copy(fpinfo_o->remote_conds));
        fpinfo->remote_conds =
            extract_join_equals(fpinfo->remote_conds, &fpinfo->joinclauses);

        /*
         * Subquery-wrapping a join-typed input would require its
         * reltarget to cover every Var the ON clause references, which
         * does not hold for SEMI/ANTI inputs (join-only columns are not
         * propagated upstream). Until the deparser can widen the wrapped
         * subquery's targetlist, refuse ANTI pushdown when either input
         * is itself a join and fall back to local execution. SEMI keeps
         * its historical behavior.
         */
        if (jointype == JOIN_ANTI && (IS_JOIN_REL(outerrel) || IS_JOIN_REL(innerrel))) {
            return false;
        }
        break;

    case JOIN_FULL:

        /*
         * In this case, if any of the input relations has conditions, we
         * need to deparse that relation as a subquery so that the
         * conditions can be evaluated before the join. Remember it in the
         * fpinfo of this relation so that the deparser can take
         * appropriate action. Also, save the relids of base relations
         * covered by that relation for later use by the deparser.
         */
        if (fpinfo_o->remote_conds) {
            fpinfo->make_outerrel_subquery = true;
            fpinfo->lower_subquery_rels =
                bms_add_members(fpinfo->lower_subquery_rels, outerrel->relids);
        }
        if (fpinfo_i->remote_conds) {
            fpinfo->make_innerrel_subquery = true;
            fpinfo->lower_subquery_rels =
                bms_add_members(fpinfo->lower_subquery_rels, innerrel->relids);
        }
        break;

    default:
        /* Should not happen, we have just checked this above */
        elog(ERROR, "unsupported join type %d", jointype);
    }

    /*
     * ClickHouse SEMI/ANTI JOINs require at least one equi-join key in the ON
     * clause. Reject when the joinclauses have no simple equality referencing
     * both sides (e.g., uncorrelated EXISTS).
     */
    if (jointype == JOIN_SEMI || jointype == JOIN_ANTI) {
        bool has_equi_key = false;

        foreach (lc, fpinfo->joinclauses) {
            RestrictInfo* rinfo = lfirst_node(RestrictInfo, lc);

            if (is_simple_join_clause((Expr*)rinfo)) {
                has_equi_key = true;
                break;
            }
        }
        if (!has_equi_key) {
            return false;
        }
    }

    /* Mark that this join can be pushed down safely */
    fpinfo->pushdown_safe = true;

    /* Get user mapping */
    if (fpinfo->use_remote_estimate) {
        if (fpinfo_o->use_remote_estimate) {
            fpinfo->user = fpinfo_o->user;
        } else {
            fpinfo->user = fpinfo_i->user;
        }
    } else {
        fpinfo->user = NULL;
    }

    /*
     * Set cached relation costs to some negative value, so that we can detect
     * when they are set to some sensible costs, during one (usually the
     * first) of the calls to estimate_path_cost_size().
     */
    fpinfo->rel_startup_cost = -1;
    fpinfo->rel_total_cost   = -1;

    /*
     * Set the string describing this join relation to be used in EXPLAIN
     * output of corresponding ForeignScan.
     */
    fpinfo->relation_name = makeStringInfo();
    appendStringInfo(
        fpinfo->relation_name,
        "(%s) %s JOIN (%s)",
        fpinfo_o->relation_name->data,
        chfdw_get_jointype_name(fpinfo->jointype),
        fpinfo_i->relation_name->data
    );

    /*
     * Set the relation index. This is defined as the position of this joinrel
     * in the join_rel_list list plus the length of the rtable list. Note that
     * since this joinrel is at the end of the join_rel_list list when we are
     * called, we can get the position by list_length.
     */
    Assert(fpinfo->relation_index == 0); /* shouldn't be set yet */
    fpinfo->relation_index =
        list_length(root->parse->rtable) + list_length(root->join_rel_list);

    return true;
}

static void
add_paths_with_pathkeys_for_rel(PlannerInfo* root, RelOptInfo* rel, Path* epq_path) {
    List* useful_pathkeys_list = NIL; /* List of all pathkeys */
    ListCell* lc;

    useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel);

    /* Create one path for each set of pathkeys we found above. */
    foreach (lc, useful_pathkeys_list) {
        double rows;
        int width;
        Cost startup_cost;
        Cost total_cost;
        List* useful_pathkeys = lfirst(lc);
        Path* sorted_epq_path;

        estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.5);

        /*
         * The EPQ path must be at least as well sorted as the path itself, in
         * case it gets used as input to a mergejoin.
         */
        sorted_epq_path = epq_path;
        if (sorted_epq_path != NULL &&
            !pathkeys_contained_in(useful_pathkeys, sorted_epq_path->pathkeys)) {
            sorted_epq_path = (Path*)create_sort_path(
                root, rel, sorted_epq_path, useful_pathkeys, -1.0
            );
        }

        if (IS_SIMPLE_REL(rel)) {
            add_path(
                rel,
                (Path*)create_foreignscan_path(
                    root,
                    rel,
                    NULL,
                    rows,
#if PG_VERSION_NUM >= 180000
                    0,
#endif
                    startup_cost,
                    total_cost,
                    useful_pathkeys,
                    NULL,
                    sorted_epq_path,
#if PG_VERSION_NUM >= 170000
                    NIL,
#endif
                    NIL
                )
            );
        } else {
            add_path(
                rel,
                (Path*)create_foreign_join_path(
                    root,
                    rel,
                    NULL,
                    rows,
#if PG_VERSION_NUM >= 180000
                    0,
#endif
                    startup_cost,
                    total_cost,
                    useful_pathkeys,
                    NULL,
                    sorted_epq_path,
#if PG_VERSION_NUM >= 170000
                    NIL,
#endif
                    NIL
                )
            );
        }
    }
}

/*
 * Merge FDW options from input relations into a new set of options for a join
 * or an upper rel.
 *
 * For a join relation, FDW-specific information about the inner and outer
 * relations is provided using fpinfo_i and fpinfo_o. For an upper relation,
 * fpinfo_o provides the information for the input relation; fpinfo_i is
 * expected to NULL.
 */
static void
merge_fdw_options(
    CHFdwRelationInfo* fpinfo,
    const CHFdwRelationInfo* fpinfo_o,
    const CHFdwRelationInfo* fpinfo_i
) {
    /* We must always have fpinfo_o. */
    Assert(fpinfo_o);

    /* fpinfo_i may be NULL, but if present the servers must both match. */
    Assert(!fpinfo_i || fpinfo_i->server->serverid == fpinfo_o->server->serverid);

    /*
     * Copy the server specific FDW options. (For a join, both relations come
     * from the same server, so the server options should have the same value
     * for both relations.)
     */
    fpinfo->fdw_startup_cost     = fpinfo_o->fdw_startup_cost;
    fpinfo->fdw_tuple_cost       = fpinfo_o->fdw_tuple_cost;
    fpinfo->shippable_extensions = fpinfo_o->shippable_extensions;
    fpinfo->use_remote_estimate  = fpinfo_o->use_remote_estimate;
    fpinfo->fetch_size           = fpinfo_o->fetch_size;

    /* Merge the table level options from either side of the join. */
    if (fpinfo_i) {
        /*
         * We'll prefer to use remote estimates for this join if any table
         * from either side of the join is using remote estimates. This is
         * most likely going to be preferred since they're already willing to
         * pay the price of a round trip to get the remote EXPLAIN. In any
         * case it's not entirely clear how we might otherwise handle this
         * best.
         */
        fpinfo->use_remote_estimate =
            fpinfo_o->use_remote_estimate || fpinfo_i->use_remote_estimate;

        /*
         * Set fetch size to maximum of the joining sides, since larger joins
         * benefit from bigger batches.
         */
        fpinfo->fetch_size = Max(fpinfo_o->fetch_size, fpinfo_i->fetch_size);
    }
}

/*
 * clickhouseGetForeignJoinPaths
 *		Add possible ForeignPath to joinrel, if join is safe to push down.
 */
static void
clickhouseGetForeignJoinPaths(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    RelOptInfo* outerrel,
    RelOptInfo* innerrel,
    JoinType jointype,
    JoinPathExtraData* extra
) {
    CHFdwRelationInfo* fpinfo;
    ForeignPath* joinpath;
    double rows;
    int width;
    Cost startup_cost;
    Cost total_cost;
    Path* epq_path; /* Path to create plan to be executed when
                     * EvalPlanQual gets triggered. */

    struct timeval time1, time2;

    gettimeofday(&time1, NULL);

    /*
     * Skip if this join combination has been considered already.
     */
    if (joinrel->fdw_private) {
        return;
    }

    /*
     * Create unfinished CHFdwRelationInfo entry which is used to indicate
     * that the join relation is already considered, so that we won't waste
     * time in judging safety of join pushdown and adding the same paths again
     * if found safe. Once we know that this join can be pushed down, we fill
     * the entry.
     */
    fpinfo                = (CHFdwRelationInfo*)palloc0(sizeof(CHFdwRelationInfo));
    fpinfo->pushdown_safe = false;
    joinrel->fdw_private  = fpinfo;
    /* attrs_used is only for base relations. */
    fpinfo->attrs_used = NULL;
    epq_path           = NULL;

    if (!foreign_join_ok(root, joinrel, jointype, outerrel, innerrel, extra)) {
        /* Free path required for EPQ if we copied one; we don't need it now */
        if (epq_path) {
            pfree(epq_path);
        }

        return;
    }

    /*
     * Compute the selectivity and cost of the local_conds, so we don't have
     * to do it over again for each path. The best we can do for these
     * conditions is to estimate selectivity on the basis of local statistics.
     * The local conditions are applied after the join has been computed on
     * the remote side like quals in WHERE clause, so pass jointype as
     * JOIN_INNER.
     */
    fpinfo->local_conds_sel =
        clauselist_selectivity(root, fpinfo->local_conds, 0, JOIN_INNER, NULL);
    cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

    /*
     * If we are going to estimate costs locally, estimate the join clause
     * selectivity here while we have special join info.
     */
    if (!fpinfo->use_remote_estimate) {
        fpinfo->joinclause_sel = clauselist_selectivity(
            root, fpinfo->joinclauses, 0, fpinfo->jointype, extra->sjinfo
        );
    }

    /* Estimate costs for bare join relation */
    estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0);
    /* Now update this information in the joinrel */
    joinrel->rows             = rows;
    joinrel->reltarget->width = width;
    fpinfo->rows              = rows;
    fpinfo->width             = width;
    fpinfo->startup_cost      = startup_cost;
    fpinfo->total_cost        = total_cost;

    /*
     * Create a new join path and add it to the joinrel which represents a
     * join between foreign tables.
     */
    joinpath = create_foreign_join_path(
        root,
        joinrel,
        NULL, /* default pathtarget */
        rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        NIL, /* no pathkeys */
        NULL,
        epq_path,
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        NIL
    ); /* no fdw_private */

    /* Add generated path into joinrel by add_path(). */
    add_path(joinrel, (Path*)joinpath);

    /* Consider pathkeys for the join relation */
    add_paths_with_pathkeys_for_rel(root, joinrel, epq_path);

    gettimeofday(&time2, NULL);
    time_used += time_diff(&time1, &time2);
}

/*
 * Assess whether the aggregation, grouping and having operations can be pushed
 * down to the foreign server. As a side effect, save information we obtain in
 * this function to CHFdwRelationInfo of the input relation.
 */
static bool
foreign_grouping_ok(PlannerInfo* root, RelOptInfo* grouped_rel, Node* havingQual) {
    Query* query                = root->parse;
    CHFdwRelationInfo* fpinfo   = (CHFdwRelationInfo*)grouped_rel->fdw_private;
    PathTarget* grouping_target = grouped_rel->reltarget;
    CHFdwRelationInfo* ofpinfo;
    List* aggvars;
    ListCell* lc;
    int i;
    List* tlist = NIL;

    /* We currently don't support pushing Grouping Sets. */
    if (query->groupingSets) {
        return false;
    }

    /* Get the fpinfo of the underlying scan relation. */
    ofpinfo = (CHFdwRelationInfo*)fpinfo->outerrel->fdw_private;

    /*
     * If underlying scan relation has any local conditions, those conditions
     * are required to be applied before performing aggregation. Hence the
     * aggregate cannot be pushed down.
     */
    if (ofpinfo->local_conds) {
        return false;
    }

    /*
     * Examine grouping expressions, as well as other expressions we'd need to
     * compute, and check whether they are safe to push down to the foreign
     * server. All GROUP BY expressions will be part of the grouping target
     * and thus there is no need to search for them separately. Add grouping
     * expressions into target list which will be passed to foreign server.
     *
     * A tricky fine point is that we must not put any expression into the
     * target list that is just a foreign param (that is, something that
     * deparse.c would conclude has to be sent to the foreign server).  If we
     * do, the expression will also appear in the fdw_exprs list of the plan
     * node, and setrefs.c will get confused and decide that the fdw_exprs
     * entry is actually a reference to the fdw_scan_tlist entry, resulting in
     * a broken plan.  Somewhat oddly, it's OK if the expression contains such
     * a node, as long as it's not at top level; then no match is possible.
     */
    i = 0;
    foreach (lc, grouping_target->exprs) {
        Expr* expr  = (Expr*)lfirst(lc);
        Index sgref = get_pathtarget_sortgroupref(grouping_target, i);
        ListCell* l;

        /* Check whether this expression is part of GROUP BY clause */
        if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause)) {
            TargetEntry* tle;

            /*
             * If any GROUP BY expression is not shippable, then we cannot
             * push down aggregation to the foreign server.
             */
            if (!chfdw_is_foreign_expr(root, grouped_rel, expr, true)) {
                return false;
            }

            /*
             * If it would be a foreign param, we can't put it into the tlist,
             * so we have to fail.
             */
            if (is_foreign_param(root, grouped_rel, expr)) {
                return false;
            }

            /*
             * Pushable, so add to tlist. We need to create a TLE for this
             * expression and apply the sortgroupref to it. We cannot use
             * add_to_flat_tlist() here because that avoids making duplicate
             * entries in the tlist. If there are duplicate entries with
             * distinct sortgrouprefs, we have to duplicate that situation in
             * the output tlist.
             */
            tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
            tle->ressortgroupref = sgref;
            tlist                = lappend(tlist, tle);
        } else {
            /*
             * Non-grouping expression we need to compute. Is it shippable?
             */
            if (chfdw_is_foreign_expr(root, grouped_rel, expr, true)) {
                /* Yes, so add to tlist as-is; OK to suppress duplicates */
                tlist = add_to_flat_tlist(tlist, list_make1(expr));
            } else {
                /* Not pushable as a whole; extract its Vars and aggregates */
                aggvars = pull_var_clause((Node*)expr, PVC_INCLUDE_AGGREGATES);

                /*
                 * If any aggregate expression is not shippable, then we
                 * cannot push down aggregation to the foreign server.
                 */
                if (!chfdw_is_foreign_expr(root, grouped_rel, (Expr*)aggvars, true)) {
                    return false;
                }

                /*
                 * Add aggregates, if any, into the targetlist. Plain Vars
                 * outside an aggregate can be ignored, because they should be
                 * either same as some GROUP BY column or part of some GROUP
                 * BY expression. In either case, they are already part of the
                 * targetlist and thus no need to add them again. In fact
                 * including plain Vars in the tlist when they do not match a
                 * GROUP BY column would cause the foreign server to complain
                 * that the shipped query is invalid.
                 */
                foreach (l, aggvars) {
                    Expr* first_expr = (Expr*)lfirst(l);

                    if (IsA(first_expr, Aggref)) {
                        tlist = add_to_flat_tlist(tlist, list_make1(first_expr));
                    }
                }
            }
        }

        i++;
    }

    /*
     * Classify the pushable and non-pushable HAVING clauses and save them in
     * remote_conds and local_conds of the grouped rel's fpinfo.
     */
    if (havingQual) {
        ListCell* having_lc;

        foreach (having_lc, (List*)havingQual) {
            Expr* expr = (Expr*)lfirst(having_lc);
            RestrictInfo* rinfo;

            /*
             * Currently, the core code doesn't wrap havingQuals in
             * RestrictInfos, so we must make our own.
             */
            Assert(!IsA(expr, RestrictInfo));
            rinfo = make_restrictinfo(
#if PG_VERSION_NUM >= 140000
                root,
#endif
                expr,
                true,
                false,
#if PG_VERSION_NUM >= 160000
                false,
#endif
                false,
                root->qual_security_level,
                grouped_rel->relids,
                NULL,
                NULL
            );
            if (chfdw_is_foreign_expr(root, grouped_rel, expr, false)) {
                fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
            } else {
                fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
            }
        }
    }

    /*
     * If there are any local conditions, pull Vars and aggregates from it and
     * check whether they are safe to pushdown or not.
     */
    if (fpinfo->local_conds) {
        List* local_aggvars = NIL;
        ListCell* agg_lc;

        foreach (agg_lc, fpinfo->local_conds) {
            RestrictInfo* rinfo = lfirst_node(RestrictInfo, agg_lc);

            local_aggvars = list_concat(
                local_aggvars,
                pull_var_clause((Node*)rinfo->clause, PVC_INCLUDE_AGGREGATES)
            );
        }

        foreach (agg_lc, local_aggvars) {
            Expr* expr = (Expr*)lfirst(agg_lc);

            /*
             * If aggregates within local conditions are not safe to push
             * down, then we cannot push down the query. Vars are already part
             * of GROUP BY clause which are checked above, so no need to
             * access them again here.
             */
            if (IsA(expr, Aggref)) {
                if (!chfdw_is_foreign_expr(root, grouped_rel, expr, true)) {
                    return false;
                }

                tlist = add_to_flat_tlist(tlist, list_make1(expr));
            }
        }
    }

    /* Store generated targetlist */
    fpinfo->grouped_tlist = tlist;

    /* Safe to pushdown */
    fpinfo->pushdown_safe = true;

    /*
     * Set cached relation costs to some negative value, so that we can detect
     * when they are set to some sensible costs, during one (usually the
     * first) of the calls to estimate_path_cost_size().
     */
    fpinfo->rel_startup_cost = -1;
    fpinfo->rel_total_cost   = -1;

    /*
     * Set the string describing this grouped relation to be used in EXPLAIN
     * output of corresponding ForeignScan.
     */
    fpinfo->relation_name = makeStringInfo();
    appendStringInfo(
        fpinfo->relation_name, "Aggregate on (%s)", ofpinfo->relation_name->data
    );

    return true;
}

static bool
setop_exec_param_walker(Node* node, void* context) {
    (void)context;

    if (node == NULL) {
        return false;
    }
    if (IsA(node, Param) && ((Param*)node)->paramkind == PARAM_EXEC) {
        return true;
    }
    return expression_tree_walker(node, setop_exec_param_walker, context);
}

static bool
setop_restrictinfos_require_local_gate(List* conditions) {
    ListCell* lc;

    foreach (lc, conditions) {
        RestrictInfo* rinfo = lfirst_node(RestrictInfo, lc);

        if (rinfo->pseudoconstant ||
            setop_exec_param_walker((Node*)rinfo->clause, NULL)) {
            return true;
        }
    }
    return false;
}

/*
 * Result gating quals and InitPlan parameters are added above a scan only
 * after path selection. A combined remote UNION drops its input plan, so
 * reject such arms before offering that path.
 */
static bool
setop_relation_requires_local_gate(RelOptInfo* rel) {
    CHFdwRelationInfo* fpinfo;

    if (rel == NULL || rel->fdw_private == NULL) {
        return true;
    }

    fpinfo = (CHFdwRelationInfo*)rel->fdw_private;
    if (setop_restrictinfos_require_local_gate(rel->baserestrictinfo) ||
        setop_restrictinfos_require_local_gate(fpinfo->remote_conds) ||
        setop_restrictinfos_require_local_gate(fpinfo->local_conds) ||
        setop_restrictinfos_require_local_gate(fpinfo->joinclauses) ||
        setop_exec_param_walker((Node*)rel->reltarget->exprs, NULL) ||
        setop_exec_param_walker((Node*)fpinfo->grouped_tlist, NULL) ||
        setop_exec_param_walker((Node*)fpinfo->final_remote_exprs, NULL)) {
        return true;
    }

    if (fpinfo->outerrel != NULL && fpinfo->outerrel != rel &&
        setop_relation_requires_local_gate(fpinfo->outerrel)) {
        return true;
    }
    if (fpinfo->innerrel != NULL && fpinfo->innerrel != rel &&
        setop_relation_requires_local_gate(fpinfo->innerrel)) {
        return true;
    }
    return false;
}

/*
 * DISTINCT must use the same equality semantics on both servers. Keep this
 * list intentionally narrow; notably, bpchar ignores trailing spaces in
 * PostgreSQL while ClickHouse String does not.
 */
static bool
distinct_expr_is_safe(Node* expr) {
    Oid type;

    if (expr == NULL) {
        return false;
    }
    type = exprType(expr);
    switch (type) {
    case BOOLOID:
    case INT2OID:
    case INT4OID:
    case INT8OID:
    case DATEOID:
    case BYTEAOID:
    case UUIDOID:
        return true;
    case VARCHAROID:
        if (exprTypmod(expr) >= 0) {
            return false;
        }
        /* fall through */
    case TEXTOID: {
        Oid collation = exprCollation(expr);

        return !OidIsValid(collation) || get_collation_isdeterministic(collation);
    }
    default:
        return false;
    }
}

static bool
distinct_target_is_safe(PathTarget* target) {
    ListCell* lc;

    if (target == NULL || target->exprs == NIL) {
        return false;
    }

    foreach (lc, target->exprs) {
        if (!distinct_expr_is_safe((Node*)lfirst(lc))) {
            return false;
        }
    }
    return true;
}

typedef struct ChFdwSetopGroupingValidationState {
    List* setop_tlist;
    int setop_columns;
} ChFdwSetopGroupingValidationState;

static TargetEntry*
setop_grouping_var_target(
    const Var* var,
    const ChFdwSetopGroupingValidationState* state
) {
    ListCell* lc;

    foreach (lc, state->setop_tlist) {
        TargetEntry* tle = lfirst_node(TargetEntry, lc);

        if (!tle->resjunk && equal(var, tle->expr)) {
            return tle;
        }
    }
    if (var->varno == 0 && var->varattno > 0 && var->varattno <= state->setop_columns) {
        return get_tle_by_resno(state->setop_tlist, var->varattno);
    }
    return NULL;
}

static bool
setop_grouping_expr_walker(Node* node, void* context) {
    ChFdwSetopGroupingValidationState* state =
        (ChFdwSetopGroupingValidationState*)context;

    if (node == NULL) {
        return false;
    }
    if (IsA(node, SubPlan) ||
        (IsA(node, Param) && ((Param*)node)->paramkind == PARAM_EXEC)) {
        return true;
    }
    if (IsA(node, Var)) {
        Var* var         = (Var*)node;
        TargetEntry* tle = setop_grouping_var_target(var, state);

        return var->varlevelsup != 0 || tle == NULL ||
               exprType((Node*)var) != exprType((Node*)tle->expr) ||
               exprTypmod((Node*)var) != exprTypmod((Node*)tle->expr) ||
               exprCollation((Node*)var) != exprCollation((Node*)tle->expr);
    }
    return expression_tree_walker(node, setop_grouping_expr_walker, context);
}

static bool
setop_grouping_distinct_walker(Node* node, void* context) {
    (void)context;

    if (node == NULL) {
        return false;
    }
    if (IsA(node, Aggref)) {
        Aggref* agg = (Aggref*)node;
        ListCell* lc;

        foreach (lc, agg->aggdistinct) {
            SortGroupClause* sort_clause = lfirst_node(SortGroupClause, lc);
            TargetEntry* tle =
                get_sortgroupref_tle(sort_clause->tleSortGroupRef, agg->args);

            if (!distinct_expr_is_safe((Node*)tle->expr)) {
                return true;
            }
        }
    }
    return expression_tree_walker(node, setop_grouping_distinct_walker, context);
}

static bool
setop_grouping_target_is_safe(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* grouped_rel,
    List* setop_tlist
) {
    ChFdwSetopGroupingValidationState state = {
        .setop_tlist   = setop_tlist,
        .setop_columns = list_length(setop_tlist),
    };
    PathTarget* target = grouped_rel->reltarget;
    ListCell* lc;
    int index = 0;

    (void)input_rel;

    if (setop_tlist == NIL || target == NULL || target->exprs == NIL ||
        setop_grouping_expr_walker((Node*)target->exprs, &state) ||
        setop_grouping_distinct_walker((Node*)target->exprs, NULL)) {
        return false;
    }

    foreach (lc, target->exprs) {
        Node* expr    = (Node*)lfirst(lc);
        Index sortref = get_pathtarget_sortgroupref(target, index++);

        if (sortref != 0 &&
            get_sortgroupref_clause_noerr(sortref, root->parse->groupClause) != NULL &&
            !distinct_expr_is_safe(expr)) {
            return false;
        }
    }
    return true;
}

static bool
setop_relation_uses_default_engine(RelOptInfo* rel) {
    CHFdwRelationInfo* fpinfo;

    if (rel == NULL || rel->fdw_private == NULL) {
        return false;
    }
    fpinfo = (CHFdwRelationInfo*)rel->fdw_private;
    if (fpinfo->ch_table_engine != CH_DEFAULT) {
        return false;
    }
    if (fpinfo->outerrel != NULL && fpinfo->outerrel != rel &&
        !setop_relation_uses_default_engine(fpinfo->outerrel)) {
        return false;
    }
    if (fpinfo->innerrel != NULL && fpinfo->innerrel != rel &&
        !setop_relation_uses_default_engine(fpinfo->innerrel)) {
        return false;
    }
    return true;
}

static bool
setop_foreign_path_ok(const ForeignPath* path, void* arg) {
    ChFdwSetopValidationState* state = (ChFdwSetopValidationState*)arg;
    RelOptInfo* rel                  = path->path.parent;
    CHFdwRelationInfo* fpinfo;
    ListCell* lc;

    if (rel == NULL || rel->fdwroutine == NULL ||
        rel->fdwroutine->GetForeignPlan != clickhouseGetForeignPlan ||
        rel->fdw_private == NULL) {
        return false;
    }

    fpinfo = (CHFdwRelationInfo*)rel->fdw_private;
    if (!fpinfo->pushdown_safe || fpinfo->local_conds != NIL ||
        (path->fdw_outerpath != NULL && !fpinfo->is_setop) ||
        setop_relation_requires_local_gate(rel)) {
        return false;
    }

    /*
     * Base and join scans only project Vars remotely. Upper scans have
     * already built and validated an explicit remote target list.
     */
    if (!IS_UPPER_REL(rel)) {
        foreach (lc, rel->reltarget->exprs) {
            Var* var;

            if (!IsA(lfirst(lc), Var)) {
                return false;
            }
            var = (Var*)lfirst(lc);
            if (var->varlevelsup != 0 || var->varattno <= 0) {
                return false;
            }
        }
    }

    state->fetch_size = Max(state->fetch_size, fpinfo->fetch_size);
    return true;
}

static bool
is_flattened_union_all_relation(PlannerInfo* root, RelOptInfo* input_rel) {
    Query* parse = root->parse;
    RangeTblEntry* rte;
    ListCell* lc;
    int children = 0;

    if (input_rel == NULL || input_rel->reloptkind != RELOPT_BASEREL ||
        input_rel->rtekind != RTE_SUBQUERY || input_rel->relid <= 0 ||
        input_rel->baserestrictinfo != NIL ||
        !bms_is_empty(input_rel->lateral_relids) || parse->jointree == NULL ||
        list_length(parse->jointree->fromlist) != 1 ||
        !IsA(linitial(parse->jointree->fromlist), RangeTblRef) ||
        (int64)((RangeTblRef*)linitial(parse->jointree->fromlist))->rtindex !=
            (int64)input_rel->relid) {
        return false;
    }

    rte = planner_rt_fetch(input_rel->relid, root);
    if (rte->rtekind != RTE_SUBQUERY || !rte->inh || /* codespell:ignore inh */
        rte->securityQuals != NIL) {
        return false;
    }

    foreach (lc, root->append_rel_list) {
        AppendRelInfo* appinfo = lfirst_node(AppendRelInfo, lc);

        if (appinfo->parent_relid != input_rel->relid) {
            continue;
        }
        if (OidIsValid(appinfo->parent_reltype) || OidIsValid(appinfo->child_reltype) ||
            OidIsValid(appinfo->parent_reloid)) {
            return false;
        }
        children++;
    }

    return children >= 2;
}

static bool
is_flattened_union_all(PlannerInfo* root, RelOptInfo* input_rel) {
    Query* parse = root->parse;

    return is_flattened_union_all_relation(root, input_rel) &&
           parse->commandType == CMD_SELECT && parse->setOperations == NULL &&
           parse->groupClause == NIL && parse->groupingSets == NIL &&
           parse->havingQual == NULL && parse->windowClause == NIL &&
           parse->distinctClause == NIL && parse->sortClause == NIL &&
           parse->limitOffset == NULL && parse->limitCount == NULL &&
           parse->rowMarks == NIL && !parse->hasAggs && !parse->hasTargetSRFs;
}

static bool
is_flattened_union_all_grouping(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    GroupPathExtraData* extra
) {
    Query* parse = root->parse;

    return extra != NULL && extra->havingQual == NULL &&
           is_flattened_union_all_relation(root, input_rel) &&
           parse->commandType == CMD_SELECT && parse->setOperations == NULL &&
           (parse->groupClause != NIL || parse->hasAggs) &&
           parse->groupingSets == NIL && parse->havingQual == NULL &&
           parse->windowClause == NIL && parse->distinctClause == NIL &&
           parse->sortClause == NIL && parse->limitOffset == NULL &&
           parse->limitCount == NULL && parse->rowMarks == NIL && !parse->hasTargetSRFs;
}

static AppendPath*
make_setop_append_path(PlannerInfo* root, AppendPath* append_path) {
#if PG_VERSION_NUM >= 190000
    AppendPathInput input = {
        .subpaths                = list_copy(append_path->subpaths),
        .partial_subpaths        = NIL,
        .child_append_relid_sets = list_copy(append_path->child_append_relid_sets),
    };

    return create_append_path(
        root,
        append_path->path.parent,
        input,
        append_path->path.pathkeys,
        NULL,
        0,
        false,
        append_path->path.rows
    );
#else
    return create_append_path(
        root,
        append_path->path.parent,
        list_copy(append_path->subpaths),
        NIL,
        append_path->path.pathkeys,
        NULL,
        0,
        false,
#if PG_VERSION_NUM < 140000
        NIL,
#endif
        append_path->path.rows
    );
#endif
}

static bool
find_setop_path(
    PlannerInfo* root,
    RelOptInfo* rel,
    AppendPath** append_path,
    bool* is_distinct,
    ChFdwSetopPathInfo* path_info,
    int32* fetch_size,
    Cost* startup_cost,
    Cost* total_cost
) {
    ListCell* lc;
    bool found = false;

    foreach (lc, rel->pathlist) {
        Path* path = (Path*)lfirst(lc);
        AppendPath* candidate;
        ChFdwSetopPathInfo candidate_info;
        ChFdwSetopValidationState state = { 0 };
        bool candidate_distinct;

        candidate = chfdw_setop_find_append_path(path, &candidate_distinct);
        if (candidate == NULL) {
            Path* subpath = NULL;

#if PG_VERSION_NUM >= 190000
            if (IsA(path, UniquePath)) {
                subpath = ((UniquePath*)path)->subpath;
            }
#else
            if (IsA(path, UpperUniquePath)) {
                subpath = ((UpperUniquePath*)path)->subpath;
            }
#endif
            if (subpath != NULL && IsA(subpath, MergeAppendPath)) {
                MergeAppendPath* merge_path = (MergeAppendPath*)subpath;
                AppendPath merge_append;

                memset(&merge_append, 0, sizeof(merge_append));
                merge_append.path.parent   = rel;
                merge_append.path.pathkeys = NIL;
                merge_append.path.rows     = path->rows;
                merge_append.subpaths      = merge_path->subpaths;
#if PG_VERSION_NUM >= 190000
                merge_append.child_append_relid_sets =
                    merge_path->child_append_relid_sets;
#endif
                candidate          = make_setop_append_path(root, &merge_append);
                candidate_distinct = true;
            }
        }
        if (candidate == NULL ||
            !chfdw_setop_extract_foreign_paths(
                candidate, setop_foreign_path_ok, &state, &candidate_info
            ) ||
            (candidate_distinct &&
             !distinct_target_is_safe(candidate->path.pathtarget))) {
            continue;
        }

        if (!found || path->total_cost < *total_cost) {
            /*
             * add_path() can discard the original candidate while retaining
             * this ForeignPath. Keep a private AppendPath wrapper around its
             * child paths for create_foreignscan_plan().
             */
            *append_path  = make_setop_append_path(root, candidate);
            *is_distinct  = candidate_distinct;
            *path_info    = candidate_info;
            *fetch_size   = state.fetch_size;
            *startup_cost = path->startup_cost;
            *total_cost   = path->total_cost;
            found         = true;
        }
    }

    return found;
}

static void
add_foreign_setop_path(
    PlannerInfo* root,
    RelOptInfo* output_rel,
    AppendPath* append_path,
    bool is_distinct,
    const ChFdwSetopPathInfo* path_info,
    int32 fetch_size,
    Cost startup_cost,
    Cost total_cost
) {
    CHFdwRelationInfo* fpinfo;
    CHFdwRelationInfo* input_fpinfo;
    ForeignPath* setop_path;
    int arm_count = list_length(path_info->foreign_paths);

    if (output_rel->fdw_private != NULL || arm_count < 2) {
        return;
    }

    fpinfo = (CHFdwRelationInfo*)palloc0(sizeof(CHFdwRelationInfo));
    input_fpinfo =
        (CHFdwRelationInfo*)((ForeignPath*)linitial(path_info->foreign_paths))
            ->path.parent->fdw_private;

    fpinfo->pushdown_safe = true;
    fpinfo->stage         = UPPERREL_SETOP;
    fpinfo->is_setop      = true;
    fpinfo->setop_all     = !is_distinct;
    fpinfo->setop_userid  = path_info->userid;
    fpinfo->grouped_tlist = make_tlist_from_pathtarget(append_path->path.pathtarget);
    fpinfo->server        = GetForeignServer(path_info->serverid);
    fpinfo->relation_name = makeStringInfo();
    appendStringInfo(
        fpinfo->relation_name,
        "%s on (%d remote queries)",
        is_distinct ? "Union distinct" : "Union all",
        arm_count
    );
    merge_fdw_options(fpinfo, input_fpinfo, NULL);
    fpinfo->fetch_size = Max(fpinfo->fetch_size, fetch_size);

    output_rel->fdw_private     = fpinfo;
    output_rel->fdwroutine      = path_info->fdwroutine;
    output_rel->serverid        = path_info->serverid;
    output_rel->userid          = path_info->userid;
    output_rel->useridiscurrent = path_info->useridiscurrent;

    total_cost =
        Max(startup_cost, total_cost - DEFAULT_FDW_STARTUP_COST * (arm_count - 1));
    setop_path = create_foreign_upper_path(
        root,
        output_rel,
        append_path->path.pathtarget,
        append_path->path.rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        NIL,
        (Path*)append_path,
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        NIL
    );
    add_path(output_rel, (Path*)setop_path);
}

/*
 * Add a grouping path whose input is a flattened, fully remote UNION ALL.
 * The input relation is a PostgreSQL subquery rather than an FDW upper
 * relation, so give it enough synthetic FDW metadata for the existing
 * grouping safety checks while retaining the original AppendPath for plan
 * construction.
 */
static void
add_foreign_setop_grouping_path(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* grouped_rel,
    AppendPath* append_path,
    const ChFdwSetopPathInfo* path_info,
    int32 fetch_size,
    Cost startup_cost,
    Cost total_cost
) {
    CHFdwRelationInfo* setop_fpinfo;
    CHFdwRelationInfo* grouped_fpinfo;
    CHFdwRelationInfo* arm_fpinfo;
    ForeignPath* grouped_path;
    List* setop_tlist;
    double rows;
    int width;
    int arm_count = list_length(path_info->foreign_paths);
    void* old_input_private;
    FdwRoutine* old_input_routine;
    Oid old_input_serverid;
    Oid old_input_userid;
    bool old_input_useridiscurrent;
    ListCell* lc;

    if (arm_count < 2 || input_rel->fdw_private != NULL ||
        grouped_rel->fdw_private != NULL || append_path->path.pathkeys != NIL ||
        append_path->path.pathtarget == NULL ||
        append_path->path.pathtarget->exprs == NIL) {
        return;
    }

    arm_fpinfo  = (CHFdwRelationInfo*)((ForeignPath*)linitial(path_info->foreign_paths))
                      ->path.parent->fdw_private;
    setop_tlist = make_tlist_from_pathtarget(append_path->path.pathtarget);
    if (!setop_grouping_target_is_safe(root, input_rel, grouped_rel, setop_tlist)) {
        return;
    }
    foreach (lc, path_info->foreign_paths) {
        ForeignPath* arm_path = lfirst_node(ForeignPath, lc);

        if (!setop_relation_uses_default_engine(arm_path->path.parent)) {
            return;
        }
    }

    setop_fpinfo                = palloc0(sizeof(CHFdwRelationInfo));
    setop_fpinfo->pushdown_safe = true;
    setop_fpinfo->stage         = UPPERREL_SETOP;
    setop_fpinfo->is_setop      = true;
    setop_fpinfo->setop_all     = true;
    setop_fpinfo->setop_userid  = path_info->userid;
    setop_fpinfo->grouped_tlist = setop_tlist;
    setop_fpinfo->server        = GetForeignServer(path_info->serverid);
    setop_fpinfo->relation_name = makeStringInfo();
    setop_fpinfo->fetch_size    = fetch_size;
    appendStringInfo(
        setop_fpinfo->relation_name, "Union all on (%d remote queries)", arm_count
    );
    merge_fdw_options(setop_fpinfo, arm_fpinfo, NULL);
    setop_fpinfo->fetch_size = Max(setop_fpinfo->fetch_size, fetch_size);

    old_input_private         = input_rel->fdw_private;
    old_input_routine         = input_rel->fdwroutine;
    old_input_serverid        = input_rel->serverid;
    old_input_userid          = input_rel->userid;
    old_input_useridiscurrent = input_rel->useridiscurrent;

    input_rel->fdw_private     = setop_fpinfo;
    input_rel->fdwroutine      = path_info->fdwroutine;
    input_rel->serverid        = path_info->serverid;
    input_rel->userid          = path_info->userid;
    input_rel->useridiscurrent = path_info->useridiscurrent;

    grouped_fpinfo                    = palloc0(sizeof(CHFdwRelationInfo));
    grouped_fpinfo->stage             = UPPERREL_GROUP_AGG;
    grouped_fpinfo->outerrel          = input_rel;
    grouped_fpinfo->table             = setop_fpinfo->table;
    grouped_fpinfo->server            = setop_fpinfo->server;
    grouped_fpinfo->user              = setop_fpinfo->user;
    grouped_fpinfo->is_setop_grouping = true;
    grouped_fpinfo->setop_all         = true;
    grouped_fpinfo->setop_userid      = path_info->userid;
    grouped_fpinfo->setop_tlist       = copyObject(setop_tlist);
    merge_fdw_options(grouped_fpinfo, setop_fpinfo, NULL);
    grouped_fpinfo->fetch_size = Max(grouped_fpinfo->fetch_size, fetch_size);
    grouped_rel->fdw_private   = grouped_fpinfo;

    if (!foreign_grouping_ok(root, grouped_rel, NULL) ||
        setop_grouping_expr_walker(
            (Node*)grouped_fpinfo->grouped_tlist,
            &(ChFdwSetopGroupingValidationState){
                .setop_tlist   = setop_tlist,
                .setop_columns = list_length(setop_tlist),
            }
        )) {
        grouped_rel->fdw_private   = NULL;
        input_rel->fdw_private     = old_input_private;
        input_rel->fdwroutine      = old_input_routine;
        input_rel->serverid        = old_input_serverid;
        input_rel->userid          = old_input_userid;
        input_rel->useridiscurrent = old_input_useridiscurrent;
        return;
    }

    input_rel->fdw_private     = old_input_private;
    input_rel->fdwroutine      = old_input_routine;
    input_rel->serverid        = old_input_serverid;
    input_rel->userid          = old_input_userid;
    input_rel->useridiscurrent = old_input_useridiscurrent;

    grouped_rel->fdwroutine      = path_info->fdwroutine;
    grouped_rel->serverid        = path_info->serverid;
    grouped_rel->userid          = path_info->userid;
    grouped_rel->useridiscurrent = path_info->useridiscurrent;

    estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.1);
    grouped_fpinfo->rows         = rows;
    grouped_fpinfo->width        = width;
    grouped_fpinfo->startup_cost = startup_cost;
    grouped_fpinfo->total_cost   = total_cost;

    grouped_path = create_foreign_upper_path(
        root,
        grouped_rel,
        grouped_rel->reltarget,
        rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        NIL,
        (Path*)append_path,
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        NIL
    );
    add_path(grouped_rel, (Path*)grouped_path);
}

/*
 * PostgreSQL does not call an FDW's GetForeignUpperPaths callback for set
 * operations, and it rewrites simple UNION ALL into an append relation.
 * The global upper-path hook covers both planner representations.
 */
void
chfdw_create_upper_paths_hook(
    PlannerInfo* root,
    UpperRelationKind stage,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    void* extra
) {
    AppendPath* append_path;
    ChFdwSetopPathInfo path_info;
    bool is_distinct;
    int32 fetch_size;
    Cost startup_cost;
    Cost total_cost;

    if (stage == UPPERREL_SETOP) {
        if (!find_setop_path(
                root,
                output_rel,
                &append_path,
                &is_distinct,
                &path_info,
                &fetch_size,
                &startup_cost,
                &total_cost
            )) {
            return;
        }
        /*
         * An ordered DISTINCT set operation needs the original setop target
         * for its local pathkeys. Keep the whole operation local until the
         * remote setop path can preserve that target exactly.
         */
        if (is_distinct && root->parse->sortClause != NIL) {
            return;
        }
    } else if (stage == UPPERREL_FINAL && is_flattened_union_all(root, input_rel)) {
        if (!find_setop_path(
                root,
                input_rel,
                &append_path,
                &is_distinct,
                &path_info,
                &fetch_size,
                &startup_cost,
                &total_cost
            ) ||
            is_distinct) {
            return;
        }
    } else if (
        stage == UPPERREL_GROUP_AGG &&
        is_flattened_union_all_grouping(root, input_rel, (GroupPathExtraData*)extra)
    ) {
        if (!find_setop_path(
                root,
                input_rel,
                &append_path,
                &is_distinct,
                &path_info,
                &fetch_size,
                &startup_cost,
                &total_cost
            ) ||
            is_distinct) {
            return;
        }
        add_foreign_setop_grouping_path(
            root,
            input_rel,
            output_rel,
            append_path,
            &path_info,
            fetch_size,
            startup_cost,
            total_cost
        );
        return;
    } else {
        return;
    }

    add_foreign_setop_path(
        root,
        output_rel,
        append_path,
        is_distinct,
        &path_info,
        fetch_size,
        startup_cost,
        total_cost
    );
}

/*
 * clickhouseGetForeignUpperPaths
 *		Add paths for post-join operations like aggregation, grouping etc. if
 *		corresponding operations are safe to push down.
 *
 * Right now, we only support aggregate, grouping and having clause pushdown.
 */
static void
clickhouseGetForeignUpperPaths(
    PlannerInfo* root,
    UpperRelationKind stage,
    RelOptInfo* input_rel,
    RelOptInfo* output_rel,
    void* extra
) {
    CHFdwRelationInfo* fpinfo;
    struct timeval time1, time2;

    gettimeofday(&time1, NULL);

    /*
     * If input rel is not safe to pushdown, then simply return as we cannot
     * perform any post-join operations on the foreign server.
     */
    if (!input_rel->fdw_private ||
        !((CHFdwRelationInfo*)input_rel->fdw_private)->pushdown_safe) {
        return;
    }

    /*
     * Set operations are represented by a complete remote query. Keep later
     * ORDER BY/LIMIT stages local until they have dedicated wrapper deparse.
     */
    if (((CHFdwRelationInfo*)input_rel->fdw_private)->is_setop) {
        return;
    }
    if (IS_UPPER_REL(input_rel) &&
        ((CHFdwRelationInfo*)input_rel->fdw_private)->stage == UPPERREL_DISTINCT) {
        return;
    }

    /* Ignore stages we don't support; and skip any duplicate calls. */
    if ((stage != UPPERREL_GROUP_AGG && stage != UPPERREL_PARTIAL_GROUP_AGG &&
         stage != UPPERREL_WINDOW && stage != UPPERREL_DISTINCT &&
         stage != UPPERREL_ORDERED && stage != UPPERREL_FINAL) ||
        output_rel->fdw_private) {
        return;
    }

    fpinfo                  = (CHFdwRelationInfo*)palloc0(sizeof(CHFdwRelationInfo));
    fpinfo->pushdown_safe   = false;
    fpinfo->stage           = stage;
    output_rel->fdw_private = fpinfo;

    switch (stage) {
    case UPPERREL_GROUP_AGG:
        add_foreign_grouping_paths(
            root, input_rel, output_rel, (GroupPathExtraData*)extra
        );
        break;
    case UPPERREL_PARTIAL_GROUP_AGG:
        if (((GroupPathExtraData*)extra)->patype == PARTITIONWISE_AGGREGATE_PARTIAL) {
            add_foreign_grouping_paths(
                root, input_rel, output_rel, (GroupPathExtraData*)extra
            );
        }
        break;
    case UPPERREL_WINDOW:
        add_foreign_window_paths(root, input_rel, output_rel);
        break;
    case UPPERREL_DISTINCT:
        add_foreign_distinct_paths(root, input_rel, output_rel);
        break;
    case UPPERREL_ORDERED:
        add_foreign_ordered_paths(root, input_rel, output_rel);
        break;
    case UPPERREL_FINAL:
        add_foreign_final_paths(root, input_rel, output_rel, extra);
        break;
    default:
        elog(ERROR, "unexpected upper relation: %d", (int)stage);
        break;
    }

    gettimeofday(&time2, NULL);
    time_used += time_diff(&time1, &time2);
}

/*
 * add_foreign_grouping_paths
 *		Add foreign path for grouping and/or aggregation.
 *
 * Given input_rel represents the underlying scan. The paths are added to the
 * given grouped_rel.
 */
static void
add_foreign_grouping_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* grouped_rel,
    GroupPathExtraData* extra
) {
    Query* parse               = root->parse;
    CHFdwRelationInfo* ifpinfo = input_rel->fdw_private;
    CHFdwRelationInfo* fpinfo  = grouped_rel->fdw_private;
    ForeignPath* grouppath;
    ListCell* lc;
    double rows;
    int width;
    Cost startup_cost;
    Cost total_cost;

    /* Nothing to be done, if there is no grouping or aggregation required. */
    if (!parse->groupClause && !parse->groupingSets && !parse->hasAggs &&
        !root->hasHavingQual) {
        return;
    }

    Assert(
        extra->patype == PARTITIONWISE_AGGREGATE_NONE ||
        extra->patype == PARTITIONWISE_AGGREGATE_FULL ||
        extra->patype == PARTITIONWISE_AGGREGATE_PARTIAL
    );

    /* save the input_rel as outerrel in fpinfo */
    fpinfo->outerrel = input_rel;

    /*
     * Copy foreign table, foreign server, user mapping, FDW options etc.
     * details from the input relation's fpinfo.
     */
    fpinfo->table  = ifpinfo->table;
    fpinfo->server = ifpinfo->server;
    fpinfo->user   = ifpinfo->user;
    merge_fdw_options(fpinfo, ifpinfo, NULL);

    /*
     * Assess if it is safe to push down aggregation and grouping.
     *
     * Use HAVING qual from extra. In case of child partition, it will have
     * translated Vars.
     */
    if (!foreign_grouping_ok(root, grouped_rel, extra->havingQual)) {
        return;
    }

    /* Estimate the cost of push down */
    estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.1);

    /* Now update this information in the fpinfo */
    fpinfo->rows         = rows;
    fpinfo->width        = width;
    fpinfo->startup_cost = startup_cost;
    fpinfo->total_cost   = total_cost;

    /* Create and add foreign path to the grouping relation. */
#if (PG_VERSION_NUM < 120000)
    grouppath = create_foreignscan_path(
        root,
        grouped_rel,
        grouped_rel->reltarget,
        rows,
        startup_cost,
        total_cost,
        NIL,  /* no pathkeys */
        NULL, /* no required_outer */
        NULL,
        NIL
    ); /* no fdw_private */
#else
    grouppath = create_foreign_upper_path(
        root,
        grouped_rel,
        grouped_rel->reltarget,
        rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        NIL, /* no pathkeys */
        NULL,
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        NIL
    ); /* no fdw_private */
#endif

    /*
     * For simple min/max queries Postgres adds MinMaxAggPath that rewrites as
     * ORDER BY col LIMIT 1. LIMIT is invisible to FDW, causing full remote
     * scan, so increase the cost to prevent this optimization.
     */
    foreach (lc, grouped_rel->pathlist) {
        Path* path = (Path*)lfirst(lc);

        if (IsA(path, MinMaxAggPath)) {
            path->startup_cost += disable_cost;
            path->total_cost += disable_cost;
        }
    }

    /* Add generated path into grouped_rel by add_path(). */
    add_path(grouped_rel, (Path*)grouppath);
}

/*
 * Add a path for plain SELECT DISTINCT when every output expression has
 * matching equality semantics on PostgreSQL and ClickHouse.
 */
static void
add_foreign_distinct_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* distinct_rel
) {
    Query* parse               = root->parse;
    CHFdwRelationInfo* ifpinfo = input_rel->fdw_private;
    CHFdwRelationInfo* fpinfo  = distinct_rel->fdw_private;
    PathTarget* target         = root->upper_targets[UPPERREL_DISTINCT];
    ForeignPath* distinct_path;
    List* tlist = NIL;
    ListCell* lc;
    double rows;
    int width;
    Cost startup_cost;
    Cost total_cost;
    int index = 0;

    if ((input_rel->reloptkind != RELOPT_BASEREL &&
         input_rel->reloptkind != RELOPT_JOINREL) ||
        parse->distinctClause == NIL || parse->hasDistinctOn || parse->hasTargetSRFs ||
        ifpinfo->local_conds || !distinct_target_is_safe(target) ||
        !setop_relation_uses_default_engine(input_rel)) {
        return;
    }

    fpinfo->outerrel = input_rel;
    fpinfo->table    = ifpinfo->table;
    fpinfo->server   = ifpinfo->server;
    fpinfo->user     = ifpinfo->user;
    merge_fdw_options(fpinfo, ifpinfo, NULL);

    foreach (lc, target->exprs) {
        Expr* expr  = lfirst_node(Expr, lc);
        Index sgref = get_pathtarget_sortgroupref(target, index++);
        TargetEntry* tle;

        if (!chfdw_is_foreign_expr(root, distinct_rel, expr, true) ||
            is_foreign_param(root, distinct_rel, expr)) {
            return;
        }

        tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
        tle->ressortgroupref = sgref;
        tlist                = lappend(tlist, tle);
    }

    fpinfo->grouped_tlist    = tlist;
    fpinfo->pushdown_safe    = true;
    fpinfo->rel_startup_cost = -1;
    fpinfo->rel_total_cost   = -1;
    fpinfo->relation_name    = makeStringInfo();
    appendStringInfo(
        fpinfo->relation_name, "Distinct on (%s)", ifpinfo->relation_name->data
    );

    estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.1);
    fpinfo->rows         = rows;
    fpinfo->width        = width;
    fpinfo->startup_cost = startup_cost;
    fpinfo->total_cost   = total_cost;

    distinct_path = create_foreign_upper_path(
        root,
        distinct_rel,
        target,
        rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        NIL,
        NULL,
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        NIL
    );
    add_path(distinct_rel, (Path*)distinct_path);
}

/*
 * foreign_window_ok
 *		Assess whether window functions in the query are safe to push down.
 *
 * Checks that every expression in the window relation's target list is
 * shippable, and builds a target list suitable for the remote query.
 */
static bool
foreign_window_ok(PlannerInfo* root, RelOptInfo* window_rel) {
    CHFdwRelationInfo* fpinfo = (CHFdwRelationInfo*)window_rel->fdw_private;
    CHFdwRelationInfo* ofpinfo;
    PathTarget* window_target = root->upper_targets[UPPERREL_WINDOW];
    ListCell* lc;
    List* tlist = NIL;
    int i;

    /* Get the fpinfo of the underlying scan relation. */
    ofpinfo = (CHFdwRelationInfo*)fpinfo->outerrel->fdw_private;

    /*
     * If underlying scan relation has any local conditions, those conditions
     * must be applied before performing windowing. Hence the window functions
     * cannot be pushed down.
     */
    if (ofpinfo->local_conds) {
        return false;
    }

    /*
     * Examine each expression in the window target list and check whether it
     * is safe to push down.  Build a tlist that matches the window target
     * exactly (same length, same order, same sortgrouprefs) so that
     * apply_pathtarget_labeling_to_tlist can match them up later.
     */
    i = 0;
    foreach (lc, window_target->exprs) {
        Expr* expr  = (Expr*)lfirst(lc);
        Index sgref = get_pathtarget_sortgroupref(window_target, i);
        TargetEntry* tle;

        /*
         * Check whether the expression is safe to push down. We need to check
         * the expression against the window_rel so that WindowFunc nodes are
         * recognized as being in an upper relation context.
         */
        if (!chfdw_is_foreign_expr(root, window_rel, expr, true)) {
            return false;
        }

        /*
         * If it would be a foreign param, we can't put it into the tlist.
         */
        if (is_foreign_param(root, window_rel, expr)) {
            return false;
        }

        /*
         * Build a TargetEntry preserving sortgroupref, without deduplication,
         * to exactly mirror the PathTarget.
         */
        tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
        tle->ressortgroupref = sgref;
        tlist                = lappend(tlist, tle);
        i++;
    }

    /* Store generated targetlist */
    fpinfo->grouped_tlist = tlist;

    /* Safe to pushdown */
    fpinfo->pushdown_safe = true;

    fpinfo->rel_startup_cost = -1;
    fpinfo->rel_total_cost   = -1;

    fpinfo->relation_name = makeStringInfo();
    appendStringInfo(
        fpinfo->relation_name, "Window on (%s)", ofpinfo->relation_name->data
    );

    return true;
}

/*
 * add_foreign_window_paths
 *		Add foreign path for performing window functions remotely.
 *
 * Given input_rel represents the underlying scan or grouping relation.
 * The paths are added to the given window_rel.
 */
static void
add_foreign_window_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* window_rel
) {
    CHFdwRelationInfo* ifpinfo = input_rel->fdw_private;
    CHFdwRelationInfo* fpinfo  = window_rel->fdw_private;
    ForeignPath* window_path;
    double rows;
    int width;
    Cost startup_cost;
    Cost total_cost;

    if (root->parse->distinctClause != NIL) {
        return;
    }

    /* Save the input_rel as outerrel in fpinfo */
    fpinfo->outerrel = input_rel;

    /*
     * Copy foreign table, foreign server, user mapping, FDW options etc.
     * details from the input relation's fpinfo.
     */
    fpinfo->table  = ifpinfo->table;
    fpinfo->server = ifpinfo->server;
    fpinfo->user   = ifpinfo->user;
    merge_fdw_options(fpinfo, ifpinfo, NULL);

    /* Assess if it is safe to push down the window functions */
    if (!foreign_window_ok(root, window_rel)) {
        return;
    }

    /* Estimate the cost of push down */
    estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.1);

    /* Now update this information in the fpinfo */
    fpinfo->rows         = rows;
    fpinfo->width        = width;
    fpinfo->startup_cost = startup_cost;
    fpinfo->total_cost   = total_cost;

    /* Create and add foreign path to the window relation. */
#if (PG_VERSION_NUM < 120000)
    window_path = create_foreignscan_path(
        root,
        window_rel,
        root->upper_targets[UPPERREL_WINDOW],
        rows,
        startup_cost,
        total_cost,
        NIL,  /* no pathkeys */
        NULL, /* no required_outer */
        NULL,
        NIL
    ); /* no fdw_private */
#else
    window_path = create_foreign_upper_path(
        root,
        window_rel,
        root->upper_targets[UPPERREL_WINDOW],
        rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        NIL, /* no pathkeys */
        NULL,
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        NIL
    ); /* no fdw_private */
#endif

    /* Add generated path into window_rel by add_path(). */
    add_path(window_rel, (Path*)window_path);
}

/*
 * add_foreign_ordered_paths
 *		Add foreign paths for performing the final sort remotely.
 *
 * Given input_rel contains the source-data Paths. The paths are added to the
 * given ordered_rel.
 */
static void
add_foreign_ordered_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* ordered_rel
) {
    Query* parse               = root->parse;
    CHFdwRelationInfo* ifpinfo = input_rel->fdw_private;
    CHFdwRelationInfo* fpinfo  = ordered_rel->fdw_private;
    ChFdwPathExtraData* fpextra;
    double rows;
    int width;
    Cost startup_cost;
    Cost total_cost;
    List* fdw_private;
    ForeignPath* ordered_path;
    ListCell* lc;

    /* Shouldn't get here unless the query has ORDER BY */
    Assert(parse->sortClause);

    /* We don't support cases where there are any SRFs in the targetlist */
    if (parse->hasTargetSRFs) {
        return;
    }

    /* Save the input_rel as outerrel in fpinfo */
    fpinfo->outerrel = input_rel;

    /*
     * Copy foreign table, foreign server, user mapping, FDW options etc.
     * details from the input relation's fpinfo.
     */
    fpinfo->table  = ifpinfo->table;
    fpinfo->server = ifpinfo->server;
    fpinfo->user   = ifpinfo->user;
    merge_fdw_options(fpinfo, ifpinfo, NULL);

    /*
     * If the input_rel is a base or join relation, we would already have
     * considered pushing down the final sort to the remote server when
     * creating pre-sorted foreign paths for that relation, because the
     * query_pathkeys is set to the root->sort_pathkeys in that case (see
     * standard_qp_callback()).
     */
    if (input_rel->reloptkind == RELOPT_BASEREL ||
        input_rel->reloptkind == RELOPT_JOINREL) {
        Assert(root->query_pathkeys == root->sort_pathkeys);

        /* Safe to push down if the query_pathkeys is safe to push down */
        fpinfo->pushdown_safe = ifpinfo->qp_is_pushdown_safe;

        return;
    }

    /* The input_rel should be a grouping or window relation */
    Assert(
        input_rel->reloptkind == RELOPT_UPPER_REL &&
        (ifpinfo->stage == UPPERREL_GROUP_AGG || ifpinfo->stage == UPPERREL_WINDOW)
    );

    /*
     * We try to create a path below by extending a simple foreign path for
     * the underlying grouping relation to perform the final sort remotely,
     * which is stored into the fdw_private list of the resulting path.
     */

    /* Assess if it is safe to push down the final sort */
    foreach (lc, root->sort_pathkeys) {
        PathKey* pathkey             = (PathKey*)lfirst(lc);
        EquivalenceClass* pathkey_ec = pathkey->pk_eclass;
        Expr* sort_expr;

        /*
         * chfdw_is_foreign_expr would detect volatile expressions as well,
         * but checking ec_has_volatile here saves some cycles.
         */
        if (pathkey_ec->ec_has_volatile) {
            return;
        }

        /*
         * Get the sort expression for the pathkey_ec. Upper rels may have an
         * empty reltarget; use the planner's upper target for that stage
         * instead.
         */
        sort_expr = chfdw_find_em_expr_for_input_target(
            root,
            pathkey_ec,
            input_rel->reltarget->exprs != NIL ? input_rel->reltarget
                                               : root->upper_targets[ifpinfo->stage]
        );

        if (sort_expr == NULL ||
            !chfdw_is_foreign_expr(root, input_rel, sort_expr, true)) {
            return;
        }
    }

    /* Safe to push down */
    fpinfo->pushdown_safe = true;

    /* Construct ChFdwPathExtraData */
    fpextra                 = (ChFdwPathExtraData*)palloc0(sizeof(ChFdwPathExtraData));
    fpextra->target         = root->upper_targets[UPPERREL_ORDERED];
    fpextra->has_final_sort = true;

    estimate_path_cost_size(&rows, &width, &startup_cost, &total_cost, 0.1);

    /*
     * Build the fdw_private list that will be used by
     * clickhouseGetForeignPlan. Items in the list must match order in enum
     * FdwPathPrivateIndex.
     */
    fdw_private = list_make2(makeInteger(true), makeInteger(false));

    /* Create foreign ordering path */
#if (PG_VERSION_NUM < 120000)
    ordered_path = create_foreignscan_path(
        root,
        input_rel,
        root->upper_targets[UPPERREL_ORDERED],
        rows,
        startup_cost,
        total_cost,
        root->sort_pathkeys,
        NULL, /* no required_outer */
        NULL,
        fdw_private
    );
#else
    ordered_path = create_foreign_upper_path(
        root,
        input_rel,
        root->upper_targets[UPPERREL_ORDERED],
        rows,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        startup_cost,
        total_cost,
        root->sort_pathkeys,
        NULL, /* no extra plan */
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        fdw_private
    );
#endif

    /* and add it to the ordered_rel */
    add_path(ordered_rel, (Path*)ordered_path);
}

/*
 * add_foreign_final_paths
 *		Add foreign paths for performing the final processing remotely.
 *
 * Given input_rel contains the source-data Paths. The paths are added to the
 * given final_rel.
 */
static void
add_foreign_final_paths(
    PlannerInfo* root,
    RelOptInfo* input_rel,
    RelOptInfo* final_rel,
    void* fextra
) {
#if (PG_VERSION_NUM < 120000)
    /* final paths supported only on pg >= v12 */
    return;
#else
    Query* parse               = root->parse;
    CHFdwRelationInfo* ifpinfo = (CHFdwRelationInfo*)input_rel->fdw_private;
    CHFdwRelationInfo* fpinfo  = (CHFdwRelationInfo*)final_rel->fdw_private;
    bool has_final_sort        = false;
    List* pathkeys             = NIL;
    ChFdwPathExtraData* fpextra;
    List* fdw_private;
    ForeignPath* final_path;
    FinalPathExtraData* extra = (FinalPathExtraData*)fextra;

    /*
     * Currently, we only support this for SELECT commands
     */
    if (parse->commandType != CMD_SELECT) {
        return;
    }

    /*
     * No work if there is no FOR UPDATE/SHARE clause and if there is no need
     * to add a LIMIT node
     */
    if (!extra->limit_needed) {
        return;
    }

    /* We don't support cases where there are any SRFs in the targetlist */
    if (parse->hasTargetSRFs) {
        return;
    }

    /* Save the input_rel as outerrel in fpinfo */
    fpinfo->outerrel = input_rel;

    /*
     * Copy foreign table, foreign server, user mapping, FDW options etc.
     * details from the input relation's fpinfo.
     */
    fpinfo->table  = ifpinfo->table;
    fpinfo->server = ifpinfo->server;
    fpinfo->user   = ifpinfo->user;
    merge_fdw_options(fpinfo, ifpinfo, NULL);

    Assert(extra->limit_needed);

    /*
     * If the input_rel is an ordered relation, replace the input_rel with its
     * input relation
     */
    if (input_rel->reloptkind == RELOPT_UPPER_REL &&
        ifpinfo->stage == UPPERREL_ORDERED) {
        input_rel      = ifpinfo->outerrel;
        ifpinfo        = (CHFdwRelationInfo*)input_rel->fdw_private;
        has_final_sort = true;
        pathkeys       = root->sort_pathkeys;
    }

    /* The input_rel should be a base, join, grouping, or window relation */
    Assert(
        input_rel->reloptkind == RELOPT_BASEREL ||
        input_rel->reloptkind == RELOPT_JOINREL ||
        (input_rel->reloptkind == RELOPT_UPPER_REL &&
         (ifpinfo->stage == UPPERREL_GROUP_AGG || ifpinfo->stage == UPPERREL_WINDOW))
    );

    /*
     * We try to create a path below by extending a simple foreign path for
     * the underlying base, join, or grouping relation to perform the final
     * sort (if has_final_sort) and the LIMIT restriction remotely, which is
     * stored into the fdw_private list of the resulting path. (We re-estimate
     * the costs of sorting the underlying relation, if has_final_sort.)
     */

    /*
     * Assess if it is safe to push down the LIMIT and OFFSET to the remote
     * server
     */

    /*
     * If the underlying relation has any local conditions, the LIMIT/OFFSET
     * cannot be pushed down.
     */
    if (ifpinfo->local_conds) {
        return;
    }

    /*
     * Also, the LIMIT/OFFSET cannot be pushed down, if their expressions are
     * not safe to remote.
     */
    if (!chfdw_is_foreign_expr(root, input_rel, (Expr*)parse->limitOffset, true) ||
        !chfdw_is_foreign_expr(root, input_rel, (Expr*)parse->limitCount, true)) {
        return;
    }

    /* Safe to push down */
    fpinfo->pushdown_safe = true;

    /* Construct ChFdwPathExtraData */
    fpextra                 = (ChFdwPathExtraData*)palloc0(sizeof(ChFdwPathExtraData));
    fpextra->target         = root->upper_targets[UPPERREL_FINAL];
    fpextra->has_final_sort = has_final_sort;
    fpextra->has_limit      = extra->limit_needed;
    fpextra->limit_tuples   = extra->limit_tuples;
    fpextra->count_est      = extra->count_est;
    fpextra->offset_est     = extra->offset_est;
    ifpinfo->use_remote_estimate = false;

    /*
     * Build the fdw_private list that will be used by
     * clickhouseGetForeignPlan. Items in the list must match order in enum
     * FdwPathPrivateIndex.
     */
    fdw_private =
        list_make2(makeInteger(has_final_sort), makeInteger(extra->limit_needed));

    /*
     * Create foreign final path; this gets rid of a no-longer-needed outer
     * plan (if any), which makes the EXPLAIN output look cleaner
     */
    final_path = create_foreign_upper_path(
        root,
        input_rel,
        root->upper_targets[UPPERREL_FINAL],
        1,
#if PG_VERSION_NUM >= 180000
        0,
#endif
        0,
        -10,
        pathkeys,
        NULL, /* no extra plan */
#if PG_VERSION_NUM >= 170000
        NIL,
#endif
        fdw_private
    );

    /* and add it to the final_rel */
    add_path(final_rel, (Path*)final_path);
#endif
}

/*
 * Find an equivalence class member expression, all of whose Vars come from
 * the indicated relation.
 */
Expr*
chfdw_find_em_expr_for_rel(EquivalenceClass* ec, RelOptInfo* rel) {
    ListCell* lc_em;

    foreach (lc_em, ec->ec_members) {
        EquivalenceMember* em = lfirst(lc_em);

        if (bms_is_subset(em->em_relids, rel->relids) && !bms_is_empty(em->em_relids)) {
            /*
             * If there is more than one equivalence member whose Vars are
             * taken entirely from this relation, we'll be content to choose
             * any one of those.
             */
            return em->em_expr;
        }
    }

    /* We didn't find any suitable equivalence class expression */
    return NULL;
}

/*
 * Find an equivalence class member expression to be computed as a sort column
 * in the given target.
 */
Expr*
chfdw_find_em_expr_for_input_target(
    PlannerInfo* root,
    EquivalenceClass* ec,
    PathTarget* target
) {
    ListCell* lc1;
    int i;

    i = 0;
    foreach (lc1, target->exprs) {
        Expr* expr  = (Expr*)lfirst(lc1);
        Index sgref = get_pathtarget_sortgroupref(target, i);
        ListCell* lc2;

        /* Ignore non-sort expressions */
        if (sgref == 0 ||
            get_sortgroupref_clause_noerr(sgref, root->parse->sortClause) == NULL) {
            i++;
            continue;
        }

        /* We ignore binary-compatible relabeling on both ends */
        while (expr && IsA(expr, RelabelType)) {
            expr = ((RelabelType*)expr)->arg;
        }

        /* Locate an EquivalenceClass member matching this expr, if any */
        foreach (lc2, ec->ec_members) {
            EquivalenceMember* em = (EquivalenceMember*)lfirst(lc2);
            Expr* em_expr;

            /* Don't match constants */
            if (em->em_is_const) {
                continue;
            }

            /* Ignore child members */
            if (em->em_is_child) {
                continue;
            }

            /* Match if same expression (after stripping relabel) */
            em_expr = em->em_expr;
            while (em_expr && IsA(em_expr, RelabelType)) {
                em_expr = ((RelabelType*)em_expr)->arg;
            }

            if (equal(em_expr, expr)) {
                return em->em_expr;
            }
        }

        i++;
    }

    return NULL;
}

static List*
clickhouseImportForeignSchema(ImportForeignSchemaStmt* stmt, Oid serverOid) {
    ForeignServer* server;

    server = GetForeignServer(serverOid);
    return chfdw_construct_create_tables(stmt, server);
}

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
clickhouse_fdw_handler(PG_FUNCTION_ARGS) {
    FdwRoutine* routine = makeNode(FdwRoutine);

    /* Functions for scanning foreign tables */
    routine->GetForeignRelSize  = clickhouseGetForeignRelSize;
    routine->GetForeignPaths    = clickhouseGetForeignPaths;
    routine->GetForeignPlan     = clickhouseGetForeignPlan;
    routine->BeginForeignScan   = clickhouseBeginForeignScan;
    routine->IterateForeignScan = clickhouseIterateForeignScan;
    routine->ReScanForeignScan  = clickhouseReScanForeignScan;
    routine->EndForeignScan     = clickhouseEndForeignScan;

    /* Functions for updating foreign tables */
    routine->PlanForeignModify  = clickhousePlanForeignModify;
    routine->BeginForeignModify = clickhouseBeginForeignModify;
    routine->BeginForeignInsert = clickhouseBeginForeignInsert;
    routine->ExecForeignInsert  = clickhouseExecForeignInsert;

    /*
     * No ExecForeignBatchInsert / GetForeignModifyBatchSize: both drivers
     * already buffer rows and stream them as a single INSERT to ClickHouse, so
     * PG's batch-insert API would add nothing.
     *
     * TODO ExecForeignUpdate / ExecForeignDelete via ClickHouse mutations.
     */

    routine->EndForeignInsert = clickhouseEndForeignInsert;
    routine->EndForeignModify = clickhouseEndForeignInsert;

    /* Function for EvalPlanQual rechecks */
    routine->RecheckForeignScan = clickhouseRecheckForeignScan;

    /* Support functions for EXPLAIN */
    routine->ExplainForeignScan = clickhouseExplainForeignScan;

    /* Support functions for ANALYZE */
    routine->AnalyzeForeignTable = clickhouseAnalyzeForeignTable;

    /* Support functions for join push-down */
    routine->GetForeignJoinPaths = clickhouseGetForeignJoinPaths;

    /* Support functions for upper relation push-down */
    routine->GetForeignUpperPaths = clickhouseGetForeignUpperPaths;

    /* IMPORT FOREIGN SCHEMA */
    routine->ImportForeignSchema = clickhouseImportForeignSchema;

    PG_RETURN_POINTER(routine);
}

/*
 * No-op function to use for the final function of a variadic function that
 * takes any number of values of any type.
 */
Datum
clickhouse_noop(PG_FUNCTION_ARGS) {
    PG_RETURN_NULL();
}

/*
 * Function that simply raises an exception reporting that the operation
 * described by the first argument should have been pushed down.
 */
Datum
clickhouse_op_push_fail(PG_FUNCTION_ARGS) {
    text* name = PG_GETARG_TEXT_PP(0);

    ereport(
        ERROR,
        errcode(ERRCODE_FDW_ERROR),
        errmsg("pg_clickhouse: failed to push down %s", text_to_cstring(name))
    );
}

/*
 * Function that simply raises an exception reporting that the function should
 * have been pushed down.
 */
Datum
clickhouse_push_fail(PG_FUNCTION_ARGS) {
    char* name = get_func_name(fcinfo->flinfo->fn_oid);

    ereport(
        ERROR,
        errcode(ERRCODE_FDW_ERROR),
        errmsg("pg_clickhouse: failed to push down %s()", name)
    );
}

/*
 * Function returns the full pg_clickhouse library version.
 */
Datum
pgch_version(PG_FUNCTION_ARGS) {
    PG_RETURN_TEXT_P(cstring_to_text(PGCH_VERSION));
}
