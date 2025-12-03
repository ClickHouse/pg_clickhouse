\unset ECHO
SET client_min_messages = notice;

CREATE SERVER guc_loopback FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER guc_loopback;

-- Test parsing.
DO $do$
DECLARE
  cfg TEXT;
BEGIN
    FOREACH cfg IN ARRAY ARRAY[
        -- Success.
        '',
        'join_use_nulls=1',
        'join_use_nulls=1, xyz=true',
        $$ additional_result_filter = 'x != 2' $$,
        $$ additional_result_filter = 'x != 2' ,join_use_nulls = 1 $$,
        $$ xxx = DEFAULT, yyy = foo\,bar, zzz = 'He said, \'Hello\'', aaa = hi\ there $$,

        -- Failure.
        'join_use_nulls',
        'join_use_nulls xyz',
        $$ additional_result_filter = 'x != 2 $$,
        'join_use_nulls  = xyz no_preceding_comma = 2'
   ] LOOP
        BEGIN
            RAISE NOTICE 'OK `%`', set_config('pg_clickhouse.session_settings', cfg, true);
        EXCEPTION WHEN OTHERS OR ASSERT_FAILURE THEN
            RAISE NOTICE 'ERR % - %', SQLSTATE, SQLERRM;
        END;
    END LOOP;
END;
$do$ LANGUAGE plpgsql;

CREATE FOREIGN TABLE remote_settings (
    name text,
    value text
)
  SERVER guc_loopback
  OPTIONS (table_name 'settings');

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER guc_loopback;
DROP SERVER guc_loopback CASCADE;
