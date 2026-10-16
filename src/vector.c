/*-------------------------------------------------------------------------
 *
 * vector.c
 *	  Support for the pgvector extension's vector type.
 *
 * pg_clickhouse does not depend on pgvector. pg-clickhouse-c recognizes the
 * vector type at run time and maps and encodes it as Array(Float32); this file
 * covers pushdown, reading values through the layout below rather than
 * through pgvector's headers.
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/vector.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_extension.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "common/shortest_dec.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

#include "fdw.h"

#include <math.h>

/*
 * On-disk layout of a pgvector vector, matching struct Vector in pgvector's
 * src/vector.h: a varlena header, the dimension count, an unused int16, then
 * dim float4 elements.
 */
typedef struct PgVector {
    int32 vl_len_;
    int16 dim;
    int16 unused;
    float x[FLEXIBLE_ARRAY_MEMBER];
} PgVector;

/*
 * Return true if typid is pgvector's vector type.
 */
bool
chfdw_is_pgvector_type(Oid typid) {
    CustomObjectDef* cdef = chfdw_check_for_custom_type(typid);

    return cdef != NULL && cdef->cf_type == CF_PGVECTOR;
}

/*
 * Return the declaration of a pgvector vector(dims) column, or NULL when
 * pgvector is not installed.
 */
char*
chfdw_pgvector_type_decl(int dims) {
    Oid ext = get_extension_oid("vector", true);

    if (!OidIsValid(ext)) {
        return NULL;
    }

    HeapTuple tuple = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(ext));

    if (!HeapTupleIsValid(tuple)) {
        return NULL;
    }

    Oid schema = ((Form_pg_extension)GETSTRUCT(tuple))->extnamespace;

    ReleaseSysCache(tuple);

    Oid typid = GetSysCacheOid2(
        TYPENAMENSP,
        Anum_pg_type_oid,
        CStringGetDatum("vector"),
        ObjectIdGetDatum(schema)
    );

    return OidIsValid(typid) ? format_type_with_typemod(typid, dims) : NULL;
}

/*
 * Append a pgvector vector as a ClickHouse array literal, e.g. [1,2.5,-3].
 * Elements use the shortest text that reads back as the same float4, so
 * ClickHouse parses them to identical Float32 values.
 */
void
chfdw_append_vector_literal(StringInfo buf, Datum value) {
    PgVector* vec = (PgVector*)PG_DETOAST_DATUM(value);

    if (VARSIZE(vec) != offsetof(PgVector, x) + sizeof(float) * vec->dim) {
        elog(ERROR, "pg_clickhouse: unexpected pgvector vector layout");
    }

    appendStringInfoChar(buf, '[');
    for (int i = 0; i < vec->dim; i++) {
        char num[FLOAT_SHORTEST_DECIMAL_LEN];
        float v = vec->x[i];

        if (i > 0) {
            appendStringInfoChar(buf, ',');
        }

        /* pgvector rejects NaN and infinity, but a corrupt value could not. */
        if (isnan(v) || isinf(v)) {
            elog(ERROR, "pg_clickhouse: vector element %d is not finite", i + 1);
        }

        float_to_shortest_decimal_buf(v, num);
        appendStringInfoString(buf, num);
    }
    appendStringInfoChar(buf, ']');

    if ((Pointer)vec != DatumGetPointer(value)) {
        pfree(vec);
    }
}
