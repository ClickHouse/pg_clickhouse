/*-------------------------------------------------------------------------
 *
 * connection,c
 *		  Connection management functions for pg_clickhouse
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/connection.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_user_mapping.h"
#include "common/int.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "fdw.h"

/*
 * Connection cache (initialized on first use)
 */
static HTAB* ConnectionHash = NULL;

/*
 * Private (non-cached) connections handed to foreign scans that needed a
 * connection while the cached one was already serving a live ancestor scan.
 * Tracked so a transaction-end callback can close any whose scan did not
 * release cleanly (e.g. on error).
 */
static List* PrivateConnections = NIL;

/* Counts transactions, so that a lease outliving its transaction does nothing */
static uint64 lease_generation = 0;

/*
 * A lease on a connection, allocated in the context that took it. If an
 * error frees that context before the holder releases the lease, as when a
 * PL/pgSQL block catches the error, its reset callback releases it.
 * Callbacks run after the context's children are gone, so a cursor created
 * under it has already finished with the connection.
 */
typedef struct ch_lease {
    MemoryContextCallback callback;
    ch_scan_connection sconn;
    uint64 generation;
    bool released;
} ch_lease;

static void
release_lease(void* arg) {
    ch_lease* lease = arg;

    if (lease->generation == lease_generation) {
        chfdw_release_scan_connection(lease->sconn);
    }
}

static void
chfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static void
chfdw_xact_callback(XactEvent event, void* arg);

static ch_connection
clickhouse_connect(ForeignServer* server, UserMapping* user) {
    /* default settings */
    ch_connection_details details = {
        .driver         = "binary",
        .host           = "127.0.0.1",
        .dbname         = "default",
        .encoding_check = CHC_ENC_FAIL,
    };

    chfdw_extract_options(
        server->options,
        &details.driver,
        &details.host,
        &details.port,
        &details.dbname,
        &details.username,
        &details.password,
        &details.compression,
        &details.tls,
        &details.min_tls_version,
        &details.encoding_check
    );
    chfdw_extract_options(
        user->options,
        &details.driver,
        &details.host,
        &details.port,
        &details.dbname,
        &details.username,
        &details.password,
        &details.compression,
        &details.tls,
        &details.min_tls_version,
        &details.encoding_check
    );

    if (strcmp(details.driver, "http") == 0) {
        return chfdw_http_connect(&details);
    } else if (strcmp(details.driver, "binary") == 0) {
        return chfdw_binary_connect(&details);
    } else {
        ereport(
            ERROR,
            errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
            errmsg("invalid ClickHouse connection driver")
        );
    }
}

static ConnCacheEntry*
get_connection_entry(UserMapping* user) {
    bool found;
    ConnCacheEntry* entry;
    ConnCacheKey key;

    /* First time through, initialize connection cache hashtable */
    if (ConnectionHash == NULL) {
        HASHCTL ctl;

        MemSet(&ctl, 0, sizeof(ctl));
        ctl.keysize   = sizeof(ConnCacheKey);
        ctl.entrysize = sizeof(ConnCacheEntry);
        /* allocate ConnectionHash in the cache context */
        ctl.hcxt       = CacheMemoryContext;
        ConnectionHash = hash_create(
            "pg_clickhouse connections", 8, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT
        );

        /*
         * Register some callback functions that manage connection cleanup.
         * This should be done just once in each backend.
         */
        CacheRegisterSyscacheCallback(FOREIGNSERVEROID, chfdw_inval_callback, (Datum)0);
        CacheRegisterSyscacheCallback(USERMAPPINGOID, chfdw_inval_callback, (Datum)0);
        RegisterXactCallback(chfdw_xact_callback, NULL);
    }

    /* Create hash key for the entry. Assume no pad bytes in key struct */
    key.userid = user->umid;

    /*
     * Find or create cached entry for requested connection.
     */
    entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
    if (!found) {
        /*
         * Clear "conn" and "busy"; remaining fields are filled later when
         * "conn" is set.
         */
        entry->gate.conn = NULL;
        entry->busy      = false;
    }

    /*
     * Wait until connection is no longer in use before closing it after a
     * server or user mapping change. Reconnect below.
     */
    if (entry->gate.conn != NULL && entry->invalidated && !entry->busy) {
        elog(LOG, "closing connection to ClickHouse due to invalidation");
        entry->gate.methods->disconnect(entry->gate.conn);
        entry->gate.conn = NULL;
    }

    /*
     * Drop connections that hit an unrecoverable protocol/IO error on the
     * previous statement (server raised mid-INSERT and closed the socket,
     * write hit EPIPE, etc). Without this, a subsequent statement would write
     * to the dead socket and surface a useless "Broken pipe" instead of the
     * real error from the next request. As with invalidation, leave a leased
     * connection, which an abandoned scan still releases at transaction end.
     */
    if (entry->gate.conn != NULL && !entry->busy &&
        entry->gate.methods->is_broken != NULL &&
        entry->gate.methods->is_broken(entry->gate.conn)) {
        elog(LOG, "closing broken pg_clickhouse connection");
        entry->gate.methods->disconnect(entry->gate.conn);
        entry->gate.conn = NULL;
    }

    /*
     * If cache entry doesn't have a connection, we have to establish a new
     * connection. (If clickhouse_connect throws an error, the cache entry
     * will remain in a valid empty state, ie conn == NULL.)
     */
    if (entry->gate.conn == NULL) {
        ForeignServer* server = GetForeignServer(user->serverid);

        /* Reset all transient state fields, to be sure all are clean */
        entry->invalidated = false;
        entry->server_hashvalue =
            GetSysCacheHashValue1(FOREIGNSERVEROID, ObjectIdGetDatum(server->serverid));
        entry->mapping_hashvalue =
            GetSysCacheHashValue1(USERMAPPINGOID, ObjectIdGetDatum(user->umid));

        /* Now try to make the connection */
        entry->gate = clickhouse_connect(server, user);

        elog(
            DEBUG3,
            "new pg_clickhouse connection %p for server \"%s\" (user mapping oid %u, "
            "userid %u)",
            entry->gate.conn,
            server->servername,
            user->umid,
            user->userid
        );
    }

    return entry;
}

ch_scan_connection
chfdw_get_scan_connection(UserMapping* user) {
    ConnCacheEntry* entry = get_connection_entry(user);
    ch_lease* lease       = palloc0(sizeof(ch_lease));

    lease->callback.func = release_lease;
    lease->callback.arg  = lease;
    lease->generation    = lease_generation;
    if (!entry->busy) {
        entry->busy  = true;
        lease->sconn = (ch_scan_connection){ entry->gate, entry, lease };
        MemoryContextRegisterResetCallback(CurrentMemoryContext, &lease->callback);
        return lease->sconn;
    }

    /*
     * ClickHouse allows only one query at a time per connection. Open another
     * connection when cached one is busy, as in INSERT ... SELECT or a
     * correlated subquery. Track it for transaction cleanup in case of error.
     */
    {
        ForeignServer* server = GetForeignServer(user->serverid);
        ch_connection* held;
        MemoryContext old;

        old                = MemoryContextSwitchTo(CacheMemoryContext);
        held               = palloc(sizeof(ch_connection));
        *held              = clickhouse_connect(server, user);
        PrivateConnections = lappend(PrivateConnections, held);
        MemoryContextSwitchTo(old);

        lease->sconn = (ch_scan_connection){ *held, NULL, lease };
        MemoryContextRegisterResetCallback(CurrentMemoryContext, &lease->callback);
        return lease->sconn;
    }
}

void
chfdw_release_scan_connection(ch_scan_connection sconn) {
    ListCell* lc;

    if (sconn.lease != NULL) {
        if (sconn.lease->released) {
            return;
        }
        sconn.lease->released = true;
    }

    /* ConnectionHash never removes entries, so this pointer stays valid. */
    if (sconn.entry != NULL) {
        sconn.entry->busy = false;
        return;
    }

    /* Transaction cleanup may have already closed and removed this connection. */
    foreach (lc, PrivateConnections) {
        ch_connection* held = (ch_connection*)lfirst(lc);

        if (held->conn == sconn.gate.conn) {
            PrivateConnections = foreach_delete_current(PrivateConnections, lc);
            if (held->conn != NULL) {
                held->methods->disconnect(held->conn);
            }
            pfree(held);
            return;
        }
    }
}

ch_server_version
chfdw_get_server_version(UserMapping* user) {
    ch_connection conn = get_connection_entry(user)->gate;

    if (conn.methods->server_version == NULL) {
        return (ch_server_version){ 0, 0, 0 };
    }
    return conn.methods->server_version(conn.conn);
}

/*
 * Connection invalidation callback function
 *
 * After a change to a pg_foreign_server or pg_user_mapping catalog entry,
 * mark connections depending on that entry as needing to be remade.
 * We can't immediately destroy them, since they might be in the midst of
 * a transaction, but we'll remake them at the next opportunity.
 *
 * Although most cache invalidation callbacks blow away all the related stuff
 * regardless of the given hashvalue, connections are expensive enough that
 * it's worth trying to avoid that.
 *
 * NB: We could avoid unnecessary disconnection more strictly by examining
 * individual option values, but it seems too much effort for the gain.
 */
static void
chfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue) {
    HASH_SEQ_STATUS scan;
    ConnCacheEntry* entry;

    Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

    /* ConnectionHash must exist already, if we're registered */
    hash_seq_init(&scan, ConnectionHash);
    while ((entry = (ConnCacheEntry*)hash_seq_search(&scan))) {
        /* Ignore empty entries */
        if (entry->gate.conn == NULL) {
            continue;
        }

        /* hashvalue == 0 means a cache reset, must clear all state */
        if (hashvalue == 0 ||
            (cacheid == FOREIGNSERVEROID && entry->server_hashvalue == hashvalue) ||
            (cacheid == USERMAPPINGOID && entry->mapping_hashvalue == hashvalue)) {
            entry->invalidated = true;
        }
    }
}

/*
 * Transaction-end cleanup. By the end of a transaction no foreign scan is live,
 * so close any private scan connections that were not released (e.g. a scan
 * whose End was skipped on error) and clear any leftover busy markers on cached
 * connections so they can be reused. Close invalidated connections, too: one
 * whose user mapping or server was dropped would otherwise stay open for the
 * life of the backend, since nothing looks it up again.
 */
static void
chfdw_xact_callback(XactEvent event, void* arg) {
    ListCell* lc;

    if (event != XACT_EVENT_COMMIT && event != XACT_EVENT_ABORT &&
        event != XACT_EVENT_PARALLEL_COMMIT && event != XACT_EVENT_PARALLEL_ABORT) {
        return;
    }

    foreach (lc, PrivateConnections) {
        ch_connection* held = (ch_connection*)lfirst(lc);

        if (held->conn != NULL) {
            held->methods->disconnect(held->conn);
        }
    }
    list_free_deep(PrivateConnections);
    PrivateConnections = NIL;
    lease_generation++;

    if (ConnectionHash != NULL) {
        HASH_SEQ_STATUS scan;
        ConnCacheEntry* entry;

        hash_seq_init(&scan, ConnectionHash);
        while ((entry = (ConnCacheEntry*)hash_seq_search(&scan)) != NULL) {
            entry->busy = false;
            if (entry->gate.conn != NULL && entry->invalidated) {
                entry->gate.methods->disconnect(entry->gate.conn);
                entry->gate.conn = NULL;
            }
        }
    }
}
