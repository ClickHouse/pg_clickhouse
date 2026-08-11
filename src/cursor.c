/*
 * cursor.c
 *
 * Shared lifecycle for a cursor over a ClickHouse Native block stream: open
 * over a driver's response, fetch rows through the decoder, release both on
 * teardown. API lives in src/include/cursor.h; the drivers in src/pglink.c
 * only supply the response and how to read it.
 */

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/memutils.h"
#include "utils/portal.h"

#include "cursor.h"

/* Release the reader before the response its blocks came from. */
static void
cursor_free(void* c) {
    ch_cursor* cursor = c;

    pgch_reader_free(&cursor->reader);
    cursor->free_response(cursor->response);
    cursor->response = NULL;
}

/* Report decoder error; a driver hook may report cancellation instead. */
static void
raise_reader_error(ch_cursor* cursor) {
    if (cursor->raise_response_error) {
        cursor->raise_response_error(cursor);
    }
    /* Prefer consistent interrupt error message when fetch interrupted */
    CHECK_FOR_INTERRUPTS();
    ereport(
        ERROR,
        errcode(ERRCODE_SQL_ROUTINE_EXCEPTION),
        errmsg("pg_clickhouse: %s", cursor->reader.error),
        errdetail_internal("Remote Query: %.64000s", cursor->query)
    );
}

/* Match the returned columns against the destination the query asked for. */
static void
configure_columns(ch_cursor* cursor, const ch_query* query) {
    pgch_reader* reader = &cursor->reader;

    if (query->tupdesc && query->attr_nums && cursor->columns_count > 0 &&
        (size_t)list_length(query->attr_nums) != cursor->columns_count) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg_internal(
                "pg_clickhouse: returned %lu columns, expected %lu",
                (unsigned long)cursor->columns_count,
                (unsigned long)list_length(query->attr_nums)
            ),
            errdetail_internal("Remote Query: %.64000s", query->sql)
        );
    }

    /* Preserve JSON text when PostgreSQL destination uses json, not jsonb. */
    if (query->tupdesc && reader->coltypes) {
        ListCell* lc;
        size_t j = 0;

        foreach (lc, query->attr_nums) {
            int i = lfirst_int(lc);

            if (reader->coltypes[j] == JSONBOID &&
                TupleDescAttr(query->tupdesc, i - 1)->atttypid == JSONOID) {
                reader->coltypes[j] = JSONOID;
            }
            j++;
        }
    }
}

ch_cursor*
chfdw_cursor_open(void* conn, const ch_query* query, const ch_cursor_source* src) {
    /* volatile: assigned inside PG_TRY, read after longjmp in PG_CATCH */
    ch_cursor* volatile cursor  = NULL;
    volatile MemoryContext cxt  = NULL;
    volatile bool owns_response = false;
    MemoryContext oldcxt        = CurrentMemoryContext;

    PG_TRY();
    {
        cxt = AllocSetContextCreate(
            PortalContext, "pg_clickhouse cursor", ALLOCSET_DEFAULT_SIZES
        );
        MemoryContextSwitchTo(cxt);

        cursor                       = palloc0(sizeof(ch_cursor));
        cursor->memcxt               = cxt;
        cursor->conn                 = conn;
        cursor->query                = pstrdup(query->sql);
        cursor->free_response        = src->free_response;
        cursor->raise_response_error = src->raise_response_error;

        /* Register before taking the response, so unwinding releases it. */
        cursor->callback.func = cursor_free;
        cursor->callback.arg  = (void*)cursor;
        MemoryContextRegisterResetCallback(cxt, &cursor->callback);
        cursor->response = src->response;
        owns_response    = true;

        /* Blocks decode into cxt, outliving the per-row context. */
        src->init_reader(&cursor->reader, cursor->response);
        cursor->columns_count = pgch_reader_columns(&cursor->reader);

        if (cursor->reader.error) {
            raise_reader_error(cursor);
        }
        configure_columns(cursor, query);

        MemoryContextSwitchTo(oldcxt);
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(oldcxt);
        if (!owns_response) {
            src->free_response(src->response);
        }
        if (cxt != NULL) {
            MemoryContextDelete(cxt);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    return cursor;
}

/* Conversion state and target attribute per returned column. */
static void
build_conversion(ch_cursor* cursor, const ChFdwScanRowContext* ctx) {
    pgch_reader* reader = &cursor->reader;
    MemoryContext old   = MemoryContextSwitchTo(cursor->memcxt);
    size_t ncols        = pgch_reader_columns(reader);
    ListCell* lc;
    size_t j = 0;

    cursor->conversion_states = palloc0(ncols * sizeof(void*));
    cursor->fill_dest         = palloc0(ncols * sizeof(int));
    foreach (lc, ctx->retrieved_attrs) {
        int attnum            = lfirst_int(lc);
        Form_pg_attribute att = TupleDescAttr(ctx->tupdesc, attnum - 1);

        cursor->fill_dest[j] = attnum - 1;
        cursor->conversion_states[j] =
            pgch_reader_convert_init(reader, j, att->atttypid, att->atttypmod);
        j++;
    }

    MemoryContextSwitchTo(old);
}

/* Apply PostgreSQL conversions to fetched Native row. */
static Datum*
apply_row(ChFdwScanRowContext* ctx) {
    ch_cursor* cursor   = ctx->cursor;
    List* attrs         = ctx->retrieved_attrs;
    TupleDesc tupdesc   = ctx->tupdesc;
    Datum* values       = ctx->values;
    bool* nulls         = ctx->nulls;
    pgch_reader* reader = &cursor->reader;
    size_t attcount     = list_length(attrs);

    if (attcount == 0) {
        if (pgch_reader_columns(reader) == 1 && reader->nulls[0]) {
            nulls[0] = true;
            return reader->values;
        }
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_ERROR),
            errmsg(
                "pg_clickhouse: unexpected state: attributes "
                "count == 0 and haven't got NULL in the response"
            )
        );
    } else if (attcount != pgch_reader_columns(reader)) {
        ereport(
            ERROR,
            errcode(ERRCODE_DATATYPE_MISMATCH),
            errmsg_internal(
                "pg_clickhouse: returned %lu columns, expected %lu",
                pgch_reader_columns(reader),
                attcount
            )
        );
    }

    if (tupdesc) {
        Assert(values && nulls);

        if (cursor->conversion_states == NULL) {
            build_conversion(cursor, ctx);
        }
        pgch_reader_fill_map(
            reader, cursor->conversion_states, cursor->fill_dest, values, nulls
        );
    }

    return reader->values;
}

static void
fetch_row_errcb(void* arg) {
    const char* sql = (const char*)arg;

    errdetail_internal("Remote Query: %.64000s", sql);
}

/*
 * Fetch a row from the cursor and return its values.
 *
 * If ctx->tupdesc is set, ctx->attinmeta must also be set, and ctx->values
 * and ctx->nulls must already be palloc'd with space for ctx->tupdesc->natts
 * values.
 *
 * Use ctx->tupdesc and ctx->attinmeta to convert the values to the
 * appropriate Datums, and store them and the indication of their NULLness in
 * ctx->values and ctx->nulls, respectively, then return ctx->values.
 *
 * If ctx->tupdesc is not set, treat all values as text and return them as
 * text `Datum`s. This is the use case for `chfdw_construct_create_tables()`,
 * which only cares about text.
 */
Datum*
chfdw_cursor_fetch_row(ChFdwScanRowContext* ctx) {
    ch_cursor* cursor = ctx->cursor;
    ErrorContextCallback errcallback;
    bool have_data;
    Datum* result;

    errcallback.callback = fetch_row_errcb;
    errcallback.arg      = (void*)cursor->query;
    errcallback.previous = error_context_stack;
    error_context_stack  = &errcallback;

    have_data = pgch_reader_next(&cursor->reader);

    if (cursor->reader.error) {
        error_context_stack = errcallback.previous;
        raise_reader_error(cursor);
    }

    result = have_data ? apply_row(ctx) : NULL;

    error_context_stack = errcallback.previous;
    return result;
}
