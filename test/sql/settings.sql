\set VERBOSITY terse
LOAD 'pg_clickhouse';
SET client_min_messages = warning;

-- The "settings" option on servers and foreign tables: syntax, precedence
-- against pg_clickhouse.session_settings, and delivery to ClickHouse, which is
-- checked in system.query_log.

CREATE SERVER vec_settings_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'vec_settings_test', driver 'binary');
CREATE SERVER vec_settings_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'vec_settings_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER vec_settings_bin_svr;
CREATE USER MAPPING FOR CURRENT_USER SERVER vec_settings_http_svr;
CREATE SERVER vec_settings_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER vec_settings_admin;

CALL clickhouse_perform('vec_settings_admin', 'DROP DATABASE IF EXISTS vec_settings_test');
CALL clickhouse_perform('vec_settings_admin', 'CREATE DATABASE vec_settings_test');
CALL clickhouse_perform('vec_settings_admin', $$
    CREATE TABLE vec_settings_test.vec_settings_marker (id Int32, v Int32)
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('vec_settings_admin', $$
    CREATE TABLE vec_settings_test.vec_settings_ins_bin (id Int32, v Int32)
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('vec_settings_admin', $$
    CREATE TABLE vec_settings_test.vec_settings_ins_http (id Int32, v Int32)
    ENGINE = MergeTree ORDER BY id
$$);
CALL clickhouse_perform('vec_settings_admin',
    'INSERT INTO vec_settings_test.vec_settings_marker VALUES (1, 10), (2, 20), (3, 30)');

CREATE FOREIGN TABLE vec_settings_bin (id int, v int)
    SERVER vec_settings_bin_svr OPTIONS (table_name 'vec_settings_marker');
CREATE FOREIGN TABLE vec_settings_bin2 (id int, v int)
    SERVER vec_settings_bin_svr OPTIONS (table_name 'vec_settings_marker');
CREATE FOREIGN TABLE vec_settings_http (id int, v int)
    SERVER vec_settings_http_svr OPTIONS (table_name 'vec_settings_marker');
CREATE FOREIGN TABLE vec_settings_http2 (id int, v int)
    SERVER vec_settings_http_svr OPTIONS (table_name 'vec_settings_marker');
CREATE FOREIGN TABLE vec_settings_ins_bin (id int, v int)
    SERVER vec_settings_bin_svr;
CREATE FOREIGN TABLE vec_settings_ins_http (id int, v int)
    SERVER vec_settings_http_svr;

-- Option handling.
ALTER SERVER vec_settings_bin_svr
    OPTIONS (ADD settings 'hnsw_candidate_list_size_for_search 64');
SELECT srvoptions FROM pg_foreign_server WHERE srvname = 'vec_settings_bin_svr';
ALTER SERVER vec_settings_bin_svr
    OPTIONS (SET settings 'hnsw_candidate_list_size_for_search 128, vector_search_filter_strategy prefilter');
SELECT srvoptions FROM pg_foreign_server WHERE srvname = 'vec_settings_bin_svr';
ALTER SERVER vec_settings_bin_svr OPTIONS (DROP settings);
SELECT srvoptions FROM pg_foreign_server WHERE srvname = 'vec_settings_bin_svr';

ALTER FOREIGN TABLE vec_settings_bin
    OPTIONS (ADD settings 'hnsw_candidate_list_size_for_search 77');
SELECT ftoptions FROM pg_foreign_table WHERE ftrelid = 'vec_settings_bin'::regclass;
ALTER FOREIGN TABLE vec_settings_bin
    OPTIONS (SET settings 'vector_search_index_fetch_multiplier 1.5, vector_search_with_rescoring 1, use_skip_indexes 0');
SELECT ftoptions FROM pg_foreign_table WHERE ftrelid = 'vec_settings_bin'::regclass;
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (DROP settings);
SELECT ftoptions FROM pg_foreign_table WHERE ftrelid = 'vec_settings_bin'::regclass;

-- Unknown keys pass through.
ALTER FOREIGN TABLE vec_settings_bin
    OPTIONS (ADD settings $$some_future_setting 'a, b', other_setting x\ y$$);
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (DROP settings);

-- ClickHouse checks the values, so only bad syntax fails here.
ALTER SERVER vec_settings_bin_svr
    OPTIONS (ADD settings 'use_skip_indexes true');
ALTER SERVER vec_settings_bin_svr OPTIONS (DROP settings);
ALTER SERVER vec_settings_bin_svr
    OPTIONS (ADD settings 'final 1, priority');
ALTER SERVER vec_settings_bin_svr
    OPTIONS (ADD settings $$final 1 'unterminated$$);
ALTER USER MAPPING FOR CURRENT_USER SERVER vec_settings_bin_svr
    OPTIONS (ADD settings 'final 1');
SELECT srvoptions FROM pg_foreign_server WHERE srvname = 'vec_settings_bin_svr';
SELECT ftoptions FROM pg_foreign_table WHERE ftrelid = 'vec_settings_bin'::regclass;

-- Report a setting of the latest query that matches a LIKE pattern. ClickHouse
-- logs only settings that differ from its defaults, so <unset> also means the
-- default value, such as final 0. Read the QueryStart row: ClickHouse may log
-- QueryFinish only after the client has the result.
CREATE FUNCTION vec_settings_seen(setting text, pattern text) RETURNS text
LANGUAGE sql AS $$
    SELECT v FROM clickhouse_query('vec_settings_admin', format($q$
        SELECT if(mapContains(Settings, %1$L), Settings[%1$L], '<unset>')
        FROM system.query_log
        WHERE type = 'QueryStart' AND query LIKE %2$L
          AND query NOT LIKE '%%query_log%%'
        ORDER BY event_time_microseconds DESC LIMIT 1
    $q$, setting, pattern)) AS t(v text)
$$;

-- Populate system.query_log with scans on each driver, then inspect settings.
-- Keep marker parameters as constants so query_log patterns match on every call.
CREATE FUNCTION vec_settings_scan(step int)
RETURNS TABLE (driver text, priority text, final_setting text)
LANGUAGE plpgsql SET plan_cache_mode = force_custom_plan AS $$
BEGIN
    PERFORM id FROM vec_settings_bin WHERE id <> 1000 + step * 10 + 1;
    PERFORM id FROM vec_settings_http WHERE id <> 1000 + step * 10 + 2;
    CALL clickhouse_perform('vec_settings_admin', 'SYSTEM FLUSH LOGS');
    RETURN QUERY SELECT 'binary',
        vec_settings_seen('priority', format('%%vec_settings_marker%%%s%%', 1000 + step * 10 + 1)),
        vec_settings_seen('final', format('%%vec_settings_marker%%%s%%', 1000 + step * 10 + 1));
    RETURN QUERY SELECT 'http',
        vec_settings_seen('priority', format('%%vec_settings_marker%%%s%%', 1000 + step * 10 + 2)),
        vec_settings_seen('final', format('%%vec_settings_marker%%%s%%', 1000 + step * 10 + 2));
END
$$;

-- Insert on each driver and report the settings ClickHouse received.
CREATE FUNCTION vec_settings_insert()
RETURNS TABLE (driver text, max_insert_threads text, final_setting text)
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO vec_settings_ins_bin VALUES (1, 1);
    INSERT INTO vec_settings_ins_http VALUES (1, 1);
    CALL clickhouse_perform('vec_settings_admin', 'SYSTEM FLUSH LOGS');
    RETURN QUERY SELECT 'binary',
        vec_settings_seen('max_insert_threads', 'INSERT INTO %vec_settings_ins_bin%'),
        vec_settings_seen('final', 'INSERT INTO %vec_settings_ins_bin%');
    RETURN QUERY SELECT 'http',
        vec_settings_seen('max_insert_threads', 'INSERT INTO %vec_settings_ins_http%'),
        vec_settings_seen('final', 'INSERT INTO %vec_settings_ins_http%');
END
$$;

-- Precedence. Nothing set: built-in defaults apply.
SELECT * FROM vec_settings_scan(1);

-- Server settings beat the default.
ALTER SERVER vec_settings_bin_svr OPTIONS (ADD settings 'priority 2, final 0');
ALTER SERVER vec_settings_http_svr OPTIONS (ADD settings 'priority 2, final 0');
SELECT * FROM vec_settings_scan(2);

-- Table settings beat the server, key by key.
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (ADD settings 'priority 3');
ALTER FOREIGN TABLE vec_settings_http OPTIONS (ADD settings 'priority 3');
SELECT * FROM vec_settings_scan(3);

-- A table setting beats the built-in default.
ALTER SERVER vec_settings_bin_svr OPTIONS (DROP settings);
ALTER SERVER vec_settings_http_svr OPTIONS (DROP settings);
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (SET settings 'priority 3, final 0');
ALTER FOREIGN TABLE vec_settings_http OPTIONS (SET settings 'priority 3, final 0');
SELECT * FROM vec_settings_scan(4);

-- session_settings beats the table, key by key.
SET pg_clickhouse.session_settings = 'final 1';
SELECT * FROM vec_settings_scan(5);

-- RESET leaves the table settings.
RESET pg_clickhouse.session_settings;
SELECT * FROM vec_settings_scan(6);

-- An empty session_settings leaves the table settings and built-in defaults.
SET pg_clickhouse.session_settings = '';
SELECT * FROM vec_settings_scan(7);
RESET pg_clickhouse.session_settings;

-- Session settings keep built-in defaults the table does not override.
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (SET settings 'priority 3');
ALTER FOREIGN TABLE vec_settings_http OPTIONS (SET settings 'priority 3');
SET pg_clickhouse.session_settings = 'priority 7';
SELECT * FROM vec_settings_scan(8);
RESET pg_clickhouse.session_settings;
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (SET settings 'priority 3, final 0');
ALTER FOREIGN TABLE vec_settings_http OPTIONS (SET settings 'priority 3, final 0');

-- Joins and aggregates. Combine table settings; ignore conflicting table values.
CREATE FUNCTION vec_settings_join(step int)
RETURNS TABLE (shape text, join_pushed bool, priority text, final_setting text)
LANGUAGE plpgsql AS $$
DECLARE
    m int := 2000 + step * 10;
    q text[] := ARRAY[
        format('SELECT a.id FROM vec_settings_bin a JOIN vec_settings_bin2 b ON a.id = b.id WHERE a.id <> %s', m + 1),
        format('SELECT a.id FROM vec_settings_http a LEFT JOIN vec_settings_http2 b ON a.id = b.id WHERE a.id <> %s', m + 2),
        format('SELECT count(*) FROM vec_settings_bin WHERE id <> %s', m + 3),
        format('SELECT a.id, count(*) FROM vec_settings_http a JOIN vec_settings_http2 b ON a.id = b.id WHERE a.id <> %s GROUP BY a.id', m + 4)
    ];
    pushed bool[] := ARRAY[false, false, false, false];
    line text;
BEGIN
    -- Populate system.query_log for each query shape; retain dynamic EXPLAIN.
    PERFORM a.id FROM vec_settings_bin a JOIN vec_settings_bin2 b ON a.id = b.id WHERE a.id <> m + 1;
    PERFORM a.id FROM vec_settings_http a LEFT JOIN vec_settings_http2 b ON a.id = b.id WHERE a.id <> m + 2;
    PERFORM count(*) FROM vec_settings_bin WHERE id <> m + 3;
    PERFORM a.id, count(*) FROM vec_settings_http a JOIN vec_settings_http2 b ON a.id = b.id WHERE a.id <> m + 4 GROUP BY a.id;
    FOR i IN 1..4 LOOP
        FOR line IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q[i] LOOP
            pushed[i] := pushed[i] OR line LIKE '%Relations:%JOIN%';
        END LOOP;
    END LOOP;
    CALL clickhouse_perform('vec_settings_admin', 'SYSTEM FLUSH LOGS');
    RETURN QUERY SELECT x.shape, pushed[x.n],
        vec_settings_seen('priority', format('%%vec_settings_marker%%%s%%', m + x.n)),
        vec_settings_seen('final', format('%%vec_settings_marker%%%s%%', m + x.n))
    FROM (VALUES (1, 'binary join'), (2, 'http left join'), (3, 'binary aggregate'),
                 (4, 'http join aggregate')) AS x(n, shape)
    ORDER BY x.n;
END
$$;

ALTER SERVER vec_settings_bin_svr OPTIONS (ADD settings 'priority 2');
ALTER SERVER vec_settings_http_svr OPTIONS (ADD settings 'priority 2');
SELECT * FROM vec_settings_join(1);

ALTER FOREIGN TABLE vec_settings_bin2 OPTIONS (ADD settings 'priority 3, final 0');
ALTER FOREIGN TABLE vec_settings_http2 OPTIONS (ADD settings 'priority 3, final 0');
SELECT * FROM vec_settings_join(2);

ALTER FOREIGN TABLE vec_settings_bin2 OPTIONS (SET settings 'priority 4');
ALTER FOREIGN TABLE vec_settings_http2 OPTIONS (SET settings 'priority 4');
SELECT * FROM vec_settings_join(3);

-- Conflict notices report effective values after precedence.
\set VERBOSITY default
SET client_min_messages = notice;
DO $$ BEGIN
    PERFORM a.id FROM vec_settings_bin a JOIN vec_settings_bin2 b ON a.id = b.id;
END $$;
SET pg_clickhouse.session_settings = 'priority 7';
DO $$ BEGIN
    PERFORM a.id FROM vec_settings_bin a JOIN vec_settings_bin2 b ON a.id = b.id;
END $$;
RESET pg_clickhouse.session_settings;
SET client_min_messages = warning;
ALTER SERVER vec_settings_bin_svr OPTIONS (DROP settings);
DO $$ BEGIN
    PERFORM id FROM vec_settings_bin;
END $$;
SET client_min_messages = notice;
DO $$ BEGIN
    PERFORM a.id FROM vec_settings_bin a JOIN vec_settings_bin2 b ON a.id = b.id;
END $$;
SET client_min_messages = warning;
\set VERBOSITY terse

ALTER SERVER vec_settings_http_svr OPTIONS (DROP settings);

-- Inserts use the same settings.
SELECT * FROM vec_settings_insert();
ALTER SERVER vec_settings_bin_svr OPTIONS (ADD settings 'max_insert_threads 2');
ALTER SERVER vec_settings_http_svr OPTIONS (ADD settings 'max_insert_threads 2');
SELECT * FROM vec_settings_insert();
ALTER FOREIGN TABLE vec_settings_ins_bin OPTIONS (ADD settings 'max_insert_threads 5');
ALTER FOREIGN TABLE vec_settings_ins_http OPTIONS (ADD settings 'max_insert_threads 5');
SELECT * FROM vec_settings_insert();
SET pg_clickhouse.session_settings = 'max_insert_threads 6';
SELECT * FROM vec_settings_insert();
RESET pg_clickhouse.session_settings;
ALTER SERVER vec_settings_bin_svr OPTIONS (DROP settings);
ALTER SERVER vec_settings_http_svr OPTIONS (DROP settings);

-- Known vector settings are sent only to servers that support them, and are
-- skipped with a warning on older ones. Other settings always go. The warning
-- names the server version, so hide it.
SELECT (regexp_split_to_array(clickhouse_server_version('vec_settings_admin'), '\.'))[1:2]::int[]
       >= '{25,8}'::int[] AS ch_ge_25_8 \gset
SELECT (regexp_split_to_array(clickhouse_server_version('vec_settings_admin'), '\.'))[1:2]::int[]
       >= '{26,7}'::int[] AS ch_ge_26_7 \gset
SET client_min_messages = error;
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (SET settings
    'max_limit_for_vector_search_queries 777, use_skip_indexes 0, vector_search_use_quantized_codes 1, priority 5');
ALTER FOREIGN TABLE vec_settings_http OPTIONS (SET settings
    'max_limit_for_vector_search_queries 777, use_skip_indexes 0, vector_search_use_quantized_codes 1, priority 5');
SELECT id FROM vec_settings_bin WHERE id <> 3001;
SELECT id FROM vec_settings_http WHERE id <> 3002;
SELECT id FROM vec_settings_bin WHERE id <> 3003;
CALL clickhouse_perform('vec_settings_admin', 'SYSTEM FLUSH LOGS');
SELECT n,
       vec_settings_seen('max_limit_for_vector_search_queries', format('%%vec_settings_marker%%%s%%', n))
           = CASE WHEN :'ch_ge_25_8'::bool THEN '777' ELSE '<unset>' END AS limit_sent_if_supported,
       vec_settings_seen('vector_search_use_quantized_codes', format('%%vec_settings_marker%%%s%%', n))
           = CASE WHEN :'ch_ge_26_7'::bool THEN '1' ELSE '<unset>' END AS codes_sent_if_supported,
       vec_settings_seen('use_skip_indexes', format('%%vec_settings_marker%%%s%%', n)) AS use_skip_indexes,
       vec_settings_seen('priority', format('%%vec_settings_marker%%%s%%', n)) AS priority
FROM (VALUES (3001), (3002)) AS t(n) ORDER BY n;

-- The same goes for session_settings.
ALTER FOREIGN TABLE vec_settings_bin OPTIONS (DROP settings);
SET pg_clickhouse.session_settings = 'max_limit_for_vector_search_queries 888, priority 6';
SELECT id FROM vec_settings_bin WHERE id <> 3004;
RESET pg_clickhouse.session_settings;
CALL clickhouse_perform('vec_settings_admin', 'SYSTEM FLUSH LOGS');
SELECT vec_settings_seen('max_limit_for_vector_search_queries', '%vec_settings_marker%3004%')
           = CASE WHEN :'ch_ge_25_8'::bool THEN '888' ELSE '<unset>' END AS limit_sent_if_supported,
       vec_settings_seen('priority', '%vec_settings_marker%3004%') AS priority;
SET client_min_messages = warning;

CALL clickhouse_perform('vec_settings_admin', 'DROP DATABASE vec_settings_test');
DROP FUNCTION vec_settings_seen(text, text), vec_settings_scan(int),
    vec_settings_insert(), vec_settings_join(int);
DROP USER MAPPING FOR CURRENT_USER SERVER vec_settings_bin_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER vec_settings_http_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER vec_settings_admin;
DROP SERVER vec_settings_bin_svr CASCADE;
DROP SERVER vec_settings_http_svr CASCADE;
DROP SERVER vec_settings_admin CASCADE;
