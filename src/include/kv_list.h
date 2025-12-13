#ifndef PG_CLICKHOUSE_KV_LIST_H
#define PG_CLICKHOUSE_KV_LIST_H

#include <stdbool.h>
#include "postgres.h"
#include "nodes/pathnodes.h"

/*
 * A simple data structure with a list of key/value string pairs. Use
 * new_kv_list_from_list() to create.
 */
typedef struct kv_list
{
	int			length;
	/* key/value char * pairs follow the length field. */
}			kv_list;

/*
 * Iterator for a kv_list. Use new_kv_iter() to create.
 *
 *     for (kv_iter iter = new_kv_iter(kv); !kv_iter_done(&iter); kv_iter_next(&iter))
 *     {
 *         printf("%i, %s => %s\n", iter.num, iter.name, iter.value);
 *     }
 */
typedef struct kv_iter {
    int togo;
    char * name;
    char * value;
} kv_iter;

/*
 * Defines the allocator to use when creating a new kv_list.
 * kv_pair_guc_malloc is the same as kv_pair_malloc on Postgres 15 and
 * earlier, so be sure to free() the memory on those versions.
 */
enum kv_pair_alloc
{
	kv_pair_guc_malloc,
	kv_pair_malloc,
	kv_pair_palloc,
};

/*
 * Create a new kv_list from a PostgreSQL List of DefElem. Allocate the memory
 * using the specified allocator.
*/
kv_list * new_kv_list_from_pg_list(List * list, int allocate);

/* Create a new kv_iter for a key_pairs. */
kv_iter new_kv_iter(const kv_list * ns);

/* Iterate to the next item. Returns false if there are no items. */
bool kv_iter_next(kv_iter * state);

/* Retruns true if iteration by kv_iter is complete. */
bool kv_iter_done(kv_iter * state);

#endif							/* PG_CLICKHOUSE_KV_LIST_H */
