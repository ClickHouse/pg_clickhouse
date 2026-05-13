#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "kv_list.h"

/*
 * tupdesc and attr_nums in ch_query are kept opaque to the C++ wire layer
 * (binary.cpp). PG-aware callers cast through TupleDesc / List * directly.
 */
#ifndef __cplusplus
#include "access/tupdesc.h"
#endif

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
#ifdef __cplusplus
	/* Opaque on the C++ side; unused by binary.cpp. */
	const void *tupdesc;
	const void *attr_nums;
#else
	/* A description of the Tuple for the query. */
	const		TupleDesc tupdesc;
	/* The numbers of the attributes in tupdesc that the query selects. */
	const		List *attr_nums;
#endif
	/* List of settings to pass to ClickHouse upon execution. */
	const		kv_list *settings;
}			ch_query;

#define new_query(sql, num, vals, tupdesc, attrs) {sql, num, vals, tupdesc, attrs, chfdw_get_session_settings()}

#endif							/* CLICKHOUSE_ENGINE_H */
