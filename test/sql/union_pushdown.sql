-- Tests for UNION and safely inlined CTE pushdown.
--
-- Plan checks use JSON instead of printing the full EXPLAIN tree so the
-- expected output remains stable across PostgreSQL 13-19.
CREATE SERVER union_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'union_pushdown_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER union_svr;

CREATE SERVER union_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'union_pushdown_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER union_http_svr;

-- A distinct server object pointing at the same ClickHouse database verifies
-- that server identity, rather than matching connection options, gates UNION
-- pushdown.
CREATE SERVER union_other_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (dbname 'union_pushdown_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER union_other_svr;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS union_pushdown_test');
SELECT clickhouse_raw_query('CREATE DATABASE union_pushdown_test');
SELECT clickhouse_raw_query('CREATE TABLE union_pushdown_test.messages
    (message_id Int32, is_selected Int32)
    ENGINE = MergeTree ORDER BY message_id');
SELECT clickhouse_raw_query('CREATE TABLE union_pushdown_test.source_a
    (message_id Int32, category_id Int32)
    ENGINE = MergeTree ORDER BY message_id');
SELECT clickhouse_raw_query('CREATE TABLE union_pushdown_test.collapsing_source
    (message_id Int32, category_id Int32, sign Int8)
    ENGINE = CollapsingMergeTree(sign) ORDER BY message_id');
SELECT clickhouse_raw_query('CREATE TABLE union_pushdown_test.source_b
    (message_id Int32, category_id Int32)
    ENGINE = MergeTree ORDER BY message_id');
SELECT clickhouse_raw_query('CREATE TABLE union_pushdown_test.bpchar_values
    (value String, arm Int32)
    ENGINE = MergeTree ORDER BY arm');

SELECT clickhouse_raw_query($$
    INSERT INTO union_pushdown_test.messages VALUES
    (1, 1), (2, 1), (3, 0), (4, 1)
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO union_pushdown_test.source_a VALUES
    (1, 10), (2, 20), (3, 90), (4, 40)
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO union_pushdown_test.source_b VALUES
    (1, 20), (2, 30), (3, 90), (4, 40)
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO union_pushdown_test.bpchar_values VALUES
    ('x', 1), ('x ', 2)
$$);

CREATE SCHEMA union_pushdown_test;
CREATE FOREIGN TABLE union_pushdown_test.messages (
    message_id integer NOT NULL,
    is_selected integer NOT NULL
) SERVER union_svr OPTIONS (table_name 'messages');
CREATE FOREIGN TABLE union_pushdown_test.source_a (
    message_id integer NOT NULL,
    category_id integer NOT NULL
) SERVER union_svr OPTIONS (table_name 'source_a');
CREATE FOREIGN TABLE union_pushdown_test.source_a_collapsing (
    message_id integer NOT NULL,
    category_id integer NOT NULL,
    sign smallint NOT NULL
) SERVER union_svr OPTIONS (
    table_name 'collapsing_source',
    engine 'CollapsingMergeTree(sign)'
);
CREATE FOREIGN TABLE union_pushdown_test.source_b (
    message_id integer NOT NULL,
    category_id integer NOT NULL
) SERVER union_svr OPTIONS (table_name 'source_b');
CREATE FOREIGN TABLE union_pushdown_test.source_b_other (
    message_id integer NOT NULL,
    category_id integer NOT NULL
) SERVER union_other_svr OPTIONS (table_name 'source_b');
CREATE FOREIGN TABLE union_pushdown_test.bpchar_values (
    value character(4) NOT NULL,
    arm integer NOT NULL
) SERVER union_svr OPTIONS (table_name 'bpchar_values');
CREATE FOREIGN TABLE union_pushdown_test.source_a_http (
    message_id integer NOT NULL,
    category_id integer NOT NULL
) SERVER union_http_svr OPTIONS (table_name 'source_a');
CREATE FOREIGN TABLE union_pushdown_test.source_b_http (
    message_id integer NOT NULL,
    category_id integer NOT NULL
) SERVER union_http_svr OPTIONS (table_name 'source_b');

SET SESSION search_path = union_pushdown_test,public;
SET SESSION enable_hashjoin = false;
SET SESSION enable_mergejoin = false;

\unset ECHO
CREATE FUNCTION pg_temp.assert_union_plan(
    test_name text,
    query_sql text,
    expected_full_pushdown boolean,
    expected_remote_fragment text
) RETURNS void
LANGUAGE plpgsql
AS $function$
DECLARE
    plan jsonb;
    remote_sqls jsonb;
    fully_pushed_down boolean;
BEGIN
    EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF, FORMAT JSON) ' || query_sql
        INTO plan;
    remote_sqls :=
        jsonb_path_query_array(plan, 'strict $.**."Remote SQL"');
    fully_pushed_down :=
        COALESCE(plan #>> '{0,Plan,Node Type}' = 'Foreign Scan', false)
        AND jsonb_array_length(remote_sqls) = 1;

    IF fully_pushed_down AND expected_remote_fragment IS NOT NULL THEN
        fully_pushed_down :=
            position(expected_remote_fragment IN (remote_sqls ->> 0)) > 0;
    END IF;

    RAISE NOTICE '%: %', test_name,
        fully_pushed_down IS NOT DISTINCT FROM expected_full_pushdown;
END;
$function$;

DO $do$
BEGIN
    PERFORM pg_temp.assert_union_plan(
        'same-server UNION ALL fully pushed down',
        $query$
            SELECT category_id FROM source_a WHERE message_id <= 2
            UNION ALL
            SELECT category_id FROM source_b WHERE message_id <= 2
        $query$,
        true,
        'UNION ALL'
    );

    PERFORM pg_temp.assert_union_plan(
        'plain integer DISTINCT fully pushed down',
        $query$
            SELECT DISTINCT message_id
            FROM messages
            WHERE is_selected = 1
        $query$,
        true,
        'SELECT DISTINCT'
    );

    PERFORM pg_temp.assert_union_plan(
        'UNION ALL of DISTINCT arms fully pushed down',
        $query$
            SELECT DISTINCT message_id
            FROM source_a
            WHERE category_id <= 20
            UNION ALL
            SELECT DISTINCT message_id
            FROM source_b
            WHERE category_id <= 20
        $query$,
        true,
        'UNION ALL'
    );

    PERFORM pg_temp.assert_union_plan(
        'ordered plain DISTINCT not fully pushed down',
        $query$
            SELECT DISTINCT message_id
            FROM messages
            WHERE is_selected = 1
            ORDER BY message_id
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'DISTINCT ON not fully pushed down',
        $query$
            SELECT DISTINCT ON (is_selected) is_selected, message_id
            FROM messages
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'bpchar plain DISTINCT not fully pushed down',
        $query$
            SELECT DISTINCT value FROM bpchar_values
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'non-default engine plain DISTINCT not fully pushed down',
        $query$
            SELECT DISTINCT message_id FROM source_a_collapsing
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'DISTINCT after window not fully pushed down',
        $query$
            SELECT DISTINCT row_number() OVER (ORDER BY message_id)
            FROM source_a
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'same-server UNION DISTINCT fully pushed down',
        $query$
            SELECT category_id FROM source_a WHERE message_id <= 2
            UNION
            SELECT category_id FROM source_b WHERE message_id <= 2
        $query$,
        true,
        'UNION DISTINCT'
    );

    PERFORM pg_temp.assert_union_plan(
        'HTTP same-server UNION ALL fully pushed down',
        $query$
            SELECT category_id FROM source_a_http WHERE message_id <= 2
            UNION ALL
            SELECT category_id FROM source_b_http WHERE message_id <= 2
        $query$,
        true,
        'UNION ALL'
    );

    PERFORM pg_temp.assert_union_plan(
        'ordered UNION DISTINCT not fully pushed down',
        $query$
            SELECT category_id FROM source_a WHERE message_id <= 2
            UNION
            SELECT category_id FROM source_b WHERE message_id <= 2
            ORDER BY category_id NULLS FIRST
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'default single-reference CTE fully pushed down',
        $query$
            WITH source_values AS (
                SELECT category_id FROM source_a WHERE message_id <= 2
            )
            SELECT category_id FROM source_values
            UNION ALL
            SELECT category_id FROM source_b WHERE message_id <= 2
        $query$,
        true,
        'UNION ALL'
    );

    PERFORM pg_temp.assert_union_plan(
        'NOT MATERIALIZED multi-reference CTE fully pushed down',
        $query$
            WITH selected_messages AS NOT MATERIALIZED (
                SELECT message_id FROM messages WHERE is_selected = 1
            )
            SELECT selected_messages.message_id, source_a.category_id
            FROM selected_messages
            JOIN source_a USING (message_id)
            UNION ALL
            SELECT selected_messages.message_id, source_b.category_id
            FROM selected_messages
            JOIN source_b USING (message_id)
        $query$,
        true,
        'UNION ALL'
    );

    PERFORM pg_temp.assert_union_plan(
        'GROUP BY over same-server UNION ALL fully pushed down',
        $query$
            WITH selected_messages AS NOT MATERIALIZED (
                SELECT message_id FROM messages WHERE is_selected = 1
            )
            SELECT category_id, count(DISTINCT message_id)
            FROM (
                SELECT selected_messages.message_id, source_a.category_id
                FROM selected_messages
                JOIN source_a USING (message_id)
                UNION ALL
                SELECT selected_messages.message_id, source_b.category_id
                FROM selected_messages
                JOIN source_b USING (message_id)
            ) AS combined_rows
            GROUP BY category_id
        $query$,
        true,
        'count(DISTINCT'
    );

    PERFORM pg_temp.assert_union_plan(
        'GROUP BY over filtered join UNION ALL fully pushed down',
        $query$
            SELECT category_id, count(DISTINCT message_id)
            FROM (
                SELECT messages.message_id, source_a.category_id
                FROM messages
                JOIN source_a USING (message_id)
                WHERE messages.is_selected = 1
                  AND source_a.category_id <= 40
                UNION ALL
                SELECT messages.message_id, source_b.category_id
                FROM messages
                JOIN source_b USING (message_id)
                WHERE messages.is_selected = 1
                  AND source_b.category_id <= 40
            ) AS combined_rows
            GROUP BY 1
        $query$,
        true,
        'count(DISTINCT'
    );

    PERFORM pg_temp.assert_union_plan(
        'GROUP BY pruned second UNION column fully pushed down',
        $query$
            SELECT category_id, count(*)
            FROM (
                SELECT message_id, category_id FROM source_a
                UNION ALL
                SELECT message_id, category_id FROM source_b
            ) AS combined_rows
            GROUP BY category_id
        $query$,
        true,
        'GROUP BY'
    );

    PERFORM pg_temp.assert_union_plan(
        'three-arm reordered UNION ALL fully pushed down',
        $query$
            SELECT category_id, message_id FROM source_a
            UNION ALL
            SELECT category_id, message_id FROM source_b
            UNION ALL
            SELECT category_id, message_id FROM source_a
        $query$,
        true,
        'UNION ALL'
    );

    PERFORM pg_temp.assert_union_plan(
        'default multi-reference CTE not fully pushed down',
        $query$
            WITH selected_messages AS (
                SELECT message_id FROM messages WHERE is_selected = 1
            )
            SELECT selected_messages.message_id, source_a.category_id
            FROM selected_messages
            JOIN source_a USING (message_id)
            UNION ALL
            SELECT selected_messages.message_id, source_b.category_id
            FROM selected_messages
            JOIN source_b USING (message_id)
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'MATERIALIZED multi-reference CTE not fully pushed down',
        $query$
            WITH selected_messages AS MATERIALIZED (
                SELECT message_id FROM messages WHERE is_selected = 1
            )
            SELECT selected_messages.message_id, source_a.category_id
            FROM selected_messages
            JOIN source_a USING (message_id)
            UNION ALL
            SELECT selected_messages.message_id, source_b.category_id
            FROM selected_messages
            JOIN source_b USING (message_id)
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'different-server UNION ALL not fully pushed down',
        $query$
            SELECT category_id FROM source_a WHERE message_id <= 2
            UNION ALL
            SELECT category_id FROM source_b_other WHERE message_id <= 2
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'uncorrelated InitPlans not fully pushed down',
        $query$
            SELECT category_id FROM source_a
            WHERE category_id > (SELECT 1)
            UNION ALL
            SELECT category_id FROM source_b
            WHERE category_id > (SELECT 1)
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'zero-width UNION aggregate not fully pushed down',
        $query$
            SELECT count(*)
            FROM (
                SELECT message_id FROM source_a
                UNION ALL
                SELECT message_id FROM source_b
            ) AS message_ids
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'bpchar GROUP BY over UNION ALL not fully pushed down',
        $query$
            SELECT value, count(*)
            FROM (
                SELECT value FROM bpchar_values WHERE arm = 1
                UNION ALL
                SELECT value FROM bpchar_values WHERE arm = 2
            ) AS values
            GROUP BY value
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'bpchar aggregate DISTINCT over UNION ALL not fully pushed down',
        $query$
            SELECT arm, count(DISTINCT value)
            FROM (
                SELECT arm, value FROM bpchar_values WHERE arm = 1
                UNION ALL
                SELECT arm, value FROM bpchar_values WHERE arm = 2
            ) AS values
            GROUP BY arm
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'non-default engine GROUP BY over UNION ALL not fully pushed down',
        $query$
            SELECT category_id, count(*)
            FROM (
                SELECT category_id FROM source_a_collapsing
                UNION ALL
                SELECT category_id FROM source_b
            ) AS combined_rows
            GROUP BY category_id
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'HAVING over UNION ALL not fully pushed down',
        $query$
            SELECT category_id, count(*)
            FROM (
                SELECT category_id FROM source_a
                UNION ALL
                SELECT category_id FROM source_b
            ) AS combined_rows
            GROUP BY category_id
            HAVING count(*) > 1
        $query$,
        false,
        NULL
    );

    PERFORM pg_temp.assert_union_plan(
        'bpchar UNION DISTINCT not fully pushed down',
        $query$
            SELECT value FROM bpchar_values WHERE arm = 1
            UNION
            SELECT value FROM bpchar_values WHERE arm = 2
        $query$,
        false,
        NULL
    );
END;
$do$;
\set ECHO all

-- Materialize each positive set operation before the local ORDER BY so these
-- result checks execute the remote UNION while keeping output deterministic.
WITH pushed AS MATERIALIZED (
    SELECT DISTINCT message_id
    FROM messages
    WHERE is_selected = 1
)
SELECT message_id FROM pushed
ORDER BY message_id;

-- UNION ALL preserves duplicates.
WITH pushed AS MATERIALIZED (
    SELECT category_id FROM source_a WHERE message_id <= 2
    UNION ALL
    SELECT category_id FROM source_b WHERE message_id <= 2
)
SELECT category_id FROM pushed
ORDER BY category_id;

-- Distinct generic-plan parameters verify parameter renumbering across arms.
SET plan_cache_mode = force_generic_plan;
PREPARE union_param(integer, integer) AS
WITH pushed AS MATERIALIZED (
    SELECT category_id FROM source_a WHERE message_id <= $1
    UNION ALL
    SELECT category_id FROM source_b WHERE message_id <= $2
)
SELECT category_id FROM pushed
ORDER BY category_id;
EXECUTE union_param(1, 2);
DEALLOCATE union_param;

PREPARE grouped_union_param(integer, integer, integer) AS
WITH pushed AS MATERIALIZED (
    SELECT
        category_id,
        count(DISTINCT message_id) FILTER (WHERE category_id <= $3) AS message_count
    FROM (
        SELECT message_id, category_id FROM source_a WHERE message_id <= $1
        UNION ALL
        SELECT message_id, category_id FROM source_b WHERE message_id <= $2
    ) AS combined_rows
    GROUP BY category_id
)
SELECT category_id, message_count FROM pushed
ORDER BY category_id;
EXECUTE grouped_union_param(1, 2, 20);
DEALLOCATE grouped_union_param;

PREPARE union_http_param(integer, integer) AS
WITH pushed AS MATERIALIZED (
    SELECT category_id FROM source_a_http WHERE message_id <= $1
    UNION ALL
    SELECT category_id FROM source_b_http WHERE message_id <= $2
)
SELECT category_id FROM pushed
ORDER BY category_id;
EXECUTE union_http_param(1, 2);
DEALLOCATE union_http_param;
RESET plan_cache_mode;

-- PostgreSQL's bare UNION means UNION DISTINCT.
WITH pushed AS MATERIALIZED (
    SELECT category_id FROM source_a WHERE message_id <= 2
    UNION
    SELECT category_id FROM source_b WHERE message_id <= 2
)
SELECT category_id FROM pushed
ORDER BY category_id;

-- Direct ORDER BY keeps DISTINCT local; NULLS FIRST exercises a pathkey that
-- the remote set-operation target cannot currently preserve.
SELECT category_id FROM source_a WHERE message_id <= 2
UNION
SELECT category_id FROM source_b WHERE message_id <= 2
ORDER BY category_id NULLS FIRST;

-- A side-effect-free default CTE referenced once is inlined normally.
WITH pushed AS MATERIALIZED (
    WITH source_values AS (
        SELECT category_id FROM source_a WHERE message_id <= 2
    )
    SELECT category_id FROM source_values
    UNION ALL
    SELECT category_id FROM source_b WHERE message_id <= 2
)
SELECT category_id FROM pushed
ORDER BY category_id;

-- NOT MATERIALIZED permits PostgreSQL to inline both references before the
-- same-server joins and UNION are considered for pushdown.
WITH pushed AS MATERIALIZED (
    WITH selected_messages AS NOT MATERIALIZED (
        SELECT message_id FROM messages WHERE is_selected = 1
    )
    SELECT selected_messages.message_id, source_a.category_id
    FROM selected_messages
    JOIN source_a USING (message_id)
    UNION ALL
    SELECT selected_messages.message_id, source_b.category_id
    FROM selected_messages
    JOIN source_b USING (message_id)
)
SELECT message_id, category_id FROM pushed
ORDER BY message_id, category_id;

-- Default multi-reference and explicit MATERIALIZED CTEs retain PostgreSQL's
-- evaluate-once semantics and therefore stay local.
SELECT message_id, category_id
FROM (
    WITH selected_messages AS (
        SELECT message_id FROM messages WHERE is_selected = 1
    )
    SELECT selected_messages.message_id, source_a.category_id
    FROM selected_messages
    JOIN source_a USING (message_id)
    UNION ALL
    SELECT selected_messages.message_id, source_b.category_id
    FROM selected_messages
    JOIN source_b USING (message_id)
) AS pushed
ORDER BY message_id, category_id;

SELECT message_id, category_id
FROM (
    WITH selected_messages AS MATERIALIZED (
        SELECT message_id FROM messages WHERE is_selected = 1
    )
    SELECT selected_messages.message_id, source_a.category_id
    FROM selected_messages
    JOIN source_a USING (message_id)
    UNION ALL
    SELECT selected_messages.message_id, source_b.category_id
    FROM selected_messages
    JOIN source_b USING (message_id)
) AS pushed
ORDER BY message_id, category_id;

-- Matching connection options do not make distinct foreign servers safe to
-- combine into one remote query.
SELECT category_id
FROM (
    SELECT category_id FROM source_a WHERE message_id <= 2
    UNION ALL
    SELECT category_id FROM source_b_other WHERE message_id <= 2
) AS pushed
ORDER BY category_id;

-- InitPlans in UNION arms stay local and continue to execute normally.
WITH pushed AS MATERIALIZED (
    SELECT category_id FROM source_a
    WHERE category_id > (SELECT 1)
    UNION ALL
    SELECT category_id FROM source_b
    WHERE category_id > (SELECT 1)
)
SELECT category_id FROM pushed
ORDER BY category_id;

-- PostgreSQL bpchar equality ignores trailing spaces; keep DISTINCT local.
SELECT
    count(*) AS distinct_rows,
    bool_and(value = 'x'::character(4)) AS trailing_space_equal
FROM (
    SELECT value FROM bpchar_values WHERE arm = 1
    UNION
    SELECT value FROM bpchar_values WHERE arm = 2
) AS bpchar_union;

-- GROUP BY and COUNT(DISTINCT) execute after the remote UNION ALL. Duplicate
-- rows within and across arms must not inflate the per-category message count.
SELECT clickhouse_raw_query($$
    INSERT INTO union_pushdown_test.source_a VALUES
    (1, 10), (4, 40)
$$);

WITH grouped AS MATERIALIZED (
    WITH selected_messages AS NOT MATERIALIZED (
        SELECT message_id FROM messages WHERE is_selected = 1
    )
    SELECT category_id, count(DISTINCT message_id) AS message_count
    FROM (
        SELECT selected_messages.message_id, source_a.category_id
        FROM selected_messages
        JOIN source_a USING (message_id)
        UNION ALL
        SELECT selected_messages.message_id, source_b.category_id
        FROM selected_messages
        JOIN source_b USING (message_id)
    ) AS combined_rows
    GROUP BY category_id
)
SELECT category_id, message_count
FROM grouped
ORDER BY category_id;

-- Pruning message_id leaves a compact one-column remote UNION target whose
-- remaining value originated as the subquery's second output column.
WITH grouped AS MATERIALIZED (
    SELECT category_id, count(*) AS row_count
    FROM (
        SELECT message_id, category_id FROM source_a
        UNION ALL
        SELECT message_id, category_id FROM source_b
    ) AS combined_rows
    GROUP BY category_id
)
SELECT category_id, row_count
FROM grouped
ORDER BY category_id;

-- Direct SET ROLE plans use that role's mapping. A view instead plans its
-- foreign scans with the view owner's mapping, even for another current user.
CREATE ROLE union_view_owner;
GRANT USAGE, CREATE ON SCHEMA union_pushdown_test TO union_view_owner;
GRANT USAGE ON FOREIGN SERVER union_svr TO union_view_owner;
GRANT SELECT ON source_a, source_b TO union_view_owner;
CREATE USER MAPPING FOR union_view_owner SERVER union_svr;

CREATE VIEW owner_grouped_union AS
SELECT category_id, count(DISTINCT message_id) AS message_count
FROM (
    SELECT message_id, category_id FROM source_a
    UNION ALL
    SELECT message_id, category_id FROM source_b
) AS combined_rows
GROUP BY category_id;
ALTER VIEW owner_grouped_union OWNER TO union_view_owner;

DROP USER MAPPING FOR CURRENT_USER SERVER union_svr;
SET ROLE union_view_owner;
WITH grouped AS MATERIALIZED (
    SELECT category_id, count(DISTINCT message_id) AS message_count
    FROM (
        SELECT message_id, category_id FROM source_a
        UNION ALL
        SELECT message_id, category_id FROM source_b
    ) AS combined_rows
    GROUP BY category_id
)
SELECT count(*) AS categories, sum(message_count) AS total FROM grouped;
RESET ROLE;

SELECT count(*) AS categories, sum(message_count) AS total
FROM owner_grouped_union;
CREATE USER MAPPING FOR CURRENT_USER SERVER union_svr;

DROP VIEW owner_grouped_union;
DROP USER MAPPING FOR union_view_owner SERVER union_svr;
REVOKE SELECT ON source_a, source_b FROM union_view_owner;
REVOKE USAGE ON FOREIGN SERVER union_svr FROM union_view_owner;
REVOKE USAGE, CREATE ON SCHEMA union_pushdown_test FROM union_view_owner;
DROP ROLE union_view_owner;

RESET enable_hashjoin;
RESET enable_mergejoin;
SET SESSION search_path = public;

DROP FOREIGN TABLE union_pushdown_test.source_b_http;
DROP FOREIGN TABLE union_pushdown_test.source_a_http;
DROP FOREIGN TABLE union_pushdown_test.bpchar_values;
DROP FOREIGN TABLE union_pushdown_test.source_b_other;
DROP FOREIGN TABLE union_pushdown_test.source_b;
DROP FOREIGN TABLE union_pushdown_test.source_a_collapsing;
DROP FOREIGN TABLE union_pushdown_test.source_a;
DROP FOREIGN TABLE union_pushdown_test.messages;
DROP SCHEMA union_pushdown_test;
DROP USER MAPPING FOR CURRENT_USER SERVER union_other_svr;
DROP SERVER union_other_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER union_http_svr;
DROP SERVER union_http_svr;
DROP USER MAPPING FOR CURRENT_USER SERVER union_svr;
DROP SERVER union_svr;
SELECT clickhouse_raw_query('DROP DATABASE union_pushdown_test');

-- End of UNION pushdown tests.
