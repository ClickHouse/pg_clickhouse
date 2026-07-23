/*-------------------------------------------------------------------------
 *
 * setop.h
 *		  Planner helpers for set-operation pushdown
 *
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/include/setop.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CLICKHOUSE_SETOP_H
#define CLICKHOUSE_SETOP_H

#include "foreign/fdwapi.h"
#include "nodes/pathnodes.h"
#include "nodes/plannodes.h"

typedef bool (*ChFdwSetopPathValidator)(const ForeignPath* path, void* arg);

typedef struct ChFdwSetopPathInfo {
    List* foreign_paths;
    FdwRoutine* fdwroutine;
    Oid serverid;
    Oid userid;
    bool useridiscurrent;
} ChFdwSetopPathInfo;

extern AppendPath*
chfdw_setop_find_append_path(Path* path, bool* is_distinct);

extern bool
chfdw_setop_extract_foreign_paths(
    AppendPath* append_path,
    ChFdwSetopPathValidator validator,
    void* validator_arg,
    ChFdwSetopPathInfo* path_info
);

extern bool
chfdw_setop_extract_foreign_scans(
    Plan* plan,
    Oid expected_serverid,
    List** foreign_scans
);

#endif /* CLICKHOUSE_SETOP_H */
