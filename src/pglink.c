#include "postgres.h"

#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_type_d.h"
#include "funcapi.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "parser/parse_coerce.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/uuid.h"

#include "fdw.h"
#include "http.h"
#include "binary.hh"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static bool		initialized = false;

static void http_disconnect(void *conn);
static ch_cursor *http_simple_query(void *conn, const char *query);
static void http_simple_insert(void *conn, const char *query);
static void http_cursor_free(void *);
static void **http_fetch_row(ch_cursor *, List *, TupleDesc, Datum *, bool *);
static void *http_prepare_insert(void *, ResultRelInfo *, List *, char *, char *);
static void http_insert_tuple(void *, TupleTableSlot *);

static libclickhouse_methods http_methods = {
	.disconnect=http_disconnect,
	.simple_query=http_simple_query,
	.fetch_row=http_fetch_row,
	.prepare_insert=http_prepare_insert,
	.insert_tuple=http_insert_tuple
};

static void binary_disconnect(void *conn);
static ch_cursor *binary_simple_query(void *conn, const char *query);
static void binary_cursor_free(void *cursor);
// static void binary_simple_insert(void *conn, const char *query);
static void **binary_fetch_row(ch_cursor *cursor, List* attrs, TupleDesc tupdesc,
		Datum *values, bool *nulls);
static void binary_insert_tuple(void *, TupleTableSlot *slot);
static void *binary_prepare_insert(void *, ResultRelInfo *, List *,
		char *query, char *table_name);

static size_t escape_string(char *to, const char *from, size_t length);

static libclickhouse_methods binary_methods = {
	.disconnect=binary_disconnect,
	.simple_query=binary_simple_query,
	.fetch_row=binary_fetch_row,
	.prepare_insert=binary_prepare_insert,
	.insert_tuple=binary_insert_tuple
};

static int http_progress_callback(void *clientp, double dltotal, double dlnow,
		double ultotal, double ulnow)
{
	if (ProcDiePending || QueryCancelPending)
		return 1;

	return 0;
}

static bool is_canceled(void)
{
	/* this variable is bool on pg < 12, but sig_atomic_t on above versions */
	if (QueryCancelPending)
		return true;

	return false;
}

ch_connection
chfdw_http_connect(ch_connection_details *details)
{
	ch_connection res;
	ch_http_connection_t *conn = ch_http_connection(details->host, details->port,
            details->username, details->password);
	if (!initialized)
	{
		initialized = true;
		ch_http_init(0, (uint32_t) MyProcPid);
	}

	if (conn == NULL)
	{
		char *error = ch_http_last_error();
		if (error == NULL)
			error = "undefined";

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("could not connect to server: %s", error)));
	}

	res.conn = conn;
	res.methods = &http_methods;
	res.is_binary = false;
	return res;
}

/*
 * Disconnect any open connection for a connection cache entry.
 */
static void
http_disconnect(void *conn)
{
	if (conn != NULL)
		ch_http_close((ch_http_connection_t *) conn);
}

/*
 * Return text before version mentioning
 */
static char *
format_error(char *errstring)
{
	int n = strlen(errstring);

	for (int i = 0; i < n; i++)
	{
		if (strncmp(errstring + i, "version", 7) == 0)
			return pnstrdup(errstring, i - 2);
	}

	return errstring;
}

static void
kill_query(void *conn, const char *query_id)
{
	ch_http_response_t *resp;
	char *query = psprintf("kill query where query_id='%s'", query_id);

	ch_http_set_progress_func(NULL);
	resp = ch_http_simple_query(conn, query);
	if (resp != NULL)
		ch_http_response_free(resp);
	pfree(query);
}

static ch_cursor *
http_simple_query(void *conn, const char *query)
{
	int			attempts = 0;
	MemoryContext	tempcxt,
					oldcxt;
	ch_cursor	*cursor;
	ch_http_response_t *resp;

	ch_http_set_progress_func(http_progress_callback);

again:
	resp = ch_http_simple_query(conn, query);
	if (resp == NULL)
		elog(ERROR, "out of memory");

	attempts++;
	if (resp->http_status == 419)
	{
		char *error = pnstrdup(resp->data, resp->datasize);
		ch_http_response_free(resp);

		if (attempts < 3)
			goto again;

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("pg_clickhouse: communication error: %s", error)));
	}
	else if (resp->http_status == 418)
	{
		kill_query(conn, resp->query_id);
		ch_http_response_free(resp);

		ereport(ERROR,
				(errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
				 errmsg("pg_clickhouse: query was aborted")));
	}
	else if (resp->http_status != 200)
	{
		char *error = pnstrdup(resp->data, resp->datasize);
		long status =  resp->http_status;
		ch_http_response_free(resp);

		ereport(ERROR, (
			errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
			errmsg("pg_clickhouse: %s", format_error(error)),
			status < 404 ? 0 : errdetail_internal("Remote Query: %.64000s", query),
			errcontext("HTTP status code: %li", status)
		));
	}

	/* we could not control properly deallocation of libclickhouse memory, so
	 * we use memory context callbacks for that */
	tempcxt = AllocSetContextCreate(PortalContext, "pg_clickhouse cursor",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(tempcxt);

	cursor = palloc0(sizeof(ch_cursor));
	cursor->query_response = resp;
	cursor->read_state = palloc0(sizeof(ch_http_read_state));
	cursor->query = pstrdup(query);
	cursor->request_time = resp->pretransfer_time * 1000;
	cursor->total_time = resp->total_time * 1000;
	ch_http_read_state_init(cursor->read_state, resp->data, resp->datasize);

	cursor->memcxt = tempcxt;
	cursor->callback.func = http_cursor_free;
	cursor->callback.arg = cursor;
	MemoryContextRegisterResetCallback(tempcxt, &cursor->callback);
	MemoryContextSwitchTo(oldcxt);

	return cursor;
}

static void
http_simple_insert(void *conn, const char *query)
{
	ch_http_response_t *resp = ch_http_simple_query(conn, query);
	if (resp == NULL)
	{
		char *error = ch_http_last_error();
		if (error == NULL)
			error = "undefined";

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("pg_clickhouse: communication error: %s", error)));
	}

	if (resp->http_status != 200)
	{
		char *error = pnstrdup(resp->data, resp->datasize);
		long status =  resp->http_status;
		ch_http_response_free(resp);

		ereport(ERROR, (
			errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
			errmsg("pg_clickhouse: %s", format_error(error)),
			status < 404 ? 0 : errdetail_internal("Remote Query: %.64000s", query),
			errcontext("HTTP status code: %li", status)
		));
	}

	ch_http_response_free(resp);
}

static void
http_cursor_free(void *c)
{
	ch_cursor *cursor = c;

	ch_http_read_state_free(cursor->read_state);
	ch_http_response_free(cursor->query_response);
}

static void **
http_fetch_row(ch_cursor *cursor, List *attrs, TupleDesc tupdesc, Datum *v, bool *n)
{
	int		rc = CH_CONT;
	size_t	attcount = list_length(attrs);

	if (attcount == 0)
		/* SELECT NULL */
		attcount = 1;

	ch_http_read_state *state = cursor->read_state;

	/* all rows or empty table */
	if (state->done || state->data == NULL)
		return NULL;

	char **values = palloc(attcount * sizeof(char *));

	for (int i=0; i < attcount; i++)
	{
		rc = ch_http_read_next(state);
		if (state->val[0] == '\\' && state->val[1] == 'N')
			values[i] = NULL;
		else if (state->val[0] != '\0')
			values[i] = pstrdup(state->val);
		else
			values[i] = NULL;
	}

	if (attcount > 0 && rc != CH_EOL && rc != CH_EOF)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg_internal("pg_clickhouse: columns mismatch"),
				 errdetail("Number of returned columns does not match "
						   "expected column count (%lu).", attcount)));
	}

	return (void **) values;
}

text *
chfdw_http_fetch_raw_data(ch_cursor *cursor)
{
	ch_http_read_state *state = cursor->read_state;
	if (state->data == NULL)
		return NULL;

	return cstring_to_text_with_len(state->data, state->maxpos + 1);
}

/*
 * extend_insert_query
 *		Construct values part of INSERT query
 */
static void
extend_insert_query(ch_http_insert_state *state, TupleTableSlot *slot)
{
#ifdef USE_ASSERT_CHECKING
	int			pindex = 0;
#endif
	bool first = true;

	if (state->sql.len == 0)
		appendStringInfoString(&state->sql, state->sql_begin);

	/* get following parameters from slot */
	if (slot != NULL && state->target_attrs != NIL)
	{
		ListCell   *lc;

		foreach (lc, state->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Datum		value;
			Oid			type;
			bool		isnull;

			value = slot_getattr(slot, attnum, &isnull);
			type = TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->atttypid;

			if (!first)
				appendStringInfoChar(&state->sql, '\t');
			first = false;

			if (isnull)
			{
				appendStringInfoString(&state->sql, "\\N");
#ifdef USE_ASSERT_CHECKING
				pindex++;
#endif
				continue;
			}

			switch (type)
			{
			case BOOLOID:
			case INT2OID:
			case INT4OID:
				appendStringInfo(&state->sql, "%d", DatumGetInt32(value));
			break;
			case INT8OID:
				appendStringInfo(&state->sql, INT64_FORMAT, DatumGetInt64(value));
			break;
			case FLOAT4OID:
				appendStringInfo(&state->sql, "%f", DatumGetFloat4(value));
			break;
			case FLOAT8OID:
				appendStringInfo(&state->sql, "%f", DatumGetFloat8(value));
			break;
			case NUMERICOID:
			{
				char *extval = DatumGetCString(DirectFunctionCall1(numeric_out, value));
				appendStringInfoString(&state->sql, extval);
				pfree(extval);
			}
			break;
			case BPCHAROID:
			case VARCHAROID:
			case TEXTOID:
			case JSONOID:
			case NAMEOID:
			case BITOID:
			case BYTEAOID:
			{
				char   *strin, *strout;
				size_t  len;
				bool	tl = false;
				Oid		typoutput = InvalidOid;

				getTypeOutputInfo(type, &typoutput, &tl);
				strin = OidOutputFunctionCall(typoutput, value);
				len = strlen(strin) + 1;
				strout = palloc(len * 3 + 1);
				escape_string(strout, strin, len);
				appendStringInfo(&state->sql, "%s", strout);
				pfree(strout);
			}
			break;
			case DATEOID:
			{
				/* we expect Date on other side */
				char *extval = DatumGetCString(DirectFunctionCall1(ch_date_out, value));
				appendStringInfoString(&state->sql, extval);
				pfree(extval);
				break;
			}
			case TIMEOID:
			{
				/* we expect DateTime on other side */
				char *extval = DatumGetCString(DirectFunctionCall1(ch_time_out, value));
				appendStringInfo(&state->sql, "1970-01-01 %s", extval);
				pfree(extval);
				break;
			}
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
			{
				/* we expect DateTime on other side */
				char *extval = DatumGetCString(DirectFunctionCall1(ch_timestamp_out, value));
				appendStringInfoString(&state->sql, extval);
				pfree(extval);
				break;
			}
			default:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("cannot convert constant value to clickhouse value"),
								errhint("Constant value data type: %u", type)));
			}
#ifdef USE_ASSERT_CHECKING
			pindex++;
#endif
		}
		appendStringInfoChar(&state->sql, '\n');

		Assert(pindex == state->p_nums);
	}
}

static void *
http_prepare_insert(void *conn, ResultRelInfo *rri, List *target_attrs,
		char *query, char *table_name)
{
	ch_http_insert_state *state = palloc0(sizeof(ch_http_insert_state));

	initStringInfo(&state->sql);
	state->sql_begin = psprintf("%s FORMAT TSV\n", query);
	state->target_attrs = target_attrs;
	state->p_nums = list_length(state->target_attrs);
	state->conn = conn;

	return state;
}

static void
http_insert_tuple(void *istate, TupleTableSlot *slot)
{
	ch_http_insert_state *state = istate;

	extend_insert_query(state, slot);

	if ((slot == NULL && state->sql.len > 0)
			|| state->sql.len > (MaxAllocSize / 2 /* 512MB */))
	{
		http_simple_insert(state->conn, state->sql.data);
		resetStringInfo(&state->sql);
	}
}

/*** BINARY PROTOCOL ***/

ch_connection
chfdw_binary_connect(ch_connection_details *details)
{
	char *ch_error = NULL;
	ch_connection res;
	ch_binary_connection_t *conn = ch_binary_connect(details->host, details->port,
			details->dbname, details->username, details->password, &ch_error);

	if (conn == NULL)
	{
		Assert(ch_error);
		char *error = pstrdup(ch_error);
		free(ch_error);

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("pg_clickhouse: connection error: %s", error)));
	}

	res.conn = conn;
	res.methods = &binary_methods;
	res.is_binary = true;
	return res;
}

static void
binary_disconnect(void *conn)
{
	if (conn != NULL)
		ch_binary_close((ch_binary_connection_t *) conn);
}

static ch_cursor *
binary_simple_query(void *conn, const char *query)
{
	MemoryContext	tempcxt,
					oldcxt;
	ch_cursor	*cursor;
	ch_binary_read_state_t *state;

	ch_binary_response_t *resp = ch_binary_simple_query(conn, query, &is_canceled);

	if (!resp->success)
	{
		char *error = pstrdup(resp->error);
		ch_binary_response_free(resp);
		ereport(ERROR, (
			errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
			errmsg("pg_clickhouse: %s", error),
			errdetail_internal("Remote Query: %.64000s", query)
		));
	}

	tempcxt = AllocSetContextCreate(PortalContext, "pg_clickhouse cursor",
										ALLOCSET_DEFAULT_SIZES);

	oldcxt = MemoryContextSwitchTo(tempcxt);
	cursor = palloc0(sizeof(ch_cursor));
	cursor->query_response = resp;
	state = (ch_binary_read_state_t *) palloc0(sizeof(ch_binary_read_state_t));
	cursor->query = pstrdup(query);
	cursor->read_state = state;
	cursor->columns_count = resp->columns_count;
	ch_binary_read_state_init(cursor->read_state, resp);
	cursor->conversion_states = palloc0(sizeof(uintptr_t) * cursor->columns_count);

	cursor->memcxt = tempcxt;
	cursor->callback.func = binary_cursor_free;
	cursor->callback.arg = cursor;
	MemoryContextRegisterResetCallback(tempcxt, &cursor->callback);
	MemoryContextSwitchTo(oldcxt);

	if (state->error)
	{
		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("pg_clickhouse: could not initialize read state: %s",
					 state->error)));
	}

	return cursor;
}

static void **
binary_fetch_row(ch_cursor *cursor, List *attrs, TupleDesc tupdesc,
	Datum *values, bool *nulls)
{
	ListCell   *lc;
	ch_binary_read_state_t *state = cursor->read_state;
	bool		have_data = ch_binary_read_row(state);
	size_t		attcount = list_length(attrs);

	if (state->error)
		ereport(ERROR,
				(errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
				 errmsg("pg_clickhouse: error while reading row: %s",
					 state->error)));

	if (!have_data)
		return NULL;

	if (attcount == 0)
	{
		if (state->resp->columns_count == 1 && state->nulls[0])
		{
			nulls[0] = true;
			goto ok;
		}
		else
			elog(ERROR, "pg_clickhouse: unexpected state: attributes "
					"count == 0 and haven't got NULL in the response");
	}
	else if (attcount != state->resp->columns_count)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg_internal("pg_clickhouse: columns mismatch"),
				 errdetail("Number of returned columns (%lu) does not match "
						   "expected column count (%lu).",
						   state->resp->columns_count, attcount)));
	}

	if (tupdesc)
	{
		size_t j = 0;
		Assert(values && nulls);

		foreach(lc, attrs)
		{
			int		i = lfirst_int(lc);
			bool	isnull = state->nulls[j];
			intptr_t	convstate;


			if (isnull)
				values[i - 1] = (Datum) 0;
			else
			{
again:
				convstate = cursor->conversion_states[j];
				switch (convstate)
				{
					case 0:
					{
						MemoryContext old_mcxt;

						Oid outtype = TupleDescAttr(tupdesc, i - 1)->atttypid;
						void *s;

						/*
						 * now we're should be in temporary memory context,
						 * so make sure conversion states outlive it.
						 */
						old_mcxt = MemoryContextSwitchTo(cursor->memcxt);
						s = ch_binary_init_convert_state(state->values[j],
								state->coltypes[j], outtype);
						MemoryContextSwitchTo(old_mcxt);

						if (s == NULL)
							/* no conversion but state is initalized */
							cursor->conversion_states[j] = 1;
						else
							cursor->conversion_states[j] = (uintptr_t) s;
						goto again;
					}
					case 1:
						/* no conversion */
						values[i - 1] = state->values[j];
						break;
					default:
						values[i - 1] = ch_binary_convert_datum((void *) convstate,
								state->values[j]);
				}
			}

			nulls[i - 1] = isnull;
			j++;
		}
	}

ok:
	return (void **) state->values;
}

static void
binary_cursor_free(void *c)
{
	ch_cursor *cursor = c;
	for (size_t i = 0; i < cursor->columns_count; i++)
	{
		if (cursor->conversion_states[i] > 1)
			ch_binary_free_convert_state((void *) cursor->conversion_states[i]);
	}

	ch_binary_read_state_free(cursor->read_state);
	ch_binary_response_free(cursor->query_response);
}

static void *
binary_prepare_insert(void *conn, ResultRelInfo *rri, List *target_attrs,
		char *query, char *table_name)
{
	ch_binary_insert_state *state = NULL;
	MemoryContext	tempcxt,
					oldcxt;

	if (table_name == NULL)
		elog(ERROR, "expected table name");

	tempcxt = AllocSetContextCreate(CurrentMemoryContext,
		"pg_clickhouse binary insert state", ALLOCSET_DEFAULT_SIZES);

	/* prepare cleanup */
	oldcxt = MemoryContextSwitchTo(tempcxt);
	state = (ch_binary_insert_state *) palloc0(sizeof(ch_binary_insert_state));
	state->memcxt = tempcxt;
	state->callback.func = ch_binary_insert_state_free;
	state->callback.arg = state;
	state->conn = conn;
	state->table_name = pstrdup(table_name);
	MemoryContextRegisterResetCallback(tempcxt, &state->callback);

	/* time for c++ stuff */
	ch_binary_prepare_insert(conn, query, state);

	/* buffers */
	state->values = (Datum *) palloc0(sizeof(Datum) * state->len);
	state->nulls = (bool *) palloc0(sizeof(bool) * state->len);
	MemoryContextSwitchTo(oldcxt);

	return state;
}

static void
binary_insert_tuple(void *istate, TupleTableSlot *slot)
{
	ch_binary_insert_state *state = istate;

	if (state->conversion_states == NULL)
	{
		MemoryContext	old_mcxt;

		old_mcxt = MemoryContextSwitchTo(state->memcxt);
		state->conversion_states = ch_binary_make_tuple_map(
				slot->tts_tupleDescriptor, state->outdesc);
		MemoryContextSwitchTo(old_mcxt);
	}

	if (slot)
	{
		ch_binary_do_output_convertion(state, slot);

		for (size_t i = 0; i < state->outdesc->natts; i++)
			ch_binary_column_append_data(state, i);
	}
	else
	{
		ch_binary_insert_columns(state);
	}
}

static char *str_types_map[][2] = {
	{"Bool", "BOOLEAN"},
	{"Int8", "INT2"},
	{"UInt8", "INT2"},
	{"Int16", "INT2"},
	{"UInt16", "INT4"},
	{"Int32", "INT4"},
	{"UInt32", "INT8"},
	{"Int64", "INT8"},
	{"UInt64", "INT8"}, //overflow risk
	{"Float32", "REAL"},
	{"Float64", "DOUBLE PRECISION"},
	{"Decimal", "NUMERIC"},
	{"Boolean", "BOOLEAN"},
	{"String", "TEXT"},
	{"DateTime", "TIMESTAMP"},
	{"Date", "DATE"}, // important that this one is after other Date types
	{"UUID", "UUID"},
	{"IPv4", "inet"},
	{"IPv6", "inet"},
	{"JSON", "JSON"},
	{NULL, NULL},
};

static char *
readstr(ch_connection conn, char *val)
{
	if (conn.is_binary)
		return TextDatumGetCString(PointerGetDatum(val));
	else
		return val;
}

static char *
parse_type(char *table_name, char *colname, char *typepart, bool *is_nullable, List **options)
{
	char *pos = strstr(typepart, "(");

	if (pos != NULL)
	{
		char *insidebr = pnstrdup(pos + 1, strrchr(typepart, ')') - pos - 1);

		if (strncmp(typepart, "Decimal", strlen("Decimal")) == 0)
		{
			if (strstr(insidebr, ",") == NULL)
				elog(ERROR, "pg_clickhouse: could not import Decimal field, "
					"should be two parameters on definition");

			return psprintf("NUMERIC(%s)", insidebr);
		}
		else if (strncmp(typepart, "FixedString", strlen("FixedString")) == 0)
			return psprintf("VARCHAR(%s)", insidebr);
		else if (strncmp(typepart, "Enum8", strlen("Enum8")) == 0)
			return "TEXT";
		else if (strncmp(typepart, "Enum16", strlen("Enum16")) == 0)
			return "TEXT";
		else if (strncmp(typepart, "DateTime64", strlen("DateTime64")) == 0)
			return "TIMESTAMP";
		else if (strncmp(typepart, "DateTime", strlen("DateTime")) == 0)
			return "TIMESTAMP";
		else if (strncmp(typepart, "Tuple", strlen("Tuple")) == 0)
		{
			elog(NOTICE, "pg_clickhouse: ClickHouse <Tuple> type was "
				"translated to <TEXT> type for column \"%s\", please create composite type and alter the column if needed", colname);
			return "TEXT";
		}
		else if (strncmp(typepart, "Array", strlen("Array")) == 0)
		{
			return psprintf("%s[]", parse_type(table_name, colname, insidebr, NULL, options));
		}
		else if (strncmp(typepart, "Nullable", strlen("Nullable")) == 0)
		{
			if (is_nullable == NULL)
				elog(ERROR, "pg_clickhouse: nested Nullable is not supported");

			*is_nullable = true;
			return parse_type(table_name, colname, insidebr, NULL, options);
		}
		else if (strncmp(typepart, "LowCardinality", strlen("LowCardinality")) == 0)
		{
			return parse_type(table_name, colname, insidebr, is_nullable, options);
		}
		else if (strncmp(typepart, "AggregateFunction", strlen("AggregateFunction")) == 0 ||
				 strncmp(typepart, "SimpleAggregateFunction", strlen("SimpleAggregateFunction")) == 0)
		{
			char *pos2 = strstr(pos, ",");
			if (pos2 == NULL) {
				// Detect COUNT with no params.
				if (strncmp(insidebr, "count", strlen("count")) == 0)
					return "BIGINT";
				elog(ERROR, "pg_clickhouse: expected comma in AggregateFunction");
			}

			char *func = pnstrdup(pos + 1, strstr(pos + 1, ",") - pos - 1);

			if (options != NULL)
			{
				int val = typepart[0] == 'A' ? 1 : 2;
				*options = lappend(*options, makeInteger(val));
				*options = lappend(*options, makeString(func));
			}

			if (strncmp(func, "sumMap", 6) == 0)
				return "istore";

			return parse_type(table_name, colname, pos2 + 2, is_nullable, options);
		}

		typepart = pos + 1;
	}

	size_t i = 0;
	while (str_types_map[i][0] != NULL)
	{
		if (strncmp(str_types_map[i][0], typepart, strlen(str_types_map[i][0])) == 0)
			return pstrdup(str_types_map[i][1]);
		i++;
	}

	ereport(ERROR, (errmsg(
		"pg_clickhouse: could not map %s.%s type <%s>",
		table_name, colname, typepart
	)));
}

List *
chfdw_construct_create_tables(ImportForeignSchemaStmt *stmt, ForeignServer *server)
{
	Oid				userid = GetUserId();
	UserMapping	   *user = GetUserMapping(userid, server->serverid);
	ch_connection	conn = chfdw_get_connection(user);
	ch_cursor	   *cursor;
	char		   *query;
	List		   *result = NIL,
				   *datts = NIL;
	char		  **row_values;

	query = psprintf("SELECT name, engine, engine_full "
			"FROM system.tables WHERE database='%s' and name not like '.inner%%'", stmt->remote_schema);
	cursor = conn.methods->simple_query(conn.conn, query);

	datts = list_make2_int(1,2);

	while ((row_values = (char **) conn.methods->fetch_row(cursor,
				list_make3_int(1,2,3), NULL, NULL, NULL)) != NULL)
	{
		StringInfoData	buf;
		ch_cursor  *table_def;
		char	   *table_name = readstr(conn, row_values[0]);
		char	   *engine = readstr(conn, row_values[1]);
		char	   *engine_full = readstr(conn, row_values[2]);
		char	  **dvalues;
		bool		first = true;

		if (table_name == NULL)
			continue;

		if (list_length(stmt->table_list))
		{
			ListCell *lc;
			bool found = false;

			foreach(lc, stmt->table_list)
			{
				RangeVar   *rv = (RangeVar *) lfirst(lc);
				if (strcmp(rv->relname, table_name) == 0)
					found = true;
			}

			if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT && found)
				continue;
			else if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO && !found)
				continue;
		}

		initStringInfo(&buf);
		appendStringInfo(&buf, "CREATE FOREIGN TABLE IF NOT EXISTS \"%s\".\"%s\" (\n",
			stmt->local_schema, table_name);
		query = psprintf("select name, type from system.columns where database='%s' and table='%s'",
            stmt->remote_schema, table_name);
		table_def = conn.methods->simple_query(conn.conn, query);

		while ((dvalues = (char **) conn.methods->fetch_row(table_def,
			datts, NULL, NULL, NULL)) != NULL)
		{
			List   *options = NIL;
			bool	is_nullable = false;
			char   *colname = readstr(conn, dvalues[0]);
			char   *remote_type = parse_type(table_name, colname, readstr(conn, dvalues[1]), &is_nullable, &options);

			if (!first)
				appendStringInfoString(&buf, ",\n");
			first = false;

			/* name */
			appendStringInfo(&buf, "\t\"%s\" ", colname);

			/* type */
			appendStringInfoString(&buf, remote_type);

			if (options != NIL)
			{
				bool first_opt = true;
				ListCell *lc;

				appendStringInfoString(&buf, " OPTIONS (");
				foreach(lc, options)
				{
					Node	*val = lfirst(lc);
					if (IsA(val, Integer))
					{
						if (!first_opt)
							appendStringInfoString(&buf, ", ");
						first_opt = false;
						switch intVal(val) {
							case 1:
								appendStringInfoString(&buf, "AggregateFunction");
								break;
							case 2:
								appendStringInfoString(&buf, "SimpleAggregateFunction");
								break;
							default:
								elog(ERROR, "programming error");
						}
					}
					else
						appendStringInfo(&buf, " '%s'", strVal(val));
				}
				appendStringInfoString(&buf, ")");
				list_free_deep(options);
			}

			if (!is_nullable)
				appendStringInfoString(&buf, " NOT NULL");
		}

		appendStringInfo(&buf, "\n) SERVER %s OPTIONS (database '%s', table_name '%s'",
			server->servername, stmt->remote_schema, table_name);

		if (engine && engine_full && strcmp(engine, "CollapsingMergeTree") == 0)
		{
			char *sub = strstr(engine_full, ")");
			if (sub)
			{
				sub[1] = '\0';
				appendStringInfo(&buf, ", engine '%s'", engine_full);
			}
		}
		else if (engine)
			appendStringInfo(&buf, ", engine '%s'", engine);

		appendStringInfoString(&buf, ");\n");
		result = lappend(result, buf.data);
		MemoryContextDelete(table_def->memcxt);
	}

	MemoryContextDelete(cursor->memcxt);
	return result;
}


/*
 * Escaping arbitrary strings to get valid SQL literal strings.
 *
 * length is the length of the source string.  (Note: if a terminating NUL
 * is encountered sooner, escape_string stops short of "length"; the behavior
 * is thus rather like strncpy.)
 *
 * For safety the buffer at "to" must be at least 2*length + 1 bytes long.
 * A terminating NUL character is added to the output string, whether the
 * input is NUL-terminated or not.
 *
 * Returns the actual length of the output (not counting the terminating NUL).
 */
static size_t
escape_string(char *to, const char *from, size_t length)
{
	const char *source = from;
	char	   *target = to;
	size_t		remaining = length;

	while (remaining > 0 && *source != '\0')
	{
		char		c = *source;
		int			len;
		int			i;

		/* Fast path for plain ASCII */
		if (!IS_HIGHBIT_SET(c))
		{
			/* Apply quoting if needed */
			if (c == '\\')
			{
				*target++ = c;
				*target++ = c;
			}
			else if (c == '\'')
			{
				*target++ = '\\';
				*target++ = c;
			}
			else if (c == '\n')
			{
				*target++ = '\\';
				*target++ = 'n';
			}
			else if (c == '\t')
			{
				*target++ = '\\';
				*target++ = 't';
			}
			else if (c == '\0')
			{
				*target++ = '\\';
				*target++ = '0';
			}
			else if (c == '\r')
			{
				*target++ = '\\';
				*target++ = 'r';
			}
			else if (c == '\b')
			{
				*target++ = '\\';
				*target++ = 'b';
			}
			else if (c == '\f')
			{
				*target++ = '\\';
				*target++ = 'f';
			}
			else
				*target++ = c;

			source++;
			remaining--;
			continue;
		}

		/* Slow path for possible multibyte characters */
		len = pg_encoding_mblen(PG_SQL_ASCII, source);

		/* Copy the character */
		for (i = 0; i < len; i++)
		{
			if (remaining == 0 || *source == '\0')
				break;
			*target++ = *source++;
			remaining--;
		}

		if (i < len)
			elog(ERROR, "pg_clickhouse: incomplete multibyte character");
	}

	/* Write the terminating NUL character. */
	*target = '\0';

	return target - to;
}
