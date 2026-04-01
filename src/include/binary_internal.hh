#ifndef CLICKHOUSE_BINARY_INTERNAL_HH
#define CLICKHOUSE_BINARY_INTERNAL_HH

#ifdef __cplusplus
extern "C"
{
#endif

clickhouse::QuerySettings ch_binary_settings(const ch_query * query);
clickhouse::QueryParams ch_binary_params(const ch_query * query);
Datum		ch_binary_make_datum(clickhouse::ColumnRef col, size_t row,
								 Oid * valtype, bool * is_null);

#ifdef __cplusplus
}
#endif

#endif							/* CLICKHOUSE_BINARY_INTERNAL_HH */
