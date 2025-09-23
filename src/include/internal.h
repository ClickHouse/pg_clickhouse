#ifndef CLICKHOUSE_INTERNAL_H
#define CLICKHOUSE_INTERNAL_H

#include "curl/curl.h"

typedef struct ch_http_connection_t
{
	CURL			   *curl;
	char			   *base_url;
	size_t				base_url_len;
} ch_http_connection_t;

typedef struct ch_binary_connection_t
{
	void			  *client;
	void			  *options;
	char			  *error;
} ch_binary_connection_t;

#endif
