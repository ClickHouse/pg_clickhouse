#ifndef CLICKHOUSE_ENGINE_H
#define CLICKHOUSE_ENGINE_H

#include "kv_list.h"
#include "access/tupdesc.h"

/*
 * ch_connection_details defines the details for connecting to ClickHouse.
 */
/* TLS mode for the "secure" FDW option (auto=heuristic, on=force, off=never) */
typedef enum
{
	CH_TLS_AUTO = 0,			/* cloud-hostname heuristic (default) */
	CH_TLS_ON,					/* force TLS; default port 8443 or 9440 */
	CH_TLS_OFF,					/* force plaintext; default port 8123 or 9000 */
} tls_mode;

/* Minimum TLS protocol version for the "min_tls_version" FDW option */
typedef enum
{
	CH_TLS_DEFAULT = 0,			/* library default; no minimum forced */
	CH_TLS_V1_0,
	CH_TLS_V1_1,
	CH_TLS_V1_2,
	CH_TLS_V1_3,
} tls_version;

typedef struct
{
	char	   *host;
	int			port;
	char	   *username;
	char	   *password;
	char	   *dbname;
	char	   *compression;
	tls_mode	tls;			/* TLS mode; CH_TLS_AUTO when not specified */
	tls_version min_tls_version;	/* minimum TLS version; CH_TLS_DEFAULT
									 * when not specified */
} ch_connection_details;

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
	const TupleDesc tupdesc;
	/* The numbers of the attributes in tupdesc that the query selects. */
	const List *attr_nums;
	/* List of settings to pass to ClickHouse upon execution. */
	const kv_list *settings;
} ch_query;

#define new_query(sql, num, vals, tupdesc, attrs) {sql, num, vals, tupdesc, attrs, chfdw_get_session_settings()}

#endif							/* CLICKHOUSE_ENGINE_H */
