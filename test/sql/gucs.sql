\unset ECHO
SET client_min_messages = notice;

-- Load pg_clickhouse;
LOAD 'pg_clickhouse';

-- Test parsing.
DO $do$
DECLARE
  cfg TEXT;
BEGIN
    FOREACH cfg IN ARRAY ARRAY[
        -- Success.
        '',
        'join_use_nulls 1',
        'join_use_nulls 1, xyz true',
        $$ additional_result_filter 'x != 2' $$,
        $$ additional_result_filter 'x != 2' ,join_use_nulls 1 $$,
        $$ xxx DEFAULT, yyy foo\,bar, zzz    'He said, \'Hello\'', aaa  hi\ there $$,

        -- Failure.
        'join_use_nulls',
        'join_use_nulls = xyz',
        $$ additional_result_filter 'x != 2 $$,
        'join_use_nulls  xyz no_preceding_comma = 2'
   ] LOOP
        BEGIN
            RAISE NOTICE 'OK `%`', set_config('pg_clickhouse.session_settings', cfg, true);
        EXCEPTION WHEN OTHERS OR ASSERT_FAILURE THEN
            RAISE NOTICE 'ERR % - %', SQLSTATE, SQLERRM;
        END;
    END LOOP;
END;
$do$ LANGUAGE plpgsql;

-- Create servers for each engine.
CREATE SERVER guc_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER guc_bin_svr;

CREATE SERVER guc_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER guc_http_svr;

-- Create foreign tables for each engine.
CREATE FOREIGN TABLE bin_remote_settings (
    name text,
    value text
)
  SERVER guc_bin_svr
  OPTIONS (table_name 'settings');

CREATE FOREIGN TABLE http_remote_settings (
    name text,
    value text
)
  SERVER guc_http_svr
  OPTIONS (table_name 'settings');

-- Reset to defaults.
RESET pg_clickhouse.session_settings;
SELECT name, value FROM bin_remote_settings
 WHERE name IN ('join_use_nulls', 'group_by_use_nulls', 'final')
 ORDER BY name;

SELECT name, value FROM http_remote_settings
 WHERE name IN ('join_use_nulls', 'group_by_use_nulls', 'final')
 ORDER BY name;

-- List of seeings changed below.
select '{
    connect_timeout,
    count_distinct_implementation,
    join_algorithm,
    join_use_nulls,
    log_queries_min_type,
    max_block_size,
    max_execution_time,
    max_result_rows,
    metrics_perf_events_list,
    network_compression_method,
    poll_interval,
    totals_mode
}' set_list \gset

-- Unset and get defaults.
SET pg_clickhouse.session_settings TO '';

CREATE TEMPORARY TABLE default_settings AS
SELECT name, value
  FROM bin_remote_settings
 WHERE name = ANY(:'set_list');

-- Customize all of the above settings.
SET pg_clickhouse.session_settings TO $$
    connect_timeout 2,
    count_distinct_implementation uniq,
    join_algorithm 'prefer_partial_merge',
    join_use_nulls 0,
    join_use_nulls 1,
    log_queries_min_type QUERY_FINISH,
    max_block_size 32768,
    max_execution_time 45,
    max_result_rows 1024,
    metrics_perf_events_list 'this,that',
    network_compression_method ZSTD,
    poll_interval 5,
    totals_mode after_having_auto
$$;

SHOW pg_clickhouse.session_settings;

-- Check the remote settings for both engines.
SELECT name, value
  FROM bin_remote_settings
 WHERE name = ANY(:'set_list')
 ORDER BY name;

SELECT name, value
  FROM http_remote_settings
WHERE name = ANY(:'set_list')
ORDER BY name;

-- Unset back to defaults.
SET pg_clickhouse.session_settings TO '';

SELECT remote.name, remote.value IS NOT DISTINCT FROM def.value
 FROM bin_remote_settings remote
 JOIN default_settings def ON remote.name = def.name
WHERE remote.name = ANY(:'set_list')
ORDER BY remote.name;

SELECT remote.name, remote.value IS NOT DISTINCT FROM def.value
 FROM http_remote_settings remote
 JOIN default_settings def ON remote.name = def.name
WHERE remote.name = ANY(:'set_list')
ORDER BY remote.name;

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER guc_bin_svr;
DROP SERVER guc_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER guc_http_svr;
DROP SERVER guc_http_svr CASCADE;
