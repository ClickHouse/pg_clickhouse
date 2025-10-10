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

/*
 * Check whether the given string matches a ClickHouse Cloud host name.
 */
extern int ch_is_cloud_host(const char *host);
int ends_with(const char *s, const char *suffix);

#endif /* CLICKHOUSE_INTERNAL_H */
