#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "kv_list.h"
#include "access/tupdesc.h"

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
 * ch_query an SQL query to execute on ClickHouse.
 */
typedef struct
{
	/* The SQL query. */
	const char *sql;
	/* The number of parameters in the query. */
	const int	num_params;
	/* The list of parameters to pass when executing the query. */
	const char **param_values;
	/* A description of the Tuple for the query. */
	const		TupleDesc tupdesc;
	/* The numbers of the attributes in tupdesc that the query selects. */
	const		List *attr_nums;
	/* List of settings to pass to ClickHouse upon execution. */
	const		kv_list *settings;
}			ch_query;

#define new_query(sql, num, vals, tupdesc, attrs) {sql, num, vals, tupdesc, attrs, chfdw_get_session_settings()}

#endif							/* CLICKHOUSE_ENGINE_H */
