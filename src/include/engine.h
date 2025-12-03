#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "nodes/pathnodes.h"

/* 
 * ch_connection_details defines the details for connecting to ClickHouse.
 */
typedef struct
{
	char	   *host;
	int			port;
	char	   *username;
	char	   *password;
	char	   *dbname;
}			ch_connection_details;

/* 
 * ch_connection_details an SQL query to execute on ClickHouse.
 */
typedef struct
{
	const char	   *sql;
}			ch_query;

#define new_query(q) ch_query {q}

#endif							/* CLICKHOUSE_ENGINE_H */
