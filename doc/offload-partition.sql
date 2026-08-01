-- Offload contiguous set of local RANGE partitions to single ClickHouse table,
-- replacing them with one wide foreign-table partition.
--
-- The resulting partition spans union of old_parts' bounds, so it can only ever
-- match whole partitions. The remote table ch_table must already exist with
-- matching columns & hold this range only; it defaults to parent's name.
CREATE FUNCTION clickhouse_offload_range(
    parent      regclass,
    old_parts   regclass[],
    server      name,
    ch_table    text DEFAULT NULL,
    table_opts  text DEFAULT NULL,
    new_part    name DEFAULT NULL
) RETURNS bigint
LANGUAGE plpgsql AS $offload$
DECLARE
    partstrat  "char";
    partnatts  int;
    keyattnum  int;
    keycol     name;
    keytype    text;
    schemaname name;
    parentname name;
    coldefs    text;
    newrel     name := new_part;
    opts       text;
    p          regclass;
    bound      text;
    m          text[];
    rows_csv   text := '';
    from_value text;
    to_value   text;
    contiguous boolean;
    n          bigint;
    local_rows bigint := 0;
BEGIN
    -- Restrict to single-column RANGE partitioning, the time-series case
    SELECT pt.partstrat, pt.partnatts, pt.partattrs[0]
      INTO partstrat, partnatts, keyattnum
      FROM pg_catalog.pg_partitioned_table pt
     WHERE pt.partrelid = parent;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'pg_clickhouse: % is not a partitioned table', parent;
    END IF;
    IF partstrat <> 'r' OR partnatts <> 1 OR keyattnum = 0 THEN
        RAISE EXCEPTION
            'pg_clickhouse: clickhouse_offload_range supports single-column RANGE partitioning only';
    END IF;

    SELECT a.attname, pg_catalog.format_type(a.atttypid, a.atttypmod),
           n.nspname, c.relname
      INTO keycol, keytype, schemaname, parentname
      FROM pg_catalog.pg_class c
      JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
      JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = keyattnum
     WHERE c.oid = parent;

    IF ch_table IS NULL THEN
        ch_table := parentname;
    END IF;

    -- Lock sources, count them, and collect their bounds. Deriving the range
    -- from the catalog rules out splitting a partition or leaving a gap
    FOREACH p IN ARRAY old_parts LOOP
        IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_inherits
                        WHERE inhrelid = p AND inhparent = parent) THEN
            RAISE EXCEPTION 'pg_clickhouse: % is not a partition of %', p, parent;
        END IF;
        EXECUTE pg_catalog.format('LOCK TABLE %s IN SHARE MODE', p);
        EXECUTE pg_catalog.format('SELECT pg_catalog.count(*) FROM %s', p) INTO n;
        local_rows := local_rows + n;

        SELECT pg_catalog.pg_get_expr(c.relpartbound, c.oid) INTO bound
          FROM pg_catalog.pg_class c WHERE c.oid = p;
        m := pg_catalog.regexp_match(bound, '^FOR VALUES FROM \((.+?)\) TO \((.+)\)$');
        IF m IS NULL OR m[1] = 'MINVALUE' OR m[2] = 'MAXVALUE' THEN
            RAISE EXCEPTION 'pg_clickhouse: % has unsupported bound %', p, bound;
        END IF;
        rows_csv := rows_csv || CASE WHEN rows_csv = '' THEN '' ELSE ', ' END
                 || pg_catalog.format('(%s::%s, %s::%s)', m[1], keytype, m[2], keytype);
    END LOOP;

    -- Combined bound is min lower to max upper; require the pieces to tile it
    -- with no gap (overlap is impossible between partitions of one parent)
    EXECUTE pg_catalog.format($q$
        WITH b(lo, hi) AS (VALUES %s),
             o AS (SELECT lo, hi, pg_catalog.lead(lo) OVER (ORDER BY lo) AS nlo FROM b)
        SELECT pg_catalog.min(lo)::text, pg_catalog.max(hi)::text,
               coalesce(pg_catalog.bool_and(hi = nlo) FILTER (WHERE nlo IS NOT NULL), true)
          FROM o
    $q$, rows_csv) INTO from_value, to_value, contiguous;

    IF NOT contiguous THEN
        RAISE EXCEPTION
            'pg_clickhouse: old_parts bounds are not contiguous; they leave a gap';
    END IF;

    IF newrel IS NULL THEN
        newrel := pg_catalog.left(
            pg_catalog.regexp_replace(
                pg_catalog.format('%s_%s_%s', parentname, from_value, to_value),
                '[^a-zA-Z0-9]+', '_', 'g'),
            63);
    END IF;

    -- Stage the destination as a standalone foreign table so the copy lands
    -- before it becomes queryable through parent. Inline CHECK matches the
    -- partition bound so ATTACH skips its validation scan of the remote table.
    -- Columns are spelled out: CREATE FOREIGN TABLE rejects LIKE
    SELECT pg_catalog.string_agg(
               pg_catalog.format('%I %s%s', a.attname,
                      pg_catalog.format_type(a.atttypid, a.atttypmod),
                      CASE WHEN a.attnotnull THEN ' NOT NULL' ELSE '' END),
               ', ' ORDER BY a.attnum)
      INTO coldefs
      FROM pg_catalog.pg_attribute a
     WHERE a.attrelid = parent AND a.attnum > 0 AND NOT a.attisdropped;

    opts := pg_catalog.format('table_name %L', ch_table);
    IF table_opts IS NOT NULL AND table_opts <> '' THEN
        opts := opts || ', ' || table_opts;
    END IF;
    EXECUTE pg_catalog.format(
        'CREATE FOREIGN TABLE %I.%I (%s, CHECK (%I IS NOT NULL AND %I >= %L AND %I < %L)) SERVER %I OPTIONS (%s)',
        schemaname, newrel, coldefs,
        keycol, keycol, from_value, keycol, to_value, server, opts);

    FOREACH p IN ARRAY old_parts LOOP
        EXECUTE pg_catalog.format('INSERT INTO %I.%I SELECT * FROM %s',
                                  schemaname, newrel, p);
    END LOOP;

    -- Atomic cutover: drop locals, attach the foreign partition in their place
    FOREACH p IN ARRAY old_parts LOOP
        EXECUTE pg_catalog.format('DROP TABLE %s', p);
    END LOOP;
    EXECUTE pg_catalog.format(
        'ALTER TABLE %s ATTACH PARTITION %I.%I FOR VALUES FROM (%L) TO (%L)',
        parent, schemaname, newrel, from_value, to_value);

    RETURN local_rows;
END;
$offload$;

-- Mirrors parent's columns, mapping each PostgreSQL type to ClickHouse and
-- wrapping nullable columns in Nullable(); arrays become Array(element) and stay
-- bare since ClickHouse forbids Nullable arrays. Partition key stays non-Nullable
-- since MergeTree ORDER BY rejects Nullable keys. ch_table defaults to parent's
-- name and lands in server's database, same target clickhouse_offload_range
-- attaches. Connection is taken from server, honoring its driver. Returns the
-- executed DDL.
CREATE FUNCTION clickhouse_offload_create_table(
    parent    regclass,
    server    name,
    ch_table  text DEFAULT NULL,
    order_by  text DEFAULT NULL,
    engine    text DEFAULT 'MergeTree'
) RETURNS text
LANGUAGE plpgsql AS $create$
DECLARE
    partstrat  "char";
    partnatts  int;
    keyattnum  int;
    keycol     name;
    parentname name;
    coldefs    text;
    badcol     text;
    ddl        text;
BEGIN
    IF NOT EXISTS (SELECT 1 FROM pg_catalog.pg_foreign_server WHERE srvname = server) THEN
        RAISE EXCEPTION 'pg_clickhouse: server % does not exist', server;
    END IF;

    SELECT pt.partstrat, pt.partnatts, pt.partattrs[0]
      INTO partstrat, partnatts, keyattnum
      FROM pg_catalog.pg_partitioned_table pt
     WHERE pt.partrelid = parent;
    IF NOT FOUND THEN
        RAISE EXCEPTION 'pg_clickhouse: % is not a partitioned table', parent;
    END IF;
    IF partstrat <> 'r' OR partnatts <> 1 OR keyattnum = 0 THEN
        RAISE EXCEPTION
            'pg_clickhouse: clickhouse_offload_create_table supports single-column RANGE partitioning only';
    END IF;

    SELECT c.relname, a.attname
      INTO parentname, keycol
      FROM pg_catalog.pg_class c
      JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = keyattnum
     WHERE c.oid = parent;

    IF ch_table IS NULL THEN
        ch_table := parentname;
    END IF;
    IF order_by IS NULL THEN
        order_by := pg_catalog.quote_ident(keycol);
    END IF;

    -- Invert type map (see str_types_map in pglink.c). For array columns map the
    -- element type and wrap in Array(); et is the element type, NULL for scalars.
    -- chtype is the scalar/element ClickHouse type, NULL when unmapped
    SELECT pg_catalog.string_agg(
               pg_catalog.format('%s %s', pg_catalog.quote_ident(a.attname),
                   CASE
                       -- ClickHouse forbids Nullable(Array(...)); arrays stay bare
                       WHEN et.oid IS NOT NULL THEN pg_catalog.format('Array(%s)', chtype)
                       WHEN a.attnotnull OR a.attnum = keyattnum THEN chtype
                       ELSE pg_catalog.format('Nullable(%s)', chtype)
                   END),
               ', ' ORDER BY a.attnum) FILTER (WHERE chtype IS NOT NULL),
           pg_catalog.min(pg_catalog.format('%I %s', a.attname,
                   pg_catalog.format_type(a.atttypid, a.atttypmod)))
               FILTER (WHERE chtype IS NULL)
      INTO coldefs, badcol
      FROM pg_catalog.pg_attribute a
      JOIN pg_catalog.pg_type t ON t.oid = a.atttypid
      LEFT JOIN pg_catalog.pg_type et ON et.oid = t.typelem AND t.typcategory = 'A'
      CROSS JOIN LATERAL (SELECT CASE coalesce(et.typname, t.typname)
              WHEN 'bool'        THEN 'Bool'
              WHEN 'int2'        THEN 'Int16'
              WHEN 'int4'        THEN 'Int32'
              WHEN 'int8'        THEN 'Int64'
              WHEN 'float4'      THEN 'Float32'
              WHEN 'float8'      THEN 'Float64'
              WHEN 'numeric'     THEN CASE WHEN a.atttypmod = -1 THEN NULL
                  ELSE pg_catalog.format('Decimal(%s, %s)',
                           ((a.atttypmod - 4) >> 16) & 65535,
                           (a.atttypmod - 4) & 65535) END
              WHEN 'date'        THEN 'Date32'
              WHEN 'timestamp'   THEN 'DateTime64(6)'
              WHEN 'timestamptz' THEN $$DateTime64(6, 'UTC')$$
              WHEN 'uuid'        THEN 'UUID'
              WHEN 'text'        THEN 'String'
              WHEN 'varchar'     THEN 'String'
              WHEN 'bpchar'      THEN 'String'
              WHEN 'bytea'       THEN 'String'
              WHEN 'json'        THEN 'String'
              WHEN 'jsonb'       THEN 'String'
          END) AS m(chtype)
     WHERE a.attrelid = parent AND a.attnum > 0 AND NOT a.attisdropped;

    IF badcol IS NOT NULL THEN
        RAISE EXCEPTION 'pg_clickhouse: cannot map column % to a ClickHouse type', badcol;
    END IF;

    ddl := pg_catalog.format('CREATE TABLE %s (%s) ENGINE = %s ORDER BY %s',
                             ch_table, coldefs, engine, order_by);
    CALL clickhouse_perform(server, ddl);
    RETURN ddl;
END;
$create$;
