#include <stddef.h>
#include "kv_list.h"
#include "utils/elog.h"
#include "nodes/pathnodes.h"
#include "utils/guc.h"

extern kv_list * new_empty_kv_list(void)
{
	kv_list    *pairs = (kv_list *) malloc(sizeof(kv_list));

	if (pairs == NULL)
	{
		ereport(LOG, errcode(ERRCODE_FDW_OUT_OF_MEMORY), errmsg("out of memory"));
		return NULL;
	}
	pairs->length = 0;
	return pairs;
}

extern kv_list * new_kv_list_from_pg_list(List * list)
{
	ListCell   *lc;
	DefElem    *elem;
	kv_list    *pairs;
	int			i = 0;

	/* Allocate the memory for the pairs object. */
	pairs = (kv_list *) malloc(sizeof(kv_list));
	if (pairs == NULL)
	{
		ereport(LOG, errcode(ERRCODE_FDW_OUT_OF_MEMORY), errmsg("out of memory"));
		return NULL;
	}

	/* Allocate the memory for kv_pair pointers to all the items in list. */
	pairs->items = (kv_pair * *) malloc(list_length(list) * sizeof(kv_pair *));
	if (pairs->items == NULL)
	{
		free(pairs);
		ereport(LOG, errcode(ERRCODE_FDW_OUT_OF_MEMORY), errmsg("out of memory"));
		return NULL;
	}
	pairs->length = list_length(list);

	/* Copy the values from list into pairs. */
	foreach(lc, list)
	{
		kv_pair    *pair = malloc(sizeof(kv_pair));

		if (!pair)
		{
			kv_list_free(pairs);
			ereport(LOG, errcode(ERRCODE_FDW_OUT_OF_MEMORY), errmsg("out of memory"));
			return NULL;
		}
		elem = (DefElem *) lfirst(lc);
		pair->name = strdup(elem->defname);
		pair->value = strdup(strVal(elem->arg));
		if (!pair->name || !pair->value)
		{
			kv_list_free(pairs);
			ereport(LOG, errcode(ERRCODE_FDW_OUT_OF_MEMORY), errmsg("out of memory"));
			return NULL;
		}
		pairs->items[i] = pair;
		i++;
	}

	return pairs;
}

extern void
kv_list_free(kv_list * pairs)
{
	if (pairs == NULL)
		return;
	for (int i = 0; i < pairs->length; i++)
		if (pairs->items[i] != NULL)
		{
			if (pairs->items[i]->name)
				free(pairs->items[i]->name);
			if (pairs->items[i]->value)
				free(pairs->items[i]->value);
			free(pairs->items[i]);
		}
	free(pairs->items);
	free(pairs);
}
