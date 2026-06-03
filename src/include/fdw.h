/*-------------------------------------------------------------------------
 *
 * fdw.h
 *		  Foreign-data wrapper for remote Clickhouse servers
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/include/fdw.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CLICKHOUSE_FDW_H
#define CLICKHOUSE_FDW_H

#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "utils/relcache.h"
#include "catalog/pg_operator.h"
#include "nodes/execnodes.h"
#include "optimizer/optimizer.h"
#include "nodes/pathnodes.h"
#include "access/heapam.h"
#include "engine.h"
#include "funcapi.h"

#if PG_VERSION_NUM < 150000
#define FirstUnpinnedObjectId FirstBootstrapObjectId
#endif

/*
 * Max allowed length of a quoted ClickHouse identifier, including null
 * terminator. ClickHouse allows longer names (internally they're hashed) but
 * there are a bunch of wrinkles, and names in Postgres can't be more than
 * NAMEDATALEN (currently 64), so just make enough room for every character to
 * be escaped plus opening and closing quotation marks.
*/
#define CH_ESCAPED_NAMEDATALEN NAMEDATALEN*2

/* pglink.c */
typedef struct ch_cursor ch_cursor;
typedef struct ch_cursor
{
	MemoryContext memcxt;		/* used for cleanup */
	MemoryContextCallback callback;

	void	   *query_response;
	void	   *read_state;
	void	   *conn;
	char	   *query;
	double		request_time;
	double		total_time;
	size_t		columns_count;
	uintptr_t  *conversion_states;	/* for binary */
}			ch_cursor;

typedef struct ChFdwScanRowContext
{
	TupleDesc	tupdesc;		/* tuple descriptor for row */
	List	   *retrieved_attrs;	/* list of retrieved attribute numbers */
	AttInMetadata *attinmeta;	/* list of incoming attributes */
	ch_cursor  *cursor;			/* result of query from clickhouse */
	Datum	   *values;			/* collected values for each column */
	bool	   *nulls;			/* indicates null columns */
}			ChFdwScanRowContext;

typedef void (*disconnect_method) (void *conn);
typedef void (*check_conn_method) (const char *password, UserMapping * user);
typedef ch_cursor * (*simple_query_method) (void *conn, const ch_query * query);
typedef void (*simple_insert_method) (void *conn, const ch_query * query);
typedef Datum * (*cursor_fetch_row_method) (ChFdwScanRowContext * ctx);
typedef void *(*prepare_insert_method) (void *conn, ResultRelInfo *, List *,
										const ch_query *, char *);
typedef void (*insert_tuple_method) (void *state, TupleTableSlot * slot);
typedef void (*finalize_insert_method) (void *state);
typedef ch_cursor * (*streaming_query_method) (void *conn,
											   const ch_query * query,
											   int32 fetch_size);
typedef bool (*is_broken_method) (const void *conn);

typedef struct
{
	disconnect_method disconnect;
	simple_query_method simple_query;
	cursor_fetch_row_method fetch_row;
	prepare_insert_method prepare_insert;
	insert_tuple_method insert_tuple;
	finalize_insert_method finalize_insert; /* NULL if no explicit finalize
											 * step needed */
	streaming_query_method streaming_query; /* NULL if not supported */
	cursor_fetch_row_method streaming_fetch_row;	/* NULL if not supported */
	is_broken_method is_broken; /* NULL means connection is never broken */
}			libclickhouse_methods;

typedef struct
{
	libclickhouse_methods *methods;
	void	   *conn;
	bool		is_binary;
}			ch_connection;

ch_connection_details *connstring_parse(const char *connstring);
ch_connection chfdw_http_connect(ch_connection_details * details);
ch_connection chfdw_binary_connect(ch_connection_details * details);
text	   *chfdw_http_fetch_raw_data(ch_cursor * cursor);
List	   *chfdw_construct_create_tables(ImportForeignSchemaStmt * stmt, ForeignServer * server);
char	   *ch_quote_literal(const char *rawstr);
char	   *chfdw_datum_to_ch_literal(Datum value, Oid type);
const char *ch_quote_ident(const char *rawstr);

typedef enum
{
	CH_DEFAULT,
	CH_COLLAPSING_MERGE_TREE,
	CH_AGGREGATING_MERGE_TREE
}			CHRemoteTableEngine;

/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * pg_clickhouse foreign table. For a baserel, this struct is created by
 * clickhouseGetForeignRelSize, although some fields are not filled till
 * later. clickhouseGetForeignJoinPaths creates it for a joinrel, and
 * clickhouseGetForeignUpperPaths creates it for an upperrel.
 */
typedef struct CHFdwRelationInfo
{
	/*
	 * True means that the relation can be pushed down. Always true for simple
	 * foreign scan.
	 */
	bool		pushdown_safe;

	/*
	 * Restriction clauses, divided into safe and unsafe to pushdown subsets.
	 * All entries in these lists should have RestrictInfo wrappers; that
	 * improves efficiency of selectivity and cost estimation.
	 */
	List	   *remote_conds;
	List	   *local_conds;

	/* Actual remote restriction clauses for scan (sans RestrictInfos) */
	List	   *final_remote_exprs;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;

	/* True means that the query_pathkeys is safe to push down */
	bool		qp_is_pushdown_safe;

	/* Cost and selectivity of local_conds. */
	QualCost	local_conds_cost;
	Selectivity local_conds_sel;

	/* Selectivity of join conditions */
	Selectivity joinclause_sel;

	/* Estimated size and cost for a scan or join. */
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/* Costs excluding costs for transferring data from the foreign server */
	Cost		rel_startup_cost;
	Cost		rel_total_cost;

	/* Options extracted from catalogs. */
	bool		use_remote_estimate;
	Cost		fdw_startup_cost;
	Cost		fdw_tuple_cost;
	List	   *shippable_extensions;	/* OIDs of whitelisted extensions */

	/* Cached catalog information. */
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;			/* only set in use_remote_estimate mode */

	int32		fetch_size;		/* fetch size for this remote table */

	/*
	 * Name of the relation while EXPLAINing ForeignScan. It is used for join
	 * relations but is set for all relations. For join relation, the name
	 * indicates which foreign tables are being joined and the join type used.
	 */
	StringInfo	relation_name;

	/* Join information */
	RelOptInfo *outerrel;
	RelOptInfo *innerrel;
	JoinType	jointype;

	/* joinclauses contains only JOIN/ON conditions for an outer join */
	List	   *joinclauses;	/* List of RestrictInfo */

	/* Upper relation information */
	UpperRelationKind stage;

	/* Grouping information */
	List	   *grouped_tlist;

	/* Subquery information */
	bool		make_outerrel_subquery; /* do we deparse outerrel as a
										 * subquery? */
	bool		make_innerrel_subquery; /* do we deparse innerrel as a
										 * subquery? */
	Relids		lower_subquery_rels;	/* all relids appearing in lower
										 * subqueries */

	/*
	 * Index of the relation. It is used to create an alias to a subquery
	 * representing the relation.
	 */
	int			relation_index;

	/* Custom */
	CHRemoteTableEngine ch_table_engine;

	/*
	 * Long enough for a quoted identifier with all but two characters
	 * escaped.
	 */
	char		ch_table_sign_field[CH_ESCAPED_NAMEDATALEN];
}			CHFdwRelationInfo;

/* in fdw.c */
extern ForeignServer * chfdw_get_foreign_server(Relation rel);
extern Expr * chfdw_find_em_expr_for_input_target(PlannerInfo * root,
												  EquivalenceClass * ec,
												  PathTarget * target);
extern Expr * chfdw_find_em_expr_for_rel(EquivalenceClass * ec, RelOptInfo * rel);

/* in connection.c */
extern ch_connection chfdw_get_connection(UserMapping * user);
extern void chfdw_exec_query(ch_connection conn, const char *query);
extern void chfdw_report_error(int elevel, ch_connection conn,
							   bool clear, const char *sql);

/* in option.c */
extern kv_list * chfdw_get_session_settings(void);
bool		chfdw_pushdown_regex_ok(void);

extern void
			chfdw_extract_options(List * defelems, char **driver, char **host, int *port,
								  char **dbname, char **username, char **password);
extern List * chfdw_parse_options(const char *options, bool with_comma, bool with_equal);

/* in deparse.c */
extern void chfdw_classify_conditions(PlannerInfo * root,
									  RelOptInfo * baserel,
									  List * input_conds,
									  List * *remote_conds,
									  List * *local_conds);
extern bool chfdw_is_foreign_expr(PlannerInfo * root,
								  RelOptInfo * baserel,
								  Expr * expr);
extern bool is_foreign_param(PlannerInfo * root,
							 RelOptInfo * baserel,
							 Expr * expr);
extern char *chfdw_deparse_insert_sql(StringInfo buf, RangeTblEntry * rte,
									  Index rtindex, Relation rel,
									  List * targetAttrs);
extern List * chfdw_build_tlist_to_deparse(RelOptInfo * foreignrel);
extern void chfdw_deparse_select_stmt_for_rel(StringInfo buf, PlannerInfo * root, RelOptInfo * rel,
											  List * tlist, List * remote_conds, List * pathkeys,
											  bool has_final_sort, bool has_limit, bool is_subquery,
											  List * *retrieved_attrs, List * *params_list);
extern const char *chfdw_get_jointype_name(JoinType jointype);
char	   *chfdw_array_to_ch_literal(Datum arr);

/* in shippable.c */
extern bool chfdw_is_builtin(Oid objectId);
extern int	chfdw_is_equal_op(Oid opno);

/*
 * Connection cache hash table entry
 */
typedef struct ConnCacheKey
{
	Oid			userid;
}			ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* hash key (must be first) */
	ch_connection gate;			/* connection to foreign server, or NULL */
	/* Remaining fields are invalid when conn is NULL: */
	bool		invalidated;	/* true if reconnect is pending */
	uint32		server_hashvalue;	/* hash value of foreign server OID */
	uint32		mapping_hashvalue;	/* hash value of user mapping OID */
}			ConnCacheEntry;

/* Custom behavior types */
typedef enum
{
	CF_USUAL = 0,
	CF_SIGN_SUM,				/* SUM aggregation */
	CF_SIGN_AVG,				/* AVG aggregation */
	CF_SIGN_COUNT,				/* COUNT aggregation */
	CF_DATE_TRUNC,				/* date_trunc function */
	CF_DATE_PART,				/* date_part function */
	CF_TIMESTAMPTZ_PL_INTERVAL, /* timestamptz + interval */
	CF_TIMEZONE,				/* timezone */
	CF_HSTORE_FETCHVAL,			/* -> operation on hstore */
	CF_INTARRAY_IDX,
	CF_CH_FUNCTION,				/* adapted clickhouse function */
	CF_MATCH,					/* regexp_match function */
	CF_SPLIT_BY_REGEX,			/* regexp_split_to_array → splitByRegexp */
	CF_REPLACE_REGEX,			/* regexp_replace → replaceRegexpOne or
								 * replaceRegexpAll */
	CF_REGEX_MATCH,				/* ~ POSIX regex operator */
	CF_REGEX_NO_MATCH,			/* !~ POSIX regex operator */
	CF_REGEX_ICASE_MATCH,		/* ~* case-insensitive regex operator */
	CF_REGEX_ICASE_NO_MATCH,	/* !~* case-insensitive regex operator */
	CF_JSON_FETCHVAL,			/* -> operator on json & jsonb */
	CF_JSON_FETCHVAL_TEXT,		/* ->> operator on json & jsonb */
	CF_JSON_EXTRACT_PATH_TEXT,	/* jsonb?_extract_path_text() →
								 * col."k1"."k2" */
	CF_JSON_EXTRACT_PATH,		/* json?_extract_path() →
								 * toJSONString(col."k1"."k2") */
	CF_STRING_AGG,				/* string_agg → groupConcat(delim)(expr) */
	CF_CURRENT_DATABASE,		/* CURRENT_DATABASE → string literal */
	CF_CURRENT_SCHEMA,			/* CF_CURRENT_SCHEMA → string literal */
	CF_CLOCK_TIMESTAMP,			/* clock_timestamp → nowInBlock64(6, $TZ) */
	CF_ARRAY_LENGTH,			/* array_length → length, drop dim arg */
	CF_ARRAY_PREPEND,			/* array_prepend → arrayPushFront, swap args */
	CF_STRING_TO_ARRAY,			/* string_to_array → splitByString, swap
								 * args */
	CF_STRING_TO_ARRAY_PART,	/* split_part → splitByString()[n] */
	CF_TRIM_ARRAY,				/* trim_array → arrayResize(arr,
								 * length(arr)-n) */
	CF_ARRAY_SORT_DESC,			/* array_sort(arr,desc) →
								 * arrayReverseSort/arraySort */
	CF_ARRAY_FILL,				/* array_fill → arrayWithConstant,
								 * swap+extract */
	CF_ARRAY_CONTAINS,			/* @> → hasAll(left, right) */
	CF_ARRAY_CONTAINED_BY,		/* <@ → hasAll(right, left) */
	CF_ARRAY_OVERLAP,			/* && → hasAny(left, right) */
	CF_TO_CHAR,					/* to_char(timestamp[tz], fmt) →
								 * formatDateTime, with strict format
								 * translation */
}			custom_object_type;

typedef enum
{
	CF_AGGR_USUAL = 0,
	CF_AGGR_FUNC = 1,
	CF_AGGR_SIMPLE = 2
} ch_aggregate_func_type;

typedef struct CustomObjectDef
{
	Oid			cf_oid;
	custom_object_type cf_type;
	char		custom_name[NAMEDATALEN];	/* \0 - no custom name, \1 - many
											 * names */
	int			paren_count;	/* Number closing parens; defaults to 1 */
	Oid			rowfunc;
	void	   *cf_context;
}			CustomObjectDef;

typedef struct CustomColumnInfo
{
	Oid			relid;
	int			varattno;
	char		colname[NAMEDATALEN];
	ch_aggregate_func_type is_AggregateFunction;
	custom_object_type coltype;

	CHRemoteTableEngine table_engine;
}			CustomColumnInfo;

extern bool chfdw_check_for_ordered_aggregate(Aggref * agg);
extern CustomObjectDef * chfdw_check_for_custom_function(Oid funcid);
extern CustomObjectDef * chfdw_check_for_custom_type(Oid typeoid);
extern void chfdw_apply_custom_table_options(CHFdwRelationInfo * fpinfo, Oid relid);
extern CustomColumnInfo * chfdw_get_custom_column_info(Oid relid, uint16 varattno);
extern CustomObjectDef * chfdw_check_for_custom_operator(Oid opoid, Form_pg_operator form);

extern Datum ch_timestamp_out(PG_FUNCTION_ARGS);
extern Datum ch_date_out(PG_FUNCTION_ARGS);
extern Datum ch_time_out(PG_FUNCTION_ARGS);

extern bool chfdw_is_shippable(Node * node, Oid objectId, Oid classId, CHFdwRelationInfo * fpinfo,
							   CustomObjectDef * *outcdef);
extern double time_diff(struct timeval *prior, struct timeval *latter);

/*
 * Translate a Postgres `to_char()` template into a ClickHouse
 * `formatDateTime()` template.  Returns true on success, with the
 * translation appended to `out` (caller-owned, may be NULL to validate
 * only).  Returns false on any unsupported PG keyword or modifier;
 * when used to gate pushdown, false means caller must evaluate locally.
 */
extern bool chfdw_translate_to_char_format(const char *pgfmt, StringInfo out);

/* compat */
#if PG_VERSION_NUM < 120000

#define CreateTemplateTupleDescCompat(natts) \
	CreateTemplateTupleDesc(natts, false)
#define ExecStoreHeapTuple(tup,slot,free) \
	ExecStoreTuple(tup,slot,InvalidBuffer,free)

#define execute_attr_map_tuple		do_convert_tuple
#define create_foreign_join_path	create_foreignscan_path

#define T_SubscriptingRef	T_ArrayRef
#define SubscriptingRef		ArrayRef
#else
#define CreateTemplateTupleDescCompat(natts) \
	CreateTemplateTupleDesc(natts)
#endif

#if PG_VERSION_NUM < 130000
#define table_open_compat(i,l) heap_open(i, l)
#define table_close_compat(r,l) heap_close(r, l)
#define lnext_compat(l,i) lnext(i)
#else
#define table_open_compat(i,l) table_open(i, l)
#define table_close_compat(r,l) table_close(r, l)
#define lnext_compat(l,i) lnext(l,i)
#endif

#endif							/* CLICKHOUSE_FDW_H */
