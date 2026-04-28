#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "kv_list.h"

/*
 * ch_connection_details defines the details for connecting to ClickHouse.
 */
/* TLS mode for the "secure" FDW option (auto=heuristic, on=force, off=never) */
typedef enum
{
	CH_TLS_AUTO = 0,			/* cloud-hostname heuristic (default) */
	CH_TLS_ON,					/* always HTTPS; default port 8443 */
	CH_TLS_OFF,					/* always HTTP;  default port 8123 */
}			ch_tls_mode;

typedef struct
{
	char	   *host;
	int			port;
	char	   *username;
	char	   *password;
	char	   *dbname;
	ch_tls_mode tls;			/* TLS mode; CH_TLS_AUTO when not specified */
}			ch_connection_details;

/*
 * ch_query an SQL query to execute on ClickHouse.
 */
typedef struct
{
	const char *sql;
	const int	num_params;
	const char **param_values;
	const		kv_list *settings;
}			ch_query;

#define new_query(sql, num, vals) {sql, num, vals, chfdw_get_session_settings()}

#endif							/* CLICKHOUSE_ENGINE_H */
