/*-------------------------------------------------------------------------
 *
 * connection,c
 *		  Connection management functions for pg_clickhouse
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/connection.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_user_mapping.h"
#include "common/int.h"
#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "fdw.h"

/*
 * Connection cache (initialized on first use)
 */
static HTAB * ConnectionHash = NULL;
static void chfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue);
static int32 strtoint32(const char *s);


static ch_connection
clickhouse_connect(ForeignServer * server, UserMapping * user)
{
	char	   *driver = "http";

	/* default settings */
	ch_connection_details details = {"127.0.0.1", 0, NULL, NULL, "default"};

	chfdw_extract_options(server->options, &driver, &details.host,
						  &details.port, &details.dbname, &details.username, &details.password);
	chfdw_extract_options(user->options, &driver, &details.host,
						  &details.port, &details.dbname, &details.username, &details.password);

	if (strcmp(driver, "http") == 0)
	{
		return chfdw_http_connect(&details);
	}
	else if (strcmp(driver, "binary") == 0)
	{
		return chfdw_binary_connect(&details);
	}
	else
		elog(ERROR, "invalid ClickHouse connection driver");
}

ch_connection
chfdw_get_connection(UserMapping * user)
{
	bool		found;
	ConnCacheEntry *entry;
	ConnCacheKey key;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("pg_clickhouse connections", 8,
									 &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

		/*
		 * Register some callback functions that manage connection cleanup.
		 * This should be done just once in each backend.
		 */
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
									  chfdw_inval_callback, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID,
									  chfdw_inval_callback, (Datum) 0);
	}

	/* Create hash key for the entry. Assume no pad bytes in key struct */
	key.userid = user->umid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * We need only clear "conn" here; remaining fields will be filled
		 * later when "conn" is set.
		 */
		entry->gate.conn = NULL;
	}

	/*
	 * If the connection needs to be remade due to invalidation, disconnect as
	 * soon as we're out of all transactions.
	 */
	if (entry->gate.conn != NULL && entry->invalidated)
	{
		elog(LOG, "closing connection to ClickHouse due to invalidation");
		entry->gate.methods->disconnect(entry->gate.conn);
		entry->gate.conn = NULL;
	}

	/*
	 * We don't check the health of cached connection here, because it would
	 * require some overhead. Broken connection will be detected when the
	 * connection is actually used.
	 */

	/*
	 * If cache entry doesn't have a connection, we have to establish a new
	 * connection. (If clickhouse_connect throws an error, the cache entry
	 * will remain in a valid empty state, ie conn == NULL.)
	 */
	if (entry->gate.conn == NULL)
	{
		ForeignServer *server = GetForeignServer(user->serverid);

		/* Reset all transient state fields, to be sure all are clean */
		entry->invalidated = false;
		entry->server_hashvalue =
			GetSysCacheHashValue1(FOREIGNSERVEROID,
								  ObjectIdGetDatum(server->serverid));
		entry->mapping_hashvalue =
			GetSysCacheHashValue1(USERMAPPINGOID,
								  ObjectIdGetDatum(user->umid));

		/* Now try to make the connection */
		entry->gate = clickhouse_connect(server, user);

		elog(DEBUG3,
			 "new pg_clickhouse connection %p for server \"%s\" (user mapping oid %u, userid %u)",
			 entry->gate.conn, server->servername, user->umid, user->userid);
	}

	return entry->gate;
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
chfdw_inval_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS scan;
	ConnCacheEntry *entry;

	Assert(cacheid == FOREIGNSERVEROID || cacheid == USERMAPPINGOID);

	/* ConnectionHash must exist already, if we're registered */
	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		/* Ignore empty entries */
		if (entry->gate.conn == NULL)
			continue;

		/* hashvalue == 0 means a cache reset, must clear all state */
		if (hashvalue == 0 ||
			(cacheid == FOREIGNSERVEROID &&
			 entry->server_hashvalue == hashvalue) ||
			(cacheid == USERMAPPINGOID &&
			 entry->mapping_hashvalue == hashvalue))
		{
			entry->invalidated = true;
		}
	}
}


ch_connection_details *
connstring_parse(const char *connstring)
{
	ListCell   *lc;
	List	   *options = chfdw_parse_options(connstring, false);
	ch_connection_details *details = palloc0(sizeof(ch_connection_details));

	if (options == NIL)
		return details;

	foreach(lc, options)
	{
		DefElem    *elem = (DefElem *) lfirst(lc);
		char	   *pname = elem->defname;
		char	   *pval = strVal(elem->arg);

		if (strcmp(pname, "host") == 0)
		{
			details->host = pval;
		}
		else if (strcmp(pname, "port") == 0)
		{
			details->port = strtoint32(pval);
		}
		else if (strcmp(pname, "username") == 0)
		{
			details->username = pval;
		}
		else if (strcmp(pname, "password") == 0)
		{
			details->password = pval;
		}
		else if (strcmp(pname, "dbname") == 0)
		{
			details->dbname = pval;
		}
		else if (strcmp(pname, "") != 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("pg_clickhouse: invalid connection option \"%s\"", pname)));
		}
	}

	return details;
}

/*
 * Convert input string to a signed 32 bit integer.
 *
 * Allows any number of leading or trailing whitespace characters. Will throw
 * ereport() upon bad input format or overflow.
 *
 * NB: Accumulate input as a negative number, to deal with two's complement
 * representation of the most negative number, which can't be represented as a
 * positive number.
 *
 * ---
 * Copied from postgresql source.
 */
static int32
strtoint32(const char *s)
{
	const char *ptr = s;
	int32		tmp = 0;
	bool		neg = false;

	/* skip leading spaces */
	while (likely(*ptr) && isspace((unsigned char) *ptr))
		ptr++;

	/* handle sign */
	if (*ptr == '-')
	{
		ptr++;
		neg = true;
	}
	else if (*ptr == '+')
		ptr++;

	/* require at least one digit */
	if (unlikely(!isdigit((unsigned char) *ptr)))
		goto invalid_syntax;

	/* process digits */
	while (*ptr && isdigit((unsigned char) *ptr))
	{
		int8		digit = (*ptr++ - '0');

		if (unlikely(pg_mul_s32_overflow(tmp, 10, &tmp)) ||
			unlikely(pg_sub_s32_overflow(tmp, digit, &tmp)))
			goto out_of_range;
	}

	/* allow trailing whitespace, but not other trailing chars */
	while (*ptr != '\0' && isspace((unsigned char) *ptr))
		ptr++;

	if (unlikely(*ptr != '\0'))
		goto invalid_syntax;

	if (!neg)
	{
		/* could fail if input is most negative number */
		if (unlikely(tmp == PG_INT32_MIN))
			goto out_of_range;
		tmp = -tmp;
	}

	return tmp;

out_of_range:
	ereport(ERROR,
			(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			 errmsg("value \"%s\" is out of range for type %s",
					s, "integer")));

invalid_syntax:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"integer", s)));

	return 0;					/* keep compiler quiet */
}
