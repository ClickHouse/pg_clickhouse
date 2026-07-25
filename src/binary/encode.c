/*
 * encode.c
 *
 * PG-side INSERT path. Matches slot attributes onto the ClickHouse columns
 * the server announced, then hands each Datum to pg-clickhouse-c's writer,
 * which dispatches on (PG type, CH kind) and casts when no direct pair fits.
 */

#include "postgres.h"

#include <string.h>

#include "access/tupdesc.h"
#include "executor/tuptable.h"
#include "utils/builtins.h"

#include "binary_internal.h"
#include "fdw.h"

void
ch_binary_prepare_insert(
    void* conn,
    const ch_query* query,
    ch_binary_insert_state* state
) {
    ch_binary_insert_handle* h =
        ch_binary_begin_insert((ch_binary_connection_t*)conn, query);

    state->insert_block = h;
    state->len          = ch_binary_insert_ncols(h);
}

/*
 * Resolve which slot attribute feeds each ClickHouse column. Matches by name,
 * honoring the column_name FDW option; an input descriptor whose first
 * attribute is unnamed carries no names at all, so map by position.
 */
static void
build_colmap(ch_binary_insert_state* state, TupleDesc indesc) {
    ch_binary_insert_handle* h = state->insert_block;
    bool positional =
        indesc->natts > 0 && NameStr(TupleDescAttr(indesc, 0)->attname)[0] == '\0';

    state->colmap = palloc0(state->len * sizeof(ch_binary_insert_colmap));

    for (size_t i = 0; i < state->len; i++) {
        ch_binary_insert_colmap* m = &state->colmap[i];
        const char* chname         = ch_binary_insert_column_name(h, i);

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

                /* Honor column_name FDW option, falls through to attname */
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

void
ch_binary_insert_tuple(ch_binary_insert_state* state, TupleTableSlot* slot) {
    pgch_writer* w = ch_binary_insert_writer(state->insert_block);

    if (state->colmap == NULL) {
        /* Outlive the per-tuple context the executor calls us in. */
        MemoryContext old = MemoryContextSwitchTo(state->memcxt);

        build_colmap(state, slot->tts_tupleDescriptor);
        MemoryContextSwitchTo(old);
    }

    for (size_t i = 0; i < state->len; i++) {
        ch_binary_insert_colmap* m = &state->colmap[i];
        bool isnull;
        Datum val = slot_getattr(slot, m->attnum, &isnull);

        pgch_append_datum(w, i, val, m->atttypid, isnull);
    }
}

void
ch_binary_insert_columns(ch_binary_insert_state* state) {
    ch_binary_flush_block(state->insert_block);
}

void
ch_binary_insert_state_free(void* c) {
    ch_binary_insert_state* state = (ch_binary_insert_state*)c;

    if (state->insert_block == NULL) {
        return;
    }

    /*
     * Reset-callback context: cannot ereport. Finalize runs from the FDW
     * happy path before we get here; if it did not (mid-query abort), the
     * release call flags the connection broken so the next query rebuilds.
     */
    ch_binary_release_insert(state->insert_block);
    state->insert_block = NULL;
}
