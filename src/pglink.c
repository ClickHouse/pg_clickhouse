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
#include "utils/date.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/uuid.h"

#include "binary.h"
#include "cursor.h"
#include "fdw.h"
#include "http.h"
#include "pg-clickhouse-decode.h"
#include "pg-clickhouse-encode.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Errors reach the CH_ERROR_MSG_LEN buffers through chc_err.msg, which must
 * hold them whole. Makefile sets CHC_ERR_MSG_LEN.
 */
StaticAssertDecl(
    CHC_ERR_MSG_LEN >= CH_ERROR_MSG_LEN,
    "chc_err.msg clips ClickHouse errors"
);

static bool initialized = false;

/*
 * This threshold lets bulk loads stream instead of buffering every row.
 * ClickHouse can combine small blocks within one INSERT according to the
 * min_insert_block_size_rows and min_insert_block_size_bytes settings.
 */
#define INSERT_FLUSH_BYTES (64 * 1024 * 1024)

/* Rows buffered for one Native INSERT over HTTP. */
typedef struct {
    char* sql;       /* INSERT statement, for error reporting */
    char* sql_begin; /* sql plus the FORMAT clause the body follows */
    pgch_writer* writer;
    AttrNumber* attnums; /* slot attribute feeding each column */
    Oid* atttypids;
    size_t ncols;
    ch_http_connection_t* conn;
} ch_http_insert_state;

static void
http_disconnect(void* conn);
static void
http_simple_insert(void* conn, const ch_query* query);
static ch_cursor*
http_native_cursor(void* conn, const ch_query* query);
static void
http_native_read_error(ch_cursor* cursor);
static void*
http_prepare_insert(void*, ResultRelInfo*, List*, const ch_query*, char*);
static void
http_insert_tuple(void*, TupleTableSlot*);
static void
report_http_stream_query_failure(void* conn, const ch_query* query, HttpStream* stream);
static ch_server_version
http_server_version(void* conn);

static libclickhouse_methods http_methods = {
    .disconnect          = http_disconnect,
    .simple_query        = http_native_cursor,
    .fetch_row           = chfdw_cursor_fetch_row,
    .prepare_insert      = http_prepare_insert,
    .insert_tuple        = http_insert_tuple,
    .streaming_query     = http_native_cursor,
    .streaming_fetch_row = chfdw_cursor_fetch_row,
    .server_version      = http_server_version,
};

static void
binary_disconnect(void* conn);
static ch_cursor*
binary_simple_query(void* conn, const ch_query* query);
static bool
binary_is_broken(const void* conn);

/* static void binary_simple_insert(void *conn, const char *query); */
static void
binary_insert_tuple(void*, TupleTableSlot* slot);
static void
binary_finalize_insert(void* istate);
static void*
binary_prepare_insert(
    void*,
    ResultRelInfo*,
    List*,
    const ch_query* query,
    char* table_name
);
static char*
ch_escape_string(const char* s, size_t len);
static void
ch_quote_literal_internal(char* dst, const char* src, size_t len);
extern char*
ch_quote_literal(const char* rawstr);
extern const char*
ch_quote_ident(const char* rawstr);
static ch_server_version
binary_server_version(void* conn);

static libclickhouse_methods binary_methods = {
    .disconnect          = binary_disconnect,
    .simple_query        = binary_simple_query,
    .fetch_row           = chfdw_cursor_fetch_row,
    .prepare_insert      = binary_prepare_insert,
    .insert_tuple        = binary_insert_tuple,
    .finalize_insert     = binary_finalize_insert,
    .streaming_query     = NULL,
    .streaming_fetch_row = NULL,
    .is_broken           = binary_is_broken,
    .server_version      = binary_server_version,
};

/* ch_cancel_check for the HTTP transport, polled while a request is in flight. */
static bool
http_canceled(void) {
    return QueryCancelPending || ProcDiePending;
}

static bool
is_canceled(void) {
    if (QueryCancelPending) {
        return true;
    }

    return false;
}

ch_connection
chfdw_http_connect(ch_connection_details* details) {
    ch_connection res;
    ch_http_connection_t* conn;
    const char* error;

    if (!initialized) {
        initialized = true;
        ch_http_init(0);
    }

    /*
     * Since http.c will set the database name in a plain text header, we
     * cannot allow line endings because they could allow header injection.
     */
    if (details->dbname) {
        for (char* c = details->dbname; *c != '\0'; c++) {
            if (*c == '\n' || *c == '\r') {
                ereport(
                    ERROR,
                    errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
                    errmsg(
                        "pg_clickhouse: unsupported line ending character in database "
                        "name"
                    ),
                    errdetail(
                        "Invalid database name: %s", ch_quote_literal(details->dbname)
                    )
                );
            }
        }
    }

    conn = ch_http_connection(details, &error);
    if (conn == NULL) {
        ereport(
            ERROR,
            errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
            errmsg("could not connect to server: %s", error)
        );
    }

    res.conn    = conn;
    res.methods = &http_methods;
    return res;
}

/*
 * Disconnect any open connection for a connection cache entry.
 */
static void
http_disconnect(void* conn) {
    if (conn != NULL) {
        ch_http_close((ch_http_connection_t*)conn);
    }
}

static ch_server_version
http_server_version(void* conn) {
    return ch_http_server_version((ch_http_connection_t*)conn, http_canceled);
}

static void
kill_query(void* conn, const char* query_id) {
    ch_http_response_t* resp;
    ch_query query = new_query(
        psprintf("kill query where query_id=%s", ch_quote_literal(query_id)),
        0,
        NULL,
        NULL,
        NULL
    );

    /* Not cancellable: it's the cleanup for an already cancelled query. */
    resp = ch_http_simple_query(conn, &query, NULL);
    if (resp != NULL) {
        ch_http_response_free(resp);
    }
}

static void
report_http_stream_query_failure(
    void* conn,
    const ch_query* query,
    HttpStream* stream
) {
    long status = ch_http_stream_status(stream);

    PG_TRY();
    {
        if (status == CH_HTTP_STATUS_CANCELED) {
            char qid[CH_HTTP_QUERY_ID_LEN];

            memcpy(qid, ch_http_stream_query_id(stream), sizeof(qid));
            kill_query(conn, qid);
            ereport(
                ERROR,
                errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
                errmsg("pg_clickhouse: query was aborted")
            );
        } else if (status == CH_HTTP_STATUS_TRANSPORT_ERROR) {
            const char* err = ch_http_stream_error(stream);

            ereport(
                ERROR,
                errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
                errmsg(
                    "pg_clickhouse: communication error: %s",
                    err ? err : "connection error"
                )
            );
        } else {
            char error[CH_ERROR_MSG_LEN];

            ch_http_copy_error(
                error,
                sizeof(error),
                ch_http_stream_buffer(stream),
                ch_http_stream_available(stream)
            );

            ereport(
                ERROR,
                errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
                errmsg("pg_clickhouse: %s", error),
                status < 404 ? 0
                             : errdetail_internal("Remote Query: %.64000s", query->sql),
                errcontext("HTTP status code: %li", status)
            );
        }
    }
    PG_FINALLY();
    { ch_http_stream_end(stream); }
    PG_END_TRY();
}

static void
http_simple_insert(void* conn, const ch_query* query) {
    ch_http_response_t* resp = ch_http_simple_query(conn, query, http_canceled);

    if (resp == NULL) {
        ereport(ERROR, errcode(ERRCODE_FDW_OUT_OF_MEMORY), errmsg("out of memory"));
    }

    if (resp->http_status == CH_HTTP_STATUS_CANCELED) {
        kill_query(conn, resp->query_id);
        ch_http_response_free(resp);

        ereport(
            ERROR,
            errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
            errmsg("pg_clickhouse: query was aborted")
        );
    }

    if (!ch_http_status_ok(resp->http_status)) {
        char error[CH_ERROR_MSG_LEN];
        long status = resp->http_status;

        ch_http_copy_error(error, sizeof(error), resp->data, resp->datasize);
        ch_http_response_free(resp);

        ereport(
            ERROR,
            errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
            errmsg("pg_clickhouse: %s", error),
            status < 404 ? 0 : errdetail_internal("Remote Query: %.64000s", query->sql),
            errcontext("HTTP status code: %li", status)
        );
    }

    ch_http_response_free(resp);
}

/* pgch_chunk_source cancellation poll, checked between reads. */
static bool
native_chunks_cancelled(void* ud pg_attribute_unused()) {
    return http_canceled();
}

/* Room for every setting native_overrides writes. */
#define NATIVE_OVERRIDES_MAX 3

/*
 * Settings the shared decoder needs from a Native response: the pair
 * PGCH_NATIVE_SETTINGS joins, plus the format itself. Listed one by one
 * because each needs its own server version gate; an unknown HTTP setting
 * fails the query.
 */
static int
native_overrides(void* conn, ch_setting out[NATIVE_OVERRIDES_MAX]) {
    ch_server_version version =
        ch_http_server_version((ch_http_connection_t*)conn, http_canceled);
    int n = 0;

    /* Format as a setting keeps SQL unchanged, so query parameters work. */
    out[n++] = (ch_setting){ "default_format", "Native" };
    if (chfdw_version_ge(version, 24, 7)) {
        out[n++] =
            (ch_setting){ "output_format_native_encode_types_in_binary_format", "0" };
    }
    if (chfdw_version_ge(version, 24, 10)) {
        out[n++] = (ch_setting){ "output_format_native_write_json_as_string", "1" };
    }

    return n;
}

static void
http_stream_reader_init(pgch_reader* reader, void* response) {
    pgch_chunk_source src = {
        .ud         = response,
        .next_chunk = ch_http_stream_next_chunk,
        .cancelled  = native_chunks_cancelled,
    };

    pgch_reader_init_chunks(reader, &src, NULL);
}

static void
http_stream_free(void* response) {
    ch_http_stream_end(response);
}

/* Create shared-decoder cursor over HTTP Native response. */
static ch_cursor*
http_native_cursor(void* conn, const ch_query* query) {
    int attempts = 0;
    HttpStream* stream;
    ch_cursor* cursor;
    ch_setting overrides[NATIVE_OVERRIDES_MAX];
    ch_http_request req = {
        .query         = query,
        .overrides     = overrides,
        .num_overrides = native_overrides(conn, overrides),
        .stream_chunks = true,
        .cancel        = http_canceled,
    };

again:
    stream = ch_http_stream_begin(conn, &req);
    if (stream == NULL) {
        ereport(
            ERROR,
            errcode(ERRCODE_INTERNAL_ERROR),
            errmsg("pg_clickhouse: failed to initialize HTTP stream")
        );
    }

    attempts++;
    if (ch_http_stream_status(stream) == CH_HTTP_STATUS_TRANSPORT_ERROR &&
        attempts < 3) {
        ch_http_stream_end(stream);
        goto again;
    }
    if (!ch_http_status_ok(ch_http_stream_status(stream))) {
        report_http_stream_query_failure(conn, query, stream);
    }

    ch_cursor_source src = {
        .response             = stream,
        .init_reader          = http_stream_reader_init,
        .free_response        = http_stream_free,
        .raise_response_error = http_native_read_error,
    };

    cursor               = chfdw_cursor_open(conn, query, &src);
    cursor->request_time = ch_http_stream_request_time(stream);

    return cursor;
}

/*
 * Convert a Datum to a ClickHouse literal string. Returns NULL if the value
 * cannot be converted to a literal.
 */
extern char*
chfdw_datum_to_ch_literal(Datum value, Oid type) {
    if (type_is_array(type)) {
        return chfdw_array_to_ch_literal(value);
    }

    switch (type) {
    case BOOLOID:
    case INT2OID:
    case INT4OID:
        return psprintf("%d", DatumGetInt32(value));
    case INT8OID:
        return psprintf(INT64_FORMAT, DatumGetInt64(value));
    case FLOAT4OID:
        return psprintf("%f", DatumGetFloat4(value));
    case FLOAT8OID:
        return psprintf("%f", DatumGetFloat8(value));
    case NUMERICOID:
        return DatumGetCString(DirectFunctionCall1(numeric_out, value));
    case BPCHAROID:
    case VARCHAROID:
    case TEXTOID:
    case JSONOID:
    case JSONBOID:
    case NAMEOID:
    case BITOID:
    case UUIDOID:
    case INETOID: {
        char* text;
        bool tl       = false;
        Oid typoutput = InvalidOid;

        getTypeOutputInfo(type, &typoutput, &tl);
        text = OidOutputFunctionCall(typoutput, value);
        return ch_escape_string(text, strlen(text));
    }
    case BYTEAOID: {
        /* Copy all of the bytes into a ClickHouse literal string. */
        bytea* bytes = PG_DETOAST_DATUM(value);

        return ch_escape_string(VARDATA(bytes), VARSIZE_ANY_EXHDR(bytes));
    }
    case DATEOID:
        /* we expect Date on other side */
        return DatumGetCString(DirectFunctionCall1(ch_date_out, value));
    case TIMEOID: {
        /* we expect DateTime on other side */
        char* extval = DatumGetCString(DirectFunctionCall1(ch_time_out, value));
        char* retval = psprintf("1970-01-01 %s", extval);

        pfree(extval);
        return retval;
    }
    case TIMESTAMPOID:
    case TIMESTAMPTZOID:
        /* we expect DateTime on other side */
        return DatumGetCString(DirectFunctionCall1(ch_timestamp_out, value));
    default:
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
            errmsg("cannot convert value to clickhouse value"),
            errhint("Value data type: %u", type)
        );
    }
}

/*
 * Serialize buffered rows as a Native block and POST them.
 *
 * Column types come from PostgreSQL, so they rarely match the destination
 * exactly. ClickHouse casts them per column name under
 * input_format_native_allow_types_conversion, on by default since 23.3.
 */
static void
http_flush_insert(ch_http_insert_state* state) {
    pgch_buf body = {};

    if (pgch_writer_rows(state->writer) == 0) {
        return;
    }

    pgch_buf_append(&body, state->sql_begin, strlen(state->sql_begin));
    /* NULL opts: no block info or custom serialization, matching the reader. */
    pgch_writer_flush(state->writer, &body, NULL);

    ch_query query = new_body_query(state->sql, body.data, body.len);

    http_simple_insert(state->conn, &query);
    pgch_buf_reset(&body);
}

static void*
http_prepare_insert(
    void* conn,
    ResultRelInfo* rri,
    List* target_attrs,
    const ch_query* query,
    char* table_name
) {
    ch_http_insert_state* state = palloc0(sizeof(ch_http_insert_state));
    Relation rel                = rri->ri_RelationDesc;
    TupleDesc tupdesc           = RelationGetDescr(rel);
    Oid relid                   = RelationGetRelid(rel);
    size_t ncols                = list_length(target_attrs);
    pgch_col* cols              = palloc0(ncols * sizeof(pgch_col));
    ListCell* lc;
    size_t i = 0;

    state->ncols     = ncols;
    state->attnums   = palloc0(ncols * sizeof(AttrNumber));
    state->atttypids = palloc0(ncols * sizeof(Oid));

    foreach (lc, target_attrs) {
        AttrNumber attnum       = lfirst_int(lc);
        Form_pg_attribute attr  = TupleDescAttr(tupdesc, attnum - 1);
        CustomColumnInfo* cinfo = chfdw_get_custom_column_info(relid, attnum);
        /* Name must match the INSERT column list chfdw_deparse_insert_sql built */
        const char* colname =
            (cinfo && cinfo->colname[0]) ? cinfo->colname : NameStr(attr->attname);
        const char* chtype;
        chc_err err = {};
        chc_type* coltype;

        /*
         * ClickHouse gained Time64 in 25.6 and casts it to none of the types
         * a table holds a time of day in, so send a timestamp on the epoch
         * date, as the TabSeparated payload did.
         */
        if (attr->atttypid == TIMEOID) {
            chtype = attr->attnotnull ? "DateTime64(6, 'UTC')"
                                      : "Nullable(DateTime64(6, 'UTC'))";
        } else {
            chtype = pgch_ch_type_for(
                attr->atttypid, attr->atttypmod, attr->attnotnull, NULL
            );
        }

        /*
         * A PostgreSQL array type carries no dimension count, only the
         * declared attndims does, and ClickHouse nests one Array per
         * dimension.
         */
        for (int dim = 1; dim < attr->attndims; dim++) {
            chtype = psprintf("Array(%s)", chtype);
        }

        if (chc_type_parse(chtype, strlen(chtype), &pgch_alloc, &coltype, &err) !=
            CHC_OK) {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg(
                    "pg_clickhouse: could not build ClickHouse type for column \"%s\"",
                    colname
                ),
                errdetail_internal("%s: %s", chtype, err.msg)
            );
        }

        state->attnums[i]   = attnum;
        state->atttypids[i] = attr->atttypid;
        cols[i].name        = colname;
        cols[i].name_len    = strlen(colname);
        cols[i].type        = coltype;
        i++;
    }

    state->writer    = pgch_writer_new(CurrentMemoryContext, cols, ncols);
    state->sql       = pstrdup(query->sql);
    state->sql_begin = psprintf("%s FORMAT Native\n", query->sql);
    state->conn      = conn;

    return state;
}

static void
http_insert_tuple(void* istate, TupleTableSlot* slot) {
    ch_http_insert_state* state = istate;

    if (slot != NULL) {
        for (size_t i = 0; i < state->ncols; i++) {
            bool isnull;
            Datum value = slot_getattr(slot, state->attnums[i], &isnull);
            Oid valtype = state->atttypids[i];

            /*
             * PostgreSQL casts inet to text through network_show, which
             * appends a netmask ClickHouse rejects for IPv4 and IPv6. The
             * output function omits it for single hosts.
             */
            if (valtype == INETOID && !isnull) {
                value   = CStringGetTextDatum(OidOutputFunctionCall(F_INET_OUT, value));
                valtype = TEXTOID;
            } else if (valtype == TIMEOID && !isnull) {
                /* Pair with the DateTime64 column that http_prepare_insert declares. */
                value = TimestampTzGetDatum(
                    DatumGetTimeADT(value) -
                    (TimestampTz)(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) *
                        USECS_PER_DAY
                );
                valtype = TIMESTAMPTZOID;
            }

            pgch_append_datum(state->writer, i, value, valtype, isnull);
        }

        if (pgch_writer_bytes(state->writer) < INSERT_FLUSH_BYTES) {
            return;
        }
    }
    http_flush_insert(state);
}

/*** BINARY PROTOCOL ***/

ch_connection
chfdw_binary_connect(ch_connection_details* details) {
    ch_connection res;

    res.conn    = ch_binary_connect(details);
    res.methods = &binary_methods;
    return res;
}

static void
binary_disconnect(void* conn) {
    if (conn != NULL) {
        ch_binary_close((ch_binary_connection_t*)conn);
    }
}

static bool
binary_is_broken(const void* conn) {
    return ch_binary_is_broken((const ch_binary_connection_t*)conn);
}

static ch_server_version
binary_server_version(void* conn) {
    ch_server_version v = { 0, 0, 0 };

    ch_binary_server_version(
        (ch_binary_connection_t*)conn, &v.major, &v.minor, &v.patch
    );
    return v;
}

static void
binary_reader_init(pgch_reader* reader, void* response) {
    pgch_block_source src = ch_binary_response_block_source(response);

    pgch_reader_init(reader, &src);
}

static void
binary_response_free(void* response) {
    ch_binary_response_free(response);
}

static ch_cursor*
binary_simple_query(void* conn, const ch_query* query) {
    ch_binary_response_t* resp = ch_binary_simple_query(conn, query, &is_canceled);
    const char* msg            = ch_binary_response_error(resp);

    if (msg) {
        char error[CH_ERROR_MSG_LEN];
        int clip, n;

        /* Clip short of a split multibyte character; exceptions are UTF-8 */
        clip = (int)Min(strlen(msg), sizeof(error) - 1);
        n    = pg_encoding_mbcliplen(PG_UTF8, msg, clip, clip);
        memcpy(error, msg, n);
        error[n] = '\0';

        ch_binary_response_free(resp);

        /* Prefer consistent interrupt error message when query interrupted */
        CHECK_FOR_INTERRUPTS();
        ereport(
            ERROR,
            errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
            errmsg("pg_clickhouse: %s", error),
            errdetail_internal("Remote Query: %.64000s", query->sql)
        );
    }

    ch_cursor_source src = {
        .response      = resp,
        .init_reader   = binary_reader_init,
        .free_response = binary_response_free,
    };

    return chfdw_cursor_open(conn, query, &src);
}

/* Report a truncated response as cancellation when that is what caused it. */
static void
http_native_read_error(ch_cursor* cursor) {
    HttpStream* stream = cursor->response;

    if (stream == NULL) {
        return;
    }

    if (ch_http_stream_status(stream) == CH_HTTP_STATUS_CANCELED ||
        QueryCancelPending || ProcDiePending) {
        char qid[CH_HTTP_QUERY_ID_LEN];

        memcpy(qid, ch_http_stream_query_id(stream), sizeof(qid));
        /* Drop the transfer before asking the server to kill the query. */
        ch_http_stream_end(stream);
        cursor->response = NULL;
        kill_query(cursor->conn, qid);
        ereport(
            ERROR,
            errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
            errmsg("pg_clickhouse: query was aborted")
        );
    }
}

/* Maps one ClickHouse column to its PostgreSQL slot attribute. */
typedef struct {
    AttrNumber attnum;
    Oid atttypid;
} binary_insert_colmap;

/* Tracks rows buffered for one Native INSERT over the native protocol. */
typedef struct {
    MemoryContext memcxt; /* Holds state across per-tuple context resets. */
    MemoryContextCallback callback;

    ch_binary_insert_handle* handle;
    size_t ncols;                 /* Number of ClickHouse columns. */
    binary_insert_colmap* colmap; /* Contains ncols entries after the first tuple. */
    Oid relid;                    /* Foreign table used for column_name lookups. */
} binary_insert_state;

/*
 * Matches each ClickHouse column to an input attribute by name and honors the
 * column_name FDW option. Uses positions when the first input attribute has no
 * name, because that descriptor contains no attribute names.
 */
static void
build_insert_colmap(binary_insert_state* state, TupleDesc indesc) {
    ch_binary_insert_handle* h = state->handle;
    bool positional =
        indesc->natts > 0 && NameStr(TupleDescAttr(indesc, 0)->attname)[0] == '\0';

    state->colmap = palloc0(state->ncols * sizeof(binary_insert_colmap));

    for (size_t i = 0; i < state->ncols; i++) {
        binary_insert_colmap* m = &state->colmap[i];
        const char* chname      = ch_binary_insert_column_name(h, i);

        if (positional) {
            if (i < (size_t)indesc->natts) {
                m->attnum = (AttrNumber)(i + 1);
            }
        } else {
            for (int j = 0; j < indesc->natts; j++) {
                Form_pg_attribute attin = TupleDescAttr(indesc, j);
                CustomColumnInfo* cinfo;
                const char* inname;

                if (attin->attisdropped) {
                    continue;
                }

                /* Prefer the column_name FDW option over the attribute name. */
                cinfo  = OidIsValid(state->relid)
                             ? chfdw_get_custom_column_info(state->relid, j + 1)
                             : NULL;
                inname = (cinfo && cinfo->colname[0]) ? cinfo->colname
                                                      : NameStr(attin->attname);

                if (strcmp(chname, inname) == 0) {
                    m->attnum = (AttrNumber)(j + 1);
                    break;
                }
            }
        }

        if (m->attnum == 0) {
            ereport(
                ERROR,
                errcode(ERRCODE_DATATYPE_MISMATCH),
                errmsg_internal("pg_clickhouse: could not create conversion map"),
                errdetail(
                    "ClickHouse column \"%s\" has no matching attribute in type %s.",
                    chname,
                    format_type_be(indesc->tdtypeid)
                )
            );
        }
        m->atttypid = TupleDescAttr(indesc, m->attnum - 1)->atttypid;
    }
}

static void
binary_insert_state_free(void* c) {
    binary_insert_state* state = c;

    if (state->handle == NULL) {
        return;
    }

    /*
     * MemoryContext reset callbacks cannot report errors. The normal path
     * finalizes the INSERT before this callback runs. During an abort, release
     * marks the connection as broken so the next query replaces it.
     */
    ch_binary_release_insert(state->handle);
    state->handle = NULL;
}

static void*
binary_prepare_insert(
    void* conn,
    ResultRelInfo* rri,
    List* target_attrs,
    const ch_query* query,
    char* table_name
) {
    binary_insert_state* state = NULL;
    MemoryContext tempcxt, oldcxt;

    if (table_name == NULL) {
        ereport(ERROR, errcode(ERRCODE_FDW_ERROR), errmsg("expected table name"));
    }

    tempcxt = AllocSetContextCreate(
        CurrentMemoryContext,
        "pg_clickhouse binary insert state",
        ALLOCSET_DEFAULT_SIZES
    );

    /* prepare cleanup */
    oldcxt               = MemoryContextSwitchTo(tempcxt);
    state                = palloc0(sizeof(binary_insert_state));
    state->memcxt        = tempcxt;
    state->callback.func = binary_insert_state_free;
    state->callback.arg  = state;
    state->relid         = RelationGetRelid(rri->ri_RelationDesc);
    MemoryContextRegisterResetCallback(tempcxt, &state->callback);

    /* enter insert mode, take the column list from the server */
    state->handle = ch_binary_begin_insert(conn, query);
    state->ncols  = ch_binary_insert_ncols(state->handle);
    MemoryContextSwitchTo(oldcxt);

    return state;
}

/*
 * Appends one slot of values, with one value for each ClickHouse column. A
 * NULL slot marks the end of input and flushes any buffered rows.
 */
static void
binary_insert_tuple(void* istate, TupleTableSlot* slot) {
    binary_insert_state* state = istate;
    pgch_writer* writer        = ch_binary_insert_writer(state->handle);

    if (slot != NULL) {
        if (state->colmap == NULL) {
            /* Keep the mapping across resets of the per-tuple context. */
            MemoryContext old = MemoryContextSwitchTo(state->memcxt);

            build_insert_colmap(state, slot->tts_tupleDescriptor);
            MemoryContextSwitchTo(old);
        }

        for (size_t i = 0; i < state->ncols; i++) {
            binary_insert_colmap* m = &state->colmap[i];
            bool isnull;
            Datum value = slot_getattr(slot, m->attnum, &isnull);

            pgch_append_datum(writer, i, value, m->atttypid, isnull);
        }

        if (pgch_writer_bytes(writer) < INSERT_FLUSH_BYTES) {
            return;
        }
    }
    ch_binary_flush_block(state->handle);
}

static void
binary_finalize_insert(void* istate) {
    binary_insert_state* state = istate;

    if (state && state->handle) {
        ch_binary_finalize_insert(state->handle);
    }
}

/* ClickHouse aggregate-state wrappers, which import as FDW column options. */
static const char* const aggregate_wrappers[] = {
    "AggregateFunction",
    "SimpleAggregateFunction",
};

/* Return aggregate-state wrapper named by `declaration`, NULL for other types. */
static const char*
aggregate_wrapper(const char* declaration) {
    for (size_t i = 0; i < lengthof(aggregate_wrappers); i++) {
        size_t len = strlen(aggregate_wrappers[i]);

        if (strncmp(declaration, aggregate_wrappers[i], len) == 0 &&
            declaration[len] == '(') {
            return aggregate_wrappers[i];
        }
    }
    return NULL;
}

/*
 * Take one parameter off a ClickHouse parameter list, advancing `params` past it.
 * Return NULL once the list is empty.
 */
static char*
take_parameter(char** params) {
    char* start  = *params;
    char* end    = start;
    int depth    = 0;
    bool literal = false;

    if (*start == '\0') {
        return NULL;
    }
    for (; *end != '\0'; end++) {
        if (*end == '\'') {
            literal = !literal;
        } else if (literal) {
            continue;
        } else if (*end == '(' || *end == '[') {
            depth++;
        } else if (*end == ')' || *end == ']') {
            depth--;
        } else if (*end == ',' && depth == 0) {
            break;
        }
    }
    *params = *end == ',' ? end + 1 : end;
    while (**params == ' ') {
        (*params)++;
    }
    return pnstrdup(start, end - start);
}

/*
 * Split an aggregate-state parameter list, which ClickHouse renders as optional
 * literal parameters, aggregate name, then one type per aggregate argument.
 * Report name through `out_func` and return first argument type, NULL for a
 * state that names none.
 */
static char*
take_aggregate(char* params, char** out_func) {
    char* param;

    *out_func = NULL;
    while ((param = take_parameter(&params)) != NULL) {
        if (*out_func != NULL) {
            /* Reading a multi-argument state merges over its first argument. */
            return param;
        }
        /* Literals ahead of name parameterize aggregate. */
        if (*param != '\'' && *param != '-' && *param != '.' &&
            (*param < '0' || *param > '9')) {
            *out_func = param;
        }
    }
    return NULL;
}

/*
 * Return PostgreSQL type declaration importing a ClickHouse column type.
 * Report outer nullability through `is_nullable` and aggregate-state wrappers
 * through `options`, as alternating FDW option names and values.
 */
static char*
parse_type(
    char* table_name,
    char* colname,
    char* declaration,
    bool* is_nullable,
    List** options
) {
    char* what =
        psprintf("%s.%s", quote_identifier(table_name), quote_identifier(colname));
    const char* wrapper = aggregate_wrapper(declaration);
    pgch_pg_type type;
    chc_type* parsed;
    chc_err err = {};
    char* decl;
    bool as_text;

    if (wrapper != NULL) {
        char* params = pstrdup(declaration + strlen(wrapper) + 1);
        size_t len   = strlen(params);
        char* func;
        char* arg;

        if (len == 0 || params[len - 1] != ')') {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                errmsg(
                    "pg_clickhouse: malformed %s type <%s> for %s",
                    wrapper,
                    declaration,
                    what
                )
            );
        }
        params[len - 1] = '\0';

        arg = take_aggregate(params, &func);
        if (arg == NULL) {
            /* count() state reads as its own result, so it needs no argument. */
            if (func == NULL || strcmp(func, "count") != 0) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
                    errmsg(
                        "pg_clickhouse: expected an argument type in %s type <%s> "
                        "for %s",
                        wrapper,
                        declaration,
                        what
                    )
                );
            }
            return "BIGINT";
        }

        *options    = lappend(*options, makeString(pstrdup(wrapper)));
        *options    = lappend(*options, makeString(func));
        declaration = arg;
    }

    /*
     * Legacy Object('json') predates JSON and serializes as a materialized
     * Tuple, which neither reader nor INSERT writer takes as jsonb.
     */
    if (strncmp(declaration, "Object(", strlen("Object(")) == 0) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
            errmsg("pg_clickhouse: could not map %s type <%s>", what, declaration)
        );
    }

    if (chc_type_parse(declaration, strlen(declaration), &pgch_alloc, &parsed, &err) !=
        CHC_OK) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
            errmsg("pg_clickhouse: could not map %s type <%s>", what, declaration),
            errdetail_internal("%s", err.msg)
        );
    }

    type         = pgch_pg_type_for(parsed, what);
    *is_nullable = type.nullable;

    /* Generic record pseudotypes cannot define columns, fall back to text arrays. */
    as_text = !pgch_pg_type_is_column(type);
    if (as_text) {
        /* Tuple fields read back as array items, taking one more dimension. */
        type.ndims++;
    }
    decl = as_text ? "TEXT[]" : format_type_with_typemod(type.typid, type.typmod);

    for (int dim = 1; dim < type.ndims; dim++) {
        decl = psprintf("%s[]", decl);
    }

    if (as_text) {
        elog(
            NOTICE,
            "pg_clickhouse: ClickHouse <%.*s> type was translated to <%s> type for "
            "column \"%s\", please create composite type and alter the column if "
            "needed",
            (int)strcspn(declaration, "("),
            declaration,
            decl,
            colname
        );
    }

    if (type.truncated) {
        elog(
            NOTICE,
            "pg_clickhouse: ClickHouse <column \"%s\"> precision exceeds "
            "microseconds (6), %s truncates it",
            colname,
            decl
        );
    }

    return decl;
}

List*
chfdw_construct_create_tables(ImportForeignSchemaStmt* stmt, ForeignServer* server) {
    Oid userid         = GetUserId();
    UserMapping* user  = GetUserMapping(userid, server->serverid);
    ch_connection conn = chfdw_get_connection(user);
    ch_cursor* cursor;
    ch_query query = new_query(NULL, 0, NULL, NULL, NULL);
    List* result   = NIL;
    Datum* row_values;

    query.sql = psprintf(
        "SELECT name, engine, engine_full "
        "FROM system.tables "
        "WHERE name NOT LIKE '.inner%%' "
        "AND database = %s",
        ch_quote_literal(stmt->remote_schema)
    );

    cursor = conn.methods->simple_query(conn.conn, &query);

    ChFdwScanRowContext cols_ctx = {
        NULL, list_make2_int(1, 2), NULL, NULL, NULL, NULL
    };

    ChFdwScanRowContext tables_ctx = { NULL, list_make3_int(1, 2, 3),
                                       NULL, cursor,
                                       NULL, NULL };

    /*
     * Drain the outer query into private strings before opening the per-table
     * column queries: both use the same connection, and binary streaming only
     * permits one in-flight response at a time.
     */
    List* tables = NIL;

    while ((row_values = conn.methods->fetch_row(&tables_ctx)) != NULL) {
        List* triple = list_make3(
            pstrdup(TextDatumGetCString(row_values[0])),
            pstrdup(TextDatumGetCString(row_values[1])),
            pstrdup(TextDatumGetCString(row_values[2]))
        );

        CHECK_FOR_INTERRUPTS();
        tables = lappend(tables, triple);
    }
    MemoryContextDelete(cursor->memcxt);

    ListCell* tlc;

    foreach (tlc, tables) {
        List* triple      = (List*)lfirst(tlc);
        char* table_name  = (char*)linitial(triple);
        char* engine      = (char*)lsecond(triple);
        char* engine_full = (char*)lthird(triple);
        StringInfoData buf;
        Datum* dvalues;
        bool first = true;

        if (table_name == NULL) {
            continue;
        }

        if (list_length(stmt->table_list)) {
            ListCell* lc;
            bool found = false;

            foreach (lc, stmt->table_list) {
                RangeVar* rv = (RangeVar*)lfirst(lc);

                if (strcmp(rv->relname, table_name) == 0) {
                    found = true;
                }
            }

            if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT && found) {
                continue;
            } else if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO && !found) {
                continue;
            }
        }

        initStringInfo(&buf);
        appendStringInfo(
            &buf,
            "CREATE FOREIGN TABLE IF NOT EXISTS %s.%s (\n",
            quote_identifier(stmt->local_schema),
            quote_identifier(table_name)
        );
        query.sql = psprintf(
            "SELECT name, type "
            "FROM system.columns "
            "WHERE database = %s "
            "AND table = %s",
            ch_quote_literal(stmt->remote_schema),
            ch_quote_literal(table_name)
        );

        cols_ctx.cursor = conn.methods->simple_query(conn.conn, &query);
        while ((dvalues = conn.methods->fetch_row(&cols_ctx)) != NULL) {
            List* options     = NIL;
            bool is_nullable  = false;
            char* colname     = TextDatumGetCString(dvalues[0]);
            char* remote_type = parse_type(
                table_name,
                colname,
                TextDatumGetCString(dvalues[1]),
                &is_nullable,
                &options
            );

            if (!first) {
                appendStringInfoString(&buf, ",\n");
            }
            first = false;

            /* name */
            appendStringInfo(&buf, "\t%s ", quote_identifier(colname));

            /* type */
            appendStringInfoString(&buf, remote_type);

            if (options != NIL) {
                appendStringInfoString(&buf, " OPTIONS (");
                for (int i = 0; i < list_length(options); i += 2) {
                    if (i) {
                        appendStringInfoString(&buf, ", ");
                    }
                    appendStringInfo(
                        &buf,
                        "%s %s",
                        strVal(list_nth(options, i)),
                        quote_literal_cstr(strVal(list_nth(options, i + 1)))
                    );
                }
                appendStringInfoString(&buf, ")");
                list_free_deep(options);
            }

            if (!is_nullable) {
                appendStringInfoString(&buf, " NOT NULL");
            }
        }

        appendStringInfo(
            &buf,
            "\n) SERVER %s OPTIONS (database %s, table_name %s",
            quote_identifier(server->servername),
            quote_literal_cstr(stmt->remote_schema),
            quote_literal_cstr(table_name)
        );

        if (engine && engine_full && strcmp(engine, "CollapsingMergeTree") == 0) {
            char* sub = strchr(engine_full, ')');

            if (sub) {
                sub[1] = '\0';
                appendStringInfo(&buf, ", engine %s", quote_literal_cstr(engine_full));
            }
        } else if (engine) {
            appendStringInfo(&buf, ", engine %s", quote_literal_cstr(engine));
        }

        appendStringInfoString(&buf, ");\n");
        result = lappend(result, buf.data);
        MemoryContextDelete(cols_ctx.cursor->memcxt);
    }

    return result;
}

/*
 * Escape len bytes from s as an unquoted ClickHouse literal string. Returns a
 * pointer to a palloc'd string.
 *
 * Based on ConvertToSQLString() in src/Client/BuzzHouse/AST/SQLProtoStr.cpp
 * and writeAnyEscapedStringO() in src/IO/WriteHelpers.h in the ClickHouse
 * source code.
 */
static char*
ch_escape_string(const char* from, size_t len) {
    char* result;
    size_t remaining   = len;
    const char* source = from;

    result       = palloc(len * 2 + 1);
    char* target = result;

    while (remaining > 0) {
        char c = *source;

        switch (c) {
        case '\'':
            *target++ = '\\';
            *target++ = c;
            break;
        case '\\':
            *target++ = c;
            *target++ = c;
            break;
        case '\b':
            *target++ = '\\';
            *target++ = 'b';
            break;
        case '\f':
            *target++ = '\\';
            *target++ = 'f';
            break;
        case '\r':
            *target++ = '\\';
            *target++ = 'r';
            break;
        case '\n':
            *target++ = '\\';
            *target++ = 'n';
            break;
        case '\t':
            *target++ = '\\';
            *target++ = 't';
            break;
        case '\0':
            *target++ = '\\';
            *target++ = '0';
            break;
        case '\a':
            *target++ = '\\';
            *target++ = 'a';
            break;
        case '\v':
            *target++ = '\\';
            *target++ = 'v';
            break;
        default:
            *target++ = c;
        }
        source++;
        remaining--;
    }

    *target = '\0';
    return result;
}

/*
 * Convenience function to single-quote a literal SQL string. Differs from
 * PostgreSQL's quote_literal_cstr() by never returning an E-quoted string.
 */
static void
ch_quote_literal_internal(char* dst, const char* src, size_t len) {
    *dst++ = '\'';
    while (*src) {
        if (SQL_STR_DOUBLE(*src, true)) {
            *dst++ = *src;
        }
        *dst++ = *src++;
    }
    *dst++ = '\'';
    *dst++ = '\0';
}

/*
 * Convenience function to escape and return a string as a ClickHouse literal.
 * Returns a palloc'd string.
 */
char*
ch_quote_literal(const char* rawstr) {
    char* result;
    int len;

    len = strlen(rawstr);
    /* We make a worst-case result area; wasting a little space is OK */
    result = palloc(
        (len * 2) /* doubling for every character if each one is
                   * a quote */
        + 2       /* two outer quotes */
        + 1       /* null terminator */
    );

    ch_quote_literal_internal(result, rawstr, len);
    return result;
}

/*
 * Function to quote a ClickHouse identifier. Simply returns `ident` if it's
 * already double-quoted or backtick-quoted. Otherwise quotes it using
 * PostgreSQL's `quote_identifier()`. Raises an error if the identifier length
 * is zero or greater than `NAMEDATALEN` (64) unquoted or
 * `CH_ESCAPED_NAMEDATALEN` quoted.
 */
const char*
ch_quote_ident(const char* ident) {
    /* https://clickhouse.com/docs/sql-reference/syntax#identifiers */
    int len = strlen(ident);

    if (len >= 2 && ((ident[0] == '"' && ident[len - 1] == '"') ||
                     (ident[0] == '`' && ident[len - 1] == '`'))) {
        /*
         * Make sure it has no unescaped quote character. Allowed escapes:
         *
         * ": (""|\\.)
         *
         * `: (``|\\.)
         */
        for (int i = 2; i <= len - 2; i++) {
            /* Skip escaped character. */
            if (ident[i] == '\\') {
                i++;
            }

            /* Disallow unescaped quote character. */
            else if (ident[i] == ident[0] && ident[i + 1] != ident[0]) {
                ereport(
                    ERROR,
                    errcode(ERRCODE_FDW_INVALID_STRING_LENGTH_OR_BUFFER_LENGTH),
                    errmsg_internal("pg_clickhouse: invalid identifier")
                );
            }
        }

        /* Allow already quoted identifier. */
        if (len == 2 || len > CH_ESCAPED_NAMEDATALEN - 1) {
            ereport(
                ERROR,
                errcode(ERRCODE_FDW_INVALID_STRING_LENGTH_OR_BUFFER_LENGTH),
                errmsg_internal("pg_clickhouse: invalid identifier")
            );
        }
        return ident;
    }

    /* Rely on PostgreSQL 's identifier quoting. */
    if (len == 0 || len > NAMEDATALEN - 1) {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_STRING_LENGTH_OR_BUFFER_LENGTH),
            errmsg_internal("pg_clickhouse: invalid identifier")
        );
    }
    return quote_identifier(ident);
}
