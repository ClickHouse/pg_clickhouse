-- Import from a DataLakeCatalog database. .github/ubuntu/lake.sh seeds the
-- Iceberg warehouse from test/lake/seed.sql and serves it on port 8181.
SET datestyle = 'ISO';

CREATE SERVER lake_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER lake_admin;

SELECT clickhouse_server_version('lake_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (26, 3) AS no_ch263 \gset
\if :no_ch263
\echo 'SKIP: Seeding Iceberg tables requires ClickHouse 26.3 or higher'
DROP SERVER lake_admin CASCADE;
\quit
\endif

CALL clickhouse_perform('lake_admin', 'DROP DATABASE IF EXISTS lake_test');
SET pg_clickhouse.session_settings = 'allow_experimental_database_iceberg 1';
CALL clickhouse_perform('lake_admin', $$
    CREATE DATABASE lake_test
    ENGINE = DataLakeCatalog('http://127.0.0.1:8181/v1')
    SETTINGS catalog_type = 'rest', warehouse = 'lake', vended_credentials = 0
$$);
RESET pg_clickhouse.session_settings;

CREATE SERVER lake_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'lake_test', driver 'http');
CREATE SERVER lake_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'lake_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER lake_http;
CREATE USER MAPPING FOR CURRENT_USER SERVER lake_bin;
CREATE SCHEMA lake_http;
CREATE SCHEMA lake_bin;
CREATE SCHEMA lake_limit;
CREATE SCHEMA lake_except;

-- Names keep namespace dots, with no engine option
IMPORT FOREIGN SCHEMA lake_test FROM SERVER lake_http INTO lake_http;
\det+ lake_http.*
\d lake_http."analytics.events"
\d lake_http."analytics.sub.deep"

IMPORT FOREIGN SCHEMA lake_test FROM SERVER lake_bin INTO lake_bin;
\det+ lake_bin.*
\d lake_bin."analytics.events"

SELECT * FROM lake_http."analytics.events" ORDER BY id;
SELECT * FROM lake_bin."analytics.events" ORDER BY id;
SELECT * FROM lake_bin."analytics.sub.deep" ORDER BY id;

-- Long names truncate locally, options keep remote names
\d lake_bin."analytics.sub.table_with"*
SELECT * FROM lake_bin."analytics.sub.table_with_a_name_longer_than_postgres_allows_for_identifiers";

-- Scan and join between catalog tables push down
EXPLAIN (VERBOSE, COSTS OFF)
SELECT e.id, e.name, d.label
  FROM lake_bin."analytics.events" e
  JOIN lake_bin."analytics.sub.deep" d ON d.id = e.id
 WHERE e.score >= 0
 ORDER BY e.id;
SELECT e.id, e.name, d.label
  FROM lake_bin."analytics.events" e
  JOIN lake_bin."analytics.sub.deep" d ON d.id = e.id
 WHERE e.score >= 0
 ORDER BY e.id;

-- Unquoted name parses as schema-qualified and matches nothing
IMPORT FOREIGN SCHEMA lake_test LIMIT TO (analytics.events)
    FROM SERVER lake_bin INTO lake_limit;
\det lake_limit.*

-- LIMIT TO and EXCEPT match quoted names
IMPORT FOREIGN SCHEMA lake_test LIMIT TO ("analytics.events")
    FROM SERVER lake_bin INTO lake_limit;
\det lake_limit.*
IMPORT FOREIGN SCHEMA lake_test
    LIMIT TO ("analytics.sub.table_with_a_name_longer_than_postgres_allows_for_identifiers")
    FROM SERVER lake_bin INTO lake_limit;
\det lake_limit.*
IMPORT FOREIGN SCHEMA lake_test EXCEPT ("analytics.events")
    FROM SERVER lake_bin INTO lake_except;
\det lake_except.*

CALL clickhouse_perform('lake_admin', 'DROP DATABASE lake_test');
DROP SCHEMA lake_http, lake_bin, lake_limit, lake_except CASCADE;
DROP SERVER lake_http, lake_bin, lake_admin CASCADE;
