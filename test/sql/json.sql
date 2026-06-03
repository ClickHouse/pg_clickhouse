SET datestyle = 'ISO';
CREATE SERVER binary_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_test', driver 'binary');
CREATE SERVER http_json_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'json_test', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER binary_json_loopback;
CREATE USER MAPPING FOR CURRENT_USER SERVER http_json_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS json_test');
SELECT clickhouse_raw_query('CREATE DATABASE json_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE json_test.things (
        id   Int32 NOT NULL,
        data JSON NOT NULL
    ) ENGINE = MergeTree PARTITION BY id ORDER BY (id);
$$);

CREATE SCHEMA json_bin;
CREATE SCHEMA json_http;
IMPORT FOREIGN SCHEMA "json_test" FROM SERVER binary_json_loopback INTO json_bin;
\d json_bin.things
IMPORT FOREIGN SCHEMA "json_test" FROM SERVER http_json_loopback INTO json_http;
\d json_http.things

INSERT INTO json_bin.things VALUES
    (1, '{"id": 1, "name": "widget", "size": "large", "stocked": true}'),
    (2, '{"id": 2, "name": "sprocket", "size": "small", "stocked": true}')
;

INSERT INTO json_http.things VALUES
    (3, '{"id": 3, "name": "gizmo", "size": "medium", "stocked": true}'),
    (4, '{"id": 4, "name": "doodad", "size": "large", "stocked": false}')
;

SELECT * FROM json_bin.things ORDER BY id;
SELECT * FROM json_http.things ORDER BY id;

-- Should also support JSON mapping.
CREATE FOREIGN TABLE json_bin.json_things (
    id   integer NOT NULL,
    data json    NOT NULL
) SERVER binary_json_loopback OPTIONS (table_name 'things');

CREATE FOREIGN TABLE json_http.json_things (
    id   integer NOT NULL,
    data json    NOT NULL
) SERVER http_json_loopback OPTIONS (table_name 'things');

INSERT INTO json_bin.json_things
VALUES (5, '{"id": 5, "name": "bauble", "size": "small", "stocked": true}');

INSERT INTO json_http.json_things
VALUES (6, '{"id": 6, "name": "curio", "size": "medium", "stocked": false}');

SELECT * FROM json_bin.json_things ORDER BY id;
SELECT * FROM json_http.json_things ORDER BY id;

-- Subscript access on JSON columns must not be pushed down to ClickHouse.
-- ClickHouse JSON does not support the jsonb `column['key']` syntax (it
-- requires dot notation), so subscripts must be evaluated locally by
-- PostgreSQL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data['name'] FROM json_http.things;
SELECT data['name'] FROM json_http.things ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data['name'] FROM json_bin.things;
SELECT data['name'] FROM json_bin.things ORDER BY id;

-- DISTINCT forces an ORDER BY or HashAgg; the subscript must stay local.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT DISTINCT data['size'] FROM json_http.things;
SELECT DISTINCT data['size'] FROM json_http.things;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT DISTINCT data['size'] FROM json_bin.things;
SELECT DISTINCT data['size'] FROM json_bin.things;

-- GROUP BY with a JSON subscript expression.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data['size'], count(*) FROM json_http.things GROUP BY data['size'];
SELECT data['size'], count(*) FROM json_http.things GROUP BY data['size'];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data['size'], count(*) FROM json_bin.things GROUP BY data['size'];
SELECT data['size'], count(*) FROM json_bin.things GROUP BY data['size'];

-- The jsonb ->> operator runs locally when used in SELECT target lists.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data ->> 'name' FROM json_http.things;
SELECT data ->> 'name' FROM json_http.things ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data ->> 'name' FROM json_bin.things;
SELECT data ->> 'name' FROM json_bin.things ORDER BY id;

-- The json ->> operator runs locally when used in SELECT target lists.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data ->> 'name' FROM json_http.json_things;
SELECT data ->> 'name' FROM json_http.json_things ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data ->> 'name' FROM json_bin.json_things;
SELECT data ->> 'name' FROM json_bin.json_things ORDER BY id;

-- WHERE clause with jsonb ->> equality must be pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data ->> 'name' = 'widget';
SELECT * FROM json_http.things WHERE data ->> 'name' = 'widget';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things WHERE data ->> 'name' = 'widget';
SELECT * FROM json_bin.things WHERE data ->> 'name' = 'widget';

-- WHERE clause with json ->> equality must be pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things WHERE data ->> 'name' = 'widget';
SELECT * FROM json_http.json_things WHERE data ->> 'name' = 'widget';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_things WHERE data ->> 'name' = 'widget';
SELECT * FROM json_bin.json_things WHERE data ->> 'name' = 'widget';

-- WHERE clause with jsonb ->> and LIKE.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data ->> 'name' LIKE 'wid%';
SELECT * FROM json_http.things WHERE data ->> 'name' LIKE 'wid%';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things WHERE data ->> 'name' LIKE 'wid%';
SELECT * FROM json_bin.things WHERE data ->> 'name' LIKE 'wid%';

-- WHERE clause with json ->> and LIKE.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things WHERE data ->> 'name' LIKE 'wid%';
SELECT * FROM json_http.json_things WHERE data ->> 'name' LIKE 'wid%';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_things WHERE data ->> 'name' LIKE 'wid%';
SELECT * FROM json_bin.json_things WHERE data ->> 'name' LIKE 'wid%';

-- WHERE with multiple jsonb ->> conditions (AND).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';
SELECT * FROM json_http.things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';
SELECT * FROM json_bin.things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';

-- WHERE with multiple json ->> conditions (AND).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';
SELECT * FROM json_http.json_things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';
SELECT * FROM json_bin.json_things
  WHERE data ->> 'size' = 'large' AND data ->> 'stocked' = 'true';

-- WHERE with jsonb ->> in an OR condition.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;
SELECT * FROM json_http.things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;

SELECT * FROM json_bin.things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;
SELECT * FROM json_bin.things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;
SELECT * FROM json_http.json_things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;

SELECT * FROM json_bin.json_things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;
SELECT * FROM json_bin.json_things
  WHERE data ->> 'name' = 'widget' OR data ->> 'name' = 'gizmo'
  ORDER BY id;

-- ORDER BY with jsonb ->> pushdown.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things ORDER BY data ->> 'name';
SELECT * FROM json_http.things ORDER BY data ->> 'name';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things ORDER BY data ->> 'name';
SELECT * FROM json_bin.things ORDER BY data ->> 'name';

SET pg_clickhouse.session_settings TO 'allow_suspicious_types_in_order_by 1';
SELECT * FROM json_http.things ORDER BY data ->> 'name' LIMIT 2;
SELECT * FROM json_bin.things ORDER BY data ->> 'name' LIMIT 2;
SET pg_clickhouse.session_settings TO '';

-- ORDER BY with json ->> pushdown.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things ORDER BY data ->> 'name';
SELECT * FROM json_http.json_things ORDER BY data ->> 'name';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_things ORDER BY data ->> 'name';
SELECT * FROM json_bin.json_things ORDER BY data ->> 'name';

SET pg_clickhouse.session_settings TO 'allow_suspicious_types_in_order_by 1';
SELECT * FROM json_http.json_things ORDER BY data ->> 'name' LIMIT 2;
SELECT * FROM json_bin.json_things ORDER BY data ->> 'name' LIMIT 2;
SET pg_clickhouse.session_settings TO '';

-- The jsonb -> operator runs locally when used in SELECT target lists.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT data -> 'name' FROM json_http.things;
SELECT data -> 'name' FROM json_http.things ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data -> 'name' FROM json_bin.things;
SELECT data -> 'name' FROM json_bin.things ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data -> 'name' FROM json_http.json_things;
SELECT data -> 'name' FROM json_http.json_things ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT data -> 'name' FROM json_bin.json_things;
SELECT data -> 'name' FROM json_bin.json_things ORDER BY id;

-- WHERE clause with jsonb -> equality must be pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"';
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things WHERE data -> 'name' = '"widget"';
SELECT * FROM json_bin.things WHERE data -> 'name' = '"widget"';

-- WHERE clause with json -> cast to text must be pushed down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things WHERE (data -> 'name')::text = '"widget"';
SELECT * FROM json_http.json_things WHERE (data -> 'name')::text = '"widget"';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_things WHERE (data -> 'name')::text = '"widget"';
SELECT * FROM json_bin.json_things WHERE (data -> 'name')::text = '"widget"';

-- WHERE clause with jsonb -> JSON boolean literal must push down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data -> 'stocked' = 'true'::jsonb;
SELECT * FROM json_http.things WHERE data -> 'stocked' = 'true'::jsonb ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things WHERE data -> 'stocked' = 'true'::jsonb;
SELECT * FROM json_bin.things WHERE data -> 'stocked' = 'true'::jsonb ORDER BY id;

-- WHERE clause with json -> JSON boolean literal must push down.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_things WHERE (data -> 'stocked')::text = 'true';
SELECT * FROM json_http.json_things WHERE (data -> 'stocked')::text = 'true' ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_things WHERE (data -> 'stocked')::text = 'true';
SELECT * FROM json_bin.json_things WHERE (data -> 'stocked')::text = 'true' ORDER BY id;

-- WHERE clause with jsonb -> wraps the dot notation in toJSONString() so the
-- result is a proper JSON value (->, unlike ->>, returns jsonb).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"'::jsonb;
SELECT * FROM json_http.things WHERE data -> 'name' = '"widget"'::jsonb;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things WHERE data -> 'name' = '"widget"'::jsonb;
SELECT * FROM json_bin.things WHERE data -> 'name' = '"widget"'::jsonb;

-- WHERE clause with json -> wraps the dot notation in toJSONString() so the
-- result is a proper JSON value (->, unlike ->>, returns json).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.things WHERE (data -> 'name')::text = '"widget"';
SELECT * FROM json_http.things WHERE (data -> 'name')::text = '"widget"';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.things WHERE (data -> 'name')::text = '"widget"';
SELECT * FROM json_bin.things WHERE (data -> 'name')::text = '"widget"';

-- Edge cases: JSON keys that require identifier quoting.
SELECT clickhouse_raw_query($$
    CREATE TABLE json_test.special_keys (
        id   Int32 NOT NULL,
        data JSON NOT NULL
    ) ENGINE = MergeTree ORDER BY (id);
$$);

CREATE FOREIGN TABLE json_http.special_keys (id integer NOT NULL, data jsonb NOT NULL)
  SERVER http_json_loopback OPTIONS (database 'json_test', table_name 'special_keys');

CREATE FOREIGN TABLE json_bin.special_keys (id integer NOT NULL, data jsonb NOT NULL)
  SERVER binary_json_loopback OPTIONS (database 'json_test', table_name 'special_keys');

INSERT INTO json_http.special_keys
VALUES (1, '{"my field": "hello", "CamelCase": "world", "select": "reserved"}');

INSERT INTO json_bin.special_keys
VALUES (2, E'{"The \\"meaning\\" of life": 42, "back\\\\slash": "bs", "dotted.key": "dot", "it''s": "apos", "key/with!special@chars#": "special", "123numeric": "num"}');

CREATE FOREIGN TABLE json_http.json_special_keys (id integer NOT NULL, data json NOT NULL)
  SERVER http_json_loopback OPTIONS (database 'json_test', table_name 'special_keys');

CREATE FOREIGN TABLE json_bin.json_special_keys (id integer NOT NULL, data json NOT NULL)
  SERVER binary_json_loopback OPTIONS (database 'json_test', table_name 'special_keys');

-- Key with a space: must be quoted in the remote SQL.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'my field' = 'hello';
SELECT * FROM json_http.special_keys WHERE data ->> 'my field' = 'hello';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'my field' = 'hello';
SELECT * FROM json_bin.special_keys WHERE data ->> 'my field' = 'hello';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'my field' = 'hello';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'my field' = 'hello';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'my field' = 'hello';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'my field' = 'hello';

-- Key with mixed case.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'CamelCase' = 'world';
SELECT * FROM json_http.special_keys WHERE data ->> 'CamelCase' = 'world';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'CamelCase' = 'world';
SELECT * FROM json_bin.special_keys WHERE data ->> 'CamelCase' = 'world';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'CamelCase' = 'world';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'CamelCase' = 'world';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'CamelCase' = 'world';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'CamelCase' = 'world';

-- Key that is a SQL reserved word.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'select' = 'reserved';
SELECT * FROM json_http.special_keys WHERE data ->> 'select' = 'reserved';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'select' = 'reserved';
SELECT * FROM json_bin.special_keys WHERE data ->> 'select' = 'reserved';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'select' = 'reserved';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'select' = 'reserved';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'select' = 'reserved';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'select' = 'reserved';

-- Key containing embedded double quotes.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'The "meaning" of life' = '42';
SELECT * FROM json_http.special_keys WHERE data ->> 'The "meaning" of life' = '42';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'The "meaning" of life' = '42';
SELECT * FROM json_bin.special_keys WHERE data ->> 'The "meaning" of life' = '42';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'The "meaning" of life' = '42';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'The "meaning" of life' = '42';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'The "meaning" of life' = '42';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'The "meaning" of life' = '42';

-- Key containing a backslash.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'back\slash' = 'bs';
SELECT * FROM json_http.special_keys WHERE data ->> 'back\slash' = 'bs';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'back\slash' = 'bs';
SELECT * FROM json_bin.special_keys WHERE data ->> 'back\slash' = 'bs';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'back\slash' = 'bs';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'back\slash' = 'bs';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'back\slash' = 'bs';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'back\slash' = 'bs';

-- Key containing a dot (must not be confused with nested access).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'dotted.key' = 'dot';
SELECT * FROM json_http.special_keys WHERE data ->> 'dotted.key' = 'dot';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'dotted.key' = 'dot';
SELECT * FROM json_bin.special_keys WHERE data ->> 'dotted.key' = 'dot';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'dotted.key' = 'dot';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'dotted.key' = 'dot';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'dotted.key' = 'dot';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'dotted.key' = 'dot';

-- Key containing an apostrophe / single quote.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'it''s' = 'apos';
SELECT * FROM json_http.special_keys WHERE data ->> 'it''s' = 'apos';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'it''s' = 'apos';
SELECT * FROM json_bin.special_keys WHERE data ->> 'it''s' = 'apos';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'it''s' = 'apos';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'it''s' = 'apos';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'it''s' = 'apos';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'it''s' = 'apos';

-- Key with slashes, bangs, at-signs, etc.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> 'key/with!special@chars#' = 'special';
SELECT * FROM json_http.special_keys WHERE data ->> 'key/with!special@chars#' = 'special';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> 'key/with!special@chars#' = 'special';
SELECT * FROM json_bin.special_keys WHERE data ->> 'key/with!special@chars#' = 'special';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> 'key/with!special@chars#' = 'special';
SELECT * FROM json_http.json_special_keys WHERE data ->> 'key/with!special@chars#' = 'special';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'key/with!special@chars#' = 'special';
SELECT * FROM json_bin.json_special_keys WHERE data ->> 'key/with!special@chars#' = 'special';

-- Key that starts with a digit.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.special_keys WHERE data ->> '123numeric' = 'num';
SELECT * FROM json_http.special_keys WHERE data ->> '123numeric' = 'num';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.special_keys WHERE data ->> '123numeric' = 'num';
SELECT * FROM json_bin.special_keys WHERE data ->> '123numeric' = 'num';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_http.json_special_keys WHERE data ->> '123numeric' = 'num';
SELECT * FROM json_http.json_special_keys WHERE data ->> '123numeric' = 'num';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM json_bin.json_special_keys WHERE data ->> '123numeric' = 'num';
SELECT * FROM json_bin.json_special_keys WHERE data ->> '123numeric' = 'num';

-- =======================================================================
-- jsonb_extract_path_text / jsonb_extract_path pushdown
-- =======================================================================

-- Create a table with nested JSON for multi-level path tests.
SELECT clickhouse_raw_query($$
    CREATE TABLE json_test.events (
        id         UInt32,
        event_name String,
        props      JSON
    ) ENGINE = MergeTree ORDER BY (event_name, id);
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO json_test.events VALUES
        (1, 'order', '{"customerId": "C100", "address": {"city": "Paris", "zip": "75001"}}'),
        (2, 'order', '{"customerId": "C200", "address": {"city": "London", "zip": "SW1A"}}');
$$);

CREATE FOREIGN TABLE json_http.events (
    id         integer,
    event_name text,
    props      jsonb
) SERVER http_json_loopback OPTIONS (table_name 'events');

CREATE FOREIGN TABLE json_bin.events (
    id         integer,
    event_name text,
    props      jsonb
) SERVER binary_json_loopback OPTIONS (table_name 'events');

-- Target-list: jsonb_extract_path_text is evaluated locally (like -> / ->>).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT jsonb_extract_path_text(props, 'customerId') FROM json_http.events;
SELECT jsonb_extract_path_text(props, 'customerId') FROM json_http.events ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT jsonb_extract_path_text(props, 'customerId') FROM json_bin.events;
SELECT jsonb_extract_path_text(props, 'customerId') FROM json_bin.events ORDER BY id;

-- Target-list: multi-level path, still evaluated locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT jsonb_extract_path_text(props, 'address', 'city') FROM json_http.events;
SELECT jsonb_extract_path_text(props, 'address', 'city') FROM json_http.events ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT jsonb_extract_path_text(props, 'address', 'city') FROM json_bin.events;
SELECT jsonb_extract_path_text(props, 'address', 'city') FROM json_bin.events ORDER BY id;

-- Target-list: jsonb_extract_path (returns jsonb, not text), evaluated locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT jsonb_extract_path(props, 'address') FROM json_http.events;
SELECT jsonb_extract_path(props, 'address') FROM json_http.events;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT jsonb_extract_path(props, 'address') FROM json_bin.events;
SELECT jsonb_extract_path(props, 'address') FROM json_bin.events;

-- WHERE: single-level jsonb_extract_path_text pushes down as dot notation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_http.events WHERE jsonb_extract_path_text(props, 'customerId') = 'C100';
SELECT id FROM json_http.events WHERE jsonb_extract_path_text(props, 'customerId') = 'C100';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_bin.events WHERE jsonb_extract_path_text(props, 'customerId') = 'C100';
SELECT id FROM json_bin.events WHERE jsonb_extract_path_text(props, 'customerId') = 'C100';

-- WHERE: multi-level jsonb_extract_path_text pushes down as dot notation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_http.events WHERE jsonb_extract_path_text(props, 'address', 'city') = 'Paris';
SELECT id FROM json_http.events WHERE jsonb_extract_path_text(props, 'address', 'city') = 'Paris';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_bin.events WHERE jsonb_extract_path_text(props, 'address', 'city') = 'Paris';
SELECT id FROM json_bin.events WHERE jsonb_extract_path_text(props, 'address', 'city') = 'Paris';

-- WHERE: jsonb_extract_path pushes down with toJSONString wrapping.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_http.events WHERE jsonb_extract_path(props, 'address', 'city') = '"Paris"'::jsonb;
SELECT id FROM json_http.events WHERE jsonb_extract_path(props, 'address', 'city') = '"Paris"'::jsonb;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_bin.events WHERE jsonb_extract_path(props, 'address', 'city') = '"Paris"'::jsonb;
SELECT id FROM json_bin.events WHERE jsonb_extract_path(props, 'address', 'city') = '"Paris"'::jsonb;

-- =======================================================================
-- json_extract_path_text / json_extract_path pushdown
-- =======================================================================
CREATE FOREIGN TABLE json_http.json_events (
    id         integer,
    event_name text,
    props      json
) SERVER http_json_loopback OPTIONS (table_name 'events');

CREATE FOREIGN TABLE json_bin.json_events (
    id         integer,
    event_name text,
    props      json
) SERVER binary_json_loopback OPTIONS (table_name 'events');

-- Target-list: json_extract_path_text is evaluated locally (like -> / ->>).
EXPLAIN (VERBOSE, COSTS OFF)
SELECT json_extract_path_text(props, 'customerId') FROM json_http.json_events;
SELECT json_extract_path_text(props, 'customerId') FROM json_http.json_events ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT json_extract_path_text(props, 'customerId') FROM json_bin.json_events;
SELECT json_extract_path_text(props, 'customerId') FROM json_bin.json_events ORDER BY id;

-- Target-list: multi-level path, still evaluated locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT json_extract_path_text(props, 'address', 'city') FROM json_http.json_events;
SELECT json_extract_path_text(props, 'address', 'city') FROM json_http.json_events ORDER BY id;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT json_extract_path_text(props, 'address', 'city') FROM json_bin.json_events;
SELECT json_extract_path_text(props, 'address', 'city') FROM json_bin.json_events ORDER BY id;

-- Target-list: json_extract_path (returns json, not text), evaluated locally.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT json_extract_path(props, 'address') FROM json_http.json_events;
SELECT json_extract_path(props, 'address') FROM json_http.json_events;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT json_extract_path(props, 'address') FROM json_bin.json_events;
SELECT json_extract_path(props, 'address') FROM json_bin.json_events;

-- WHERE: single-level json_extract_path_text pushes down as dot notation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_http.json_events WHERE json_extract_path_text(props, 'customerId') = 'C100';
SELECT id FROM json_http.json_events WHERE json_extract_path_text(props, 'customerId') = 'C100';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_bin.json_events WHERE json_extract_path_text(props, 'customerId') = 'C100';
SELECT id FROM json_bin.json_events WHERE json_extract_path_text(props, 'customerId') = 'C100';

-- WHERE: multi-level json_extract_path_text pushes down as dot notation.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_http.json_events WHERE json_extract_path_text(props, 'address', 'city') = 'Paris';
SELECT id FROM json_http.json_events WHERE json_extract_path_text(props, 'address', 'city') = 'Paris';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_bin.json_events WHERE json_extract_path_text(props, 'address', 'city') = 'Paris';
SELECT id FROM json_bin.json_events WHERE json_extract_path_text(props, 'address', 'city') = 'Paris';

-- WHERE: json_extract_path pushes down with toJSONString wrapping.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_http.json_events WHERE json_extract_path(props, 'address', 'city')::text = '"Paris"';
SELECT id FROM json_http.json_events WHERE json_extract_path(props, 'address', 'city')::text = '"Paris"';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM json_bin.json_events WHERE json_extract_path(props, 'address', 'city')::text = '"Paris"';
SELECT id FROM json_bin.json_events WHERE json_extract_path(props, 'address', 'city')::text = '"Paris"';

SELECT clickhouse_raw_query('DROP DATABASE json_test');
DROP USER MAPPING FOR CURRENT_USER SERVER binary_json_loopback;
DROP USER MAPPING FOR CURRENT_USER SERVER http_json_loopback;
DROP SERVER binary_json_loopback CASCADE;
DROP SERVER http_json_loopback CASCADE;
