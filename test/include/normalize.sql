\set normalize_echo :ECHO
\set ECHO none
-- Helpers masking output that differs across Postgres and ClickHouse versions.
-- pg_regress feeds tests on stdin, so \i resolves from repository root

-- Run EXPLAIN statement, masking output that differs across Postgres versions:
-- PG19 "Replaces:" lines, estimated row widths, and rangetable index in remote
-- aliases (rN, qL_N), densely renumbered keeping order
CREATE FUNCTION pg_temp.explain_filter(stmt text) RETURNS TABLE ("QUERY PLAN" text)
LANGUAGE plpgsql AS $$
DECLARE
    lines text[];
    plan  text;
    n     int;
    i     int := 0;
BEGIN
    FOR plan IN EXECUTE stmt LOOP
        CONTINUE WHEN plan ~ '^\s*Replaces: ';
        lines := lines || regexp_replace(plan, '\mwidth=\d+', 'width=N', 'g');
    END LOOP;
    plan := array_to_string(lines, E'\n');
    FOR n IN
        SELECT DISTINCT m[1]::int FROM regexp_matches(plan, '\m(?:r|q\d+_)(\d+)\M', 'g') m ORDER BY 1
    LOOP
        i := i + 1;
        plan := regexp_replace(plan, '\m(r|q\d+_)' || n || '\M', '\1' || chr(1) || i, 'g');
    END LOOP;
    RETURN QUERY SELECT unnest(string_to_array(replace(plan, chr(1), ''), E'\n'));
END
$$;

-- Remote SQL of query in plan order, for tests caring only about what gets
-- pushed down
CREATE FUNCTION pg_temp.remote_sql(query text) RETURNS SETOF text
LANGUAGE sql AS $$
    SELECT substring(plan FROM 'Remote SQL: (.*)')
      FROM pg_temp.explain_filter('EXPLAIN (VERBOSE, COSTS OFF) ' || query) AS p(plan)
     WHERE plan ~ '^\s*Remote SQL: '
$$;

-- Run statement expected to fail, printing error without ClickHouse wording
-- drift: query echoed after "In scope" or "While processing", "doesn't" in
-- 23.3, and trailing period before 26. Masks curl elapsed time, drops
-- PL/pgSQL and SPI context
CREATE FUNCTION pg_temp.error_filter(stmt text) RETURNS TABLE (error text)
LANGUAGE plpgsql AS $$
DECLARE
    msg    text;
    detail text;
    ctx    text;
BEGIN
    EXECUTE stmt;
    error := 'no error';
    RETURN NEXT;
EXCEPTION WHEN OTHERS THEN
    GET STACKED DIAGNOSTICS msg = MESSAGE_TEXT, detail = PG_EXCEPTION_DETAIL, ctx = PG_EXCEPTION_CONTEXT;
    msg := regexp_replace(msg, ': (In scope|While processing) .*', '');
    msg := regexp_replace(msg, ' after \d+ ms', ' after N ms');
    msg := regexp_replace(replace(msg, 'doesn''t', 'does not'), '\.( \([A-Z_]+\))?$', '\1');
    error := 'ERROR:  ' || msg;
    RETURN NEXT;
    IF detail <> '' THEN
        error := 'DETAIL:  ' || detail;
        RETURN NEXT;
    END IF;
    ctx := regexp_replace(ctx, '(^|\n)(SQL statement|PL/pgSQL function) .*', '');
    IF ctx <> '' THEN
        error := 'CONTEXT:  ' || ctx;
        RETURN NEXT;
    END IF;
END
$$;
\set ECHO :normalize_echo
