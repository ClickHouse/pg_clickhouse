/*
 * server_version.h
 *
 * This type represents the ClickHouse server version reported by an active
 * connection. This header has no project dependencies, which allows low-level
 * drivers and the FDW layer to share the type without sharing their state.
 */

#ifndef CLICKHOUSE_SERVER_VERSION_H
#define CLICKHOUSE_SERVER_VERSION_H

#include <stdbool.h>

typedef struct ch_server_version {
    int major;
    int minor;
    int patch;
} ch_server_version;

/* True if version v is at least major.minor. */
static inline bool
chfdw_version_ge(ch_server_version v, int major, int minor) {
    return v.major > major || (v.major == major && v.minor >= minor);
}

#endif /* CLICKHOUSE_SERVER_VERSION_H */
