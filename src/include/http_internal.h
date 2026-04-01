#ifndef CLICKHOUSE_HTTP_INTERNAL_H
#define CLICKHOUSE_HTTP_INTERNAL_H

#include "http.h"
#include "internal.h"

#define CH_HTTP_CANCELED_STATUS 418
#define CH_HTTP_COMM_ERROR_STATUS 419
#define CH_HTTP_STREAM_BUFFER_SIZE (64 * 1024)
#define CH_MINICORO_STACK_SIZE (1024 * 1024)

typedef size_t (*ch_http_write_func) (void *contents, size_t size,
									  size_t nmemb, void *userp);

void		ch_http_generate_query_id(char query_id[37]);
bool		ch_http_build_url(ch_http_connection_t * conn, const ch_query * query,
							  const char *query_id, char **url);
bool		ch_http_build_headers(ch_http_connection_t * conn,
								  struct curl_slist **headers);
bool		ch_http_set_post_data(CURL *curl, const ch_query * query,
								  curl_mime **form);
void		ch_http_set_common_options(ch_http_connection_t * conn, const char *url,
										char *errbuffer,
										ch_http_write_func write_func,
										void *write_data);
void		ch_http_set_progress_options(ch_http_connection_t * conn,
										 void *progress_data);
void		ch_http_get_transfer_info(ch_http_connection_t * conn,
									  double *pretransfer_time,
									  double *total_time,
									  long *http_status);

#endif							/* CLICKHOUSE_HTTP_INTERNAL_H */
