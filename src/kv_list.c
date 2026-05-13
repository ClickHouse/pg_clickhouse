#include <stddef.h>
#include "kv_list.h"
#include "utils/elog.h"
#include "nodes/pathnodes.h"
#include "utils/guc.h"

static kv_list * allocate_list(size_t size, int allocate)
{
	kv_list    *pairs;

	switch (allocate)
	{
		case kv_pair_guc_malloc:
			/* Fall through to malloc prior to Postgres 16. */
#if PG_VERSION_NUM >= 160000
			pairs = guc_malloc(ERROR, size);
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
	size_t		kv_size = 0;
	size_t		ck_size = 0;

	/* Count up space for dynamic key/value pairs. */
	foreach(lc, list)
	{
		elem = (DefElem *) lfirst(lc);
		kv_size += 2 + strlen(elem->defname) + strlen(strVal(elem->arg));
	}

	/* Alloc the result ... */
	pairs = allocate_list(offsetof(kv_list, data) + kv_size, allocate);

	/* ... and fill it in */
	pairs->length = list_length(list);

	/* In this loop, size ck_size reprises the kv_size calculation above. */
	foreach(lc, list)
	{
		char	   *str;

		/* Append the element name and arg that constitute the pair. */
		elem = (DefElem *) lfirst(lc);
		str = (char *) pairs->data + ck_size;
		size_t		len = strlen(elem->defname) + 1;

		memcpy(str, elem->defname, len);
		ck_size += len;
		str = (char *) pairs->data + ck_size;
		len = strlen(strVal(elem->arg)) + 1;
		memcpy(str, strVal(elem->arg), len);
		ck_size += len;
	}

	/* Assert the two loops agreed on size calculations. */
	Assert(kv_size == ck_size);

	return pairs;
}

extern kv_iter new_kv_iter(const kv_list * ns)
{
	char	   *name;

	/* The list may be NULL or empty. */
	if (!ns || ns->length < 1)
		return (kv_iter)
	{
		0,
	};

	/* Grab the number of pairs point to the first name and value. */
	name = (char *) ns->data;
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

	/* Point to the the next name and value. */
	iter->togo--;
	iter->name = iter->value + strlen(iter->value) + 1;
	iter->value = iter->name + strlen(iter->name) + 1;
	return true;
}

extern bool
kv_iter_done(kv_iter * iter)
{
	return iter->togo == 0;
}
