/*
	A PostgreSQL function for getting the an environment variable value.
*/

#include "postgres.h"
#include <stdlib.h>
#include "utils/builtins.h"
#include "include/binary.hh"
#include "include/internal.h"
#include "include/fdw.h"
#if PG_VERSION_NUM >= 160000
#include "varatt.h"
#endif

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1( get_env );

Datum get_env( PG_FUNCTION_ARGS );

Datum get_env( PG_FUNCTION_ARGS ) {
	const char* val = NULL;
	size_t length = 0;
	text *t = NULL;

	text* name = PG_GETARG_TEXT_PP(0);

	val = getenv(text_to_cstring(name));
	if (!val) PG_RETURN_NULL();

    /* Return text. */
	length = strlen( val );
	t = (text *) palloc(VARHDRSZ + length );
	SET_VARSIZE( t, VARHDRSZ + length );
	memcpy( VARDATA(t), val, length );
	PG_RETURN_TEXT_P( t );
}

ch_connection
chfdw_binary_connect(ch_connection_details *details)
{
	char *ch_error = NULL;
	ch_connection res;
	ch_binary_connection_t *conn = ch_binary_connect(details->host, details->port,
			details->dbname, details->username, details->password, &ch_error);

	if (conn == NULL)
	{
		Assert(ch_error);
		char *error = pstrdup(ch_error);
		free(ch_error);

		ereport(ERROR,
				(errcode(ERRCODE_SQLCLIENT_UNABLE_TO_ESTABLISH_SQLCONNECTION),
				 errmsg("clickhouse_fdw: connection error: %s", error)));
	}

	res.conn = conn;
	// res.methods = &binary_methods;
	res.is_binary = true;
	return res;
}
