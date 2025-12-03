#ifndef CLICKHOUSE_CONNECT_H
#define CLICKHOUSE_CONNECT_H

#include "nodes/pathnodes.h"

typedef struct
{
	char	   *host;
	int			port;
	char	   *username;
	char	   *password;
	char	   *dbname;
}			ch_connection_details;

#endif							/* CLICKHOUSE_CONNECT_H */
