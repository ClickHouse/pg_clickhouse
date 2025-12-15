#ifndef PG_CLICKHOUSE_KV_LIST_H
#define PG_CLICKHOUSE_KV_LIST_H

#include <stdbool.h>
#include "postgres.h"
#include "nodes/pathnodes.h"

/* A key/value pair. */
typedef struct kv_pair
{
	char	   *name;
	char	   *value;
}			kv_pair;

/*
 * A simple data structure with a list of key/value string pairs. Use
 * new_kv_list_from_list() to create.
 *
 *     kv_list * pairs = new_kv_list_from_pg_list(list);
 *     if (!kv_list)
 *     {
 *          printf("out of memory\n");
 *          exit(1);
 *     }
 *
 *     for (int i = 0; i < kv_list->length; i++) {
 *         kv_pair * pair = kv_list->items[i];
 *         printf("%s => %s\n", pair->name, pair->value);
 *     }
 *
 *     kv_list_free(pairs);
 */
typedef struct kv_list
{
	int			length;
	kv_pair		**items;
}			kv_list;

/*
 * Create an empty kv_list, with length set to zero and items uninitialized.
*/
kv_list * new_empty_kv_list(void);

/*
 * Create a new kv_list from a PostgreSQL List of DefElem. Logs an error
 * message and returns NULL on memory errors.
*/
kv_list * new_kv_list_from_pg_list(List * list);

/* Frees the memory owned by the kv_list. */
void kv_list_free(kv_list * pairs);

#endif							/* PG_CLICKHOUSE_KV_LIST_H */
