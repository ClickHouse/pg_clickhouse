#include <stddef.h>
#include "kv_list.h"
#include "utils/elog.h"
#include "nodes/pathnodes.h"
#include "utils/guc.h"

static const size_t kv_list_struct_size = MAXALIGN(sizeof(kv_list));

static kv_list * allocate_list(size_t size, int allocate)
{
	kv_list    *pairs;
	switch (allocate)
	{
		case kv_pair_guc_malloc:
		/* Fall through to malloc prior to Postgres 16. */
#if PG_VERSION_NUM >= 160000
			pairs = guc_malloc(LOG, size);
			break;
#endif
		case kv_pair_malloc:
			pairs = malloc(size);
			break;
		case kv_pair_palloc:
			pairs = palloc(size);
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_ERROR),
					 errmsg("unknown kv_pair_alloc %i", allocate)));
	}

	if (!pairs)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	return pairs;
}

extern kv_list * new_kv_list_from_pg_list(List * list, int allocate)
{
	ListCell   *lc;
	DefElem    *elem;
	kv_list    *pairs;
	size_t		ck_size = kv_list_struct_size;
	size_t		kv_size = kv_list_struct_size;

	/* Count up space for dynamic key/value pairs. */
	foreach(lc, list)
	{
		elem = (DefElem *) lfirst(lc);
		kv_size += 2 + strlen(elem->defname) + strlen(strVal(elem->arg));
	}

	/* Alloc the result ... */
	pairs = allocate_list(kv_size, allocate);

	/* ... and fill it in */
	pairs->length = list_length(list);

	/* in this loop, size reprises the space calculation above */
	foreach(lc, list)
	{
		char	   *str;

		/* Append the element name and arg to the kv_pair. */
		elem = (DefElem *) lfirst(lc);
		str = (char *) pairs + ck_size;
		strcpy(str, elem->defname);
		ck_size += strlen(str) + 1;
		str = (char *) pairs + ck_size;
		strcpy(str, strVal(elem->arg));
		ck_size += strlen(str) + 1;
	}

	/* Assert the two loops above agreed on size calculations */
	Assert(kv_size == ck_size);

	return pairs;
}

extern kv_iter new_kv_iter(const kv_list * ns)
{
	const		size_t kv_size = kv_list_struct_size;
	char	   *name;

	/* The list may be NULL or empty. */
	if (!ns || ns->length < 1)
		return (kv_iter)
	{
		0, "", ""
	};

	/* Grab the number of pairs and char pointers to the name and value. */
	name = (char *) ns + kv_size;
	return (kv_iter)
	{
		ns->length, name, name + strlen(name) + 1
	};
}

extern bool
kv_iter_next(kv_iter * iter)
{
	if (iter->togo == 0)
		return false;
	iter->togo--;
	iter->name += 2 + strlen(iter->name) + strlen(iter->value);
	iter->value = iter->name + strlen(iter->name) + 1;
	return true;
}

extern bool
kv_iter_done(kv_iter * iter)
{
	return iter->togo == 0;
}
