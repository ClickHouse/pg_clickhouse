/*-------------------------------------------------------------------------
 *
 * shippable.c
 *	  Determine which database objects are shippable to a remote server.
 *
 * Portions Copyright (c) 2012-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 2019-2022, Adjust GmbH
 * Copyright (c) 2025-2026, ClickHouse, Inc.
 *
 * IDENTIFICATION
 *		  github.com/clickhouse/pg_clickhouse/src/shippable.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fdw.h"

#include "access/transam.h"
#include "catalog/dependency.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "catalog/pg_operator.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/syscache.h"


/* Hash table for caching the results of shippability lookups */
static HTAB * ShippableCacheHash = NULL;

/*
 * Hash key for shippability lookups. We include the FDW server OID because
 * decisions may differ per-server. Otherwise, objects are identified by
 * their (local!) OID and catalog OID.
 */
typedef struct
{
	/* XXX we assume this struct contains no padding bytes */
	Oid			objid;			/* function/operator/type OID */
	Oid			classid;		/* OID of its catalog (pg_proc, etc) */
	Oid			serverid;		/* FDW server we are concerned with */
}			ShippableCacheKey;

typedef struct
{
	ShippableCacheKey key;		/* hash key - must be first */
	bool		shippable;
}			ShippableCacheEntry;


/*
 * Flush cache entries when pg_foreign_server is updated.
 *
 * We do this because of the possibility of ALTER SERVER being used to change
 * a server's extensions option. We do not currently bother to check whether
 * objects' extension membership changes once a shippability decision has been
 * made for them, however.
 */
static void
InvalidateShippableCacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	ShippableCacheEntry *entry;

	/*
	 * In principle we could flush only cache entries relating to the
	 * pg_foreign_server entry being outdated; but that would be more
	 * complicated, and it's probably not worth the trouble. So for now, just
	 * flush all entries.
	 */
	hash_seq_init(&status, ShippableCacheHash);
	while ((entry = (ShippableCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(ShippableCacheHash,
						(void *) &entry->key,
						HASH_REMOVE,
						NULL) == NULL)
		{
			elog(ERROR, "hash table corrupted");
		}
	}
}

/*
 * Initialize the backend-lifespan cache of shippability decisions.
 */
static void
InitializeShippableCache(void)
{
	HASHCTL		ctl;

	/* Create the hash table. */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(ShippableCacheKey);
	ctl.entrysize = sizeof(ShippableCacheEntry);
	ShippableCacheHash =
		hash_create("Shippability cache", 256, &ctl, HASH_ELEM | HASH_BLOBS);

	/* Set up invalidation callback on pg_foreign_server. */
	CacheRegisterSyscacheCallback(FOREIGNSERVEROID,
								  InvalidateShippableCacheCallback,
								  (Datum) 0);
}

/*
 * Returns true if given object (operator/function/type) is shippable
 * according to the server options.
 *
 * Right now "shippability" is exclusively a function of whether the object
 * belongs to an extension declared by the user. In the future we could
 * additionally have a whitelist of functions/operators declared one at a time.
 */
static bool
lookup_shippable(Oid objectId, Oid classId, CHFdwRelationInfo * fpinfo)
{
	Oid			extensionOid;

	/*
	 * Is object a member of some extension?  (Note: this is a fairly
	 * expensive lookup, which is why we try to cache the results.)
	 */
	extensionOid = getExtensionOfObject(classId, objectId);

	/* If so, is that extension in fpinfo->shippable_extensions? */
	if (OidIsValid(extensionOid) &&
		list_member_oid(fpinfo->shippable_extensions, extensionOid))
	{
		return true;
	}

	return false;
}

/*
 * Return true if given object is one of PostgreSQL's built-in objects.
 *
 * We use FirstUnpinnedObjectId as the cutoff, so that we only consider
 * objects with hand-assigned OIDs to be "built in", not for instance any
 * function or type defined in the information_schema.
 *
 * Our constraints for dealing with types are tighter than they are for
 * functions or operators: we want to accept only types that are in pg_catalog,
 * else deparse_type_name might incorrectly fail to schema-qualify their names.
 * Thus we must exclude information_schema types.
 *
 * XXX there is a problem with this, which is that the set of built-in
 * objects expands over time. Something that is built-in to us might not
 * be known to the remote server, if it's of an older version. But keeping
 * track of that would be a huge exercise.
 */
bool
chfdw_is_builtin(Oid objectId)
{
	return (objectId < FirstUnpinnedObjectId);
}

static bool
regex_flags_ok(char *flags, bool global_ok)
{
	while (*flags)
	{
		switch (*flags)
		{
			case 't':			/* Removed by appendRegexFlags() */
			case 'p':			/* Removed by appendRegexFlags() */
			case 's':
			case 'i':
			case 'm':
				/* Pass through as-is. */
				break;
			case 'g':
				if (global_ok)
					/* Pass through as-is; deparseFuncExpr will remove it. */
					break;
				return false;
			default:
				/* Cannot pass down any other flags */
				return false;
		}
		flags++;
	}

	/* All good. */
	return true;
}

/*
 * chfdw_is_shippable
 *	   Is this object (function/operator/type) shippable to foreign server?
 */
bool
chfdw_is_shippable(Node * node, Oid objectId, Oid classId, CHFdwRelationInfo * fpinfo,
				   CustomObjectDef * *outcdef)
{
	ShippableCacheKey key;
	ShippableCacheEntry *entry;

	/*
	 * For operators, check for custom overrides before the builtin shortcut,
	 * since some builtin operators (e.g. ~, !~, ~*, !~*) need special
	 * handling.
	 */
	if (classId == OperatorRelationId)
	{
		CustomObjectDef *cdef = chfdw_check_for_custom_operator(objectId, NULL);

		if (cdef)
		{
			switch (cdef->cf_type)
			{
				case CF_REGEX_MATCH:
				case CF_REGEX_NO_MATCH:
				case CF_REGEX_ICASE_MATCH:
				case CF_REGEX_ICASE_NO_MATCH:
					/* Don't pushdown regular expressions if the GUC is false. */
					return chfdw_pushdown_regex_ok();
				default:
					return true;
			}
		}
	}

	/*
	 * For procedures, check for custom overrides before the builtin shortcut
	 * so the caller gets the CustomObjectDef it needs for deparse.
	 */
	if (classId == ProcedureRelationId && chfdw_is_builtin(objectId))
	{
		CustomObjectDef *cdef = chfdw_check_for_custom_function(objectId);

		if (cdef)
		{
			if (outcdef != NULL)
				*outcdef = cdef;

			/*
			 * Evaluate certain special functions whose arguments prevent
			 * pushdown.
			 */
			switch (cdef->cf_type)
			{
				case CF_ARRAY_SORT_DESC:
					{
						/*
						 * If the boolean argument passed to array_sort(arr,
						 * bool) is dynamic, we can't push it down.
						 */
						Expr	   *desc_arg = (Expr *) list_nth(((FuncExpr *) node)->args, 1);

						if (!IsA(desc_arg, Const))
							/* No support for a dynamic value here. */
							return false;
					}
					break;
				case CF_TO_CHAR:
					{
						/* format must be a constant with exact CH translation */
						Expr	   *fmt = (Expr *) list_nth(((FuncExpr *) node)->args, 1);
						Const	   *fmt_const;
						char	   *pgfmt;
						bool		ok;

						if (!IsA(fmt, Const))
							return false;
						fmt_const = (Const *) fmt;
						if (fmt_const->constisnull)
							return false;

						pgfmt = TextDatumGetCString(fmt_const->constvalue);
						ok = chfdw_translate_to_char_format(pgfmt, NULL);
						pfree(pgfmt);
						if (!ok)
							return false;
					}
					break;
				case CF_MATCH:
				case CF_SPLIT_BY_REGEX:
				case CF_REPLACE_REGEX:
					{
						/*
						 * Don't pushdown regular expressions if the GUC is
						 * false.
						 */
						if (!chfdw_pushdown_regex_ok())
							return false;

						/* Unshippable if using unsupported or dynamic flags */
						FuncExpr   *fn = (FuncExpr *) node;
						int			flags_idx = cdef->cf_type == CF_REPLACE_REGEX ? 3 : 2;

						if ((list_length(fn->args) >= flags_idx + 1))
						{
							Expr	   *arg = (Expr *) list_nth(((FuncExpr *) node)->args, flags_idx);

							if (!IsA(arg, Const))
								/* No support for a dynamic value here. */
								return false;

							if (!regex_flags_ok(
												TextDatumGetCString(((Const *) arg)->constvalue),
												cdef->cf_type == CF_REPLACE_REGEX
												))
								/* Using flags unsupported by ClickHouse. */
								return false;
						}

						/*
						 * XXX Additional checks: Examine the regex and:
						 *
						 * - don't ship if it starts with "***"
						 *
						 * - use a substring function if starts with "***="
						 *
						 * - don't ship if it contains (?xyz) with unsupported
						 * flags
						 *
						 * Also applies to CF_REGEX_MATCH, CF_REGEX_NO_MATCH,
						 * CF_REGEX_ICASE_MATCH, and CF_REGEX_ICASE_NO_MATCH.
						 */
					}
					break;
				default:
					break;
			}
			return true;
		}
	}

	/*
	 * Builtin operators and types ship by default. Builtin functions only
	 * ship when explicitly registered via chfdw_check_for_custom_function, so
	 * unrecognised builtins fail planner shippability checks rather than
	 * deparse to a name that may behave differently on ClickHouse. Cast
	 * coercions are an exception: deparse handles them as cast() or by
	 * dropping the implicit wrapper, regardless of the underlying funcid.
	 */
	if (chfdw_is_builtin(objectId))
	{
		if (classId != ProcedureRelationId)
			return true;
		if (node && IsA(node, FuncExpr))
		{
			FuncExpr   *fe = (FuncExpr *) node;

			if (fe->funcformat == COERCE_IMPLICIT_CAST ||
				fe->funcformat == COERCE_EXPLICIT_CAST)
				return true;
		}
		return false;
	}

	if (classId == ProcedureRelationId)
	{
		CustomObjectDef *cdef = chfdw_check_for_custom_function(objectId);

		if (outcdef != NULL)
			*outcdef = cdef;

		return cdef != NULL;
	}
	else if (classId == TypeRelationId && chfdw_check_for_custom_type(objectId) != NULL)
		return true;

	/* Otherwise, give up if user hasn't specified any shippable extensions. */
	if (fpinfo->shippable_extensions == NIL)
		return false;

	/* Initialize cache if first time through. */
	if (!ShippableCacheHash)
	{
		InitializeShippableCache();
	}

	/* Set up cache hash key */
	key.objid = objectId;
	key.classid = classId;
	key.serverid = fpinfo->server->serverid;

	/* See if we already cached the result. */
	entry = (ShippableCacheEntry *)
		hash_search(ShippableCacheHash,
					(void *) &key,
					HASH_FIND,
					NULL);

	if (!entry)
	{
		/* Not found in cache, so perform shippability lookup. */
		bool		shippable = lookup_shippable(objectId, classId, fpinfo);

		/*
		 * Don't create a new hash entry until *after* we have the shippable
		 * result in hand, as the underlying catalog lookups might trigger a
		 * cache invalidation.
		 */
		entry = (ShippableCacheEntry *)
			hash_search(ShippableCacheHash,
						(void *) &key,
						HASH_ENTER,
						NULL);

		entry->shippable = shippable;
	}

	return entry->shippable;
}
