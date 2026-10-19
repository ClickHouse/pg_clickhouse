\i test/include/normalize.sql
-- Explicit endpoint options, empty dbname falls back to default
CREATE SERVER endpoint_bin FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'binary', host 'localhost', port '9000', secure 'false', dbname '');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_bin OPTIONS (user 'default', password '');
SELECT clickhouse_server_version('endpoint_bin') <> '0.0.0' AS connected;
CREATE SERVER endpoint_http FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'http', host 'localhost', port '8123', secure 'no', dbname '');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_http OPTIONS (user 'default', password '');
SELECT clickhouse_server_version('endpoint_http') <> '0.0.0' AS connected;

-- Unparsable port is ignored in favor of default
CREATE SERVER endpoint_badport FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'binary', port 'abc');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_badport;
SELECT clickhouse_server_version('endpoint_badport') <> '0.0.0' AS connected;

-- Nothing listens on port 1
CREATE SERVER endpoint_bin_refused FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'binary', port '1');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_bin_refused;
SELECT clickhouse_server_version('endpoint_bin_refused');
CREATE SERVER endpoint_http_refused FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'http', port '1');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_http_refused;
-- Version probe over HTTP reports unknown rather than failing
SELECT clickhouse_server_version('endpoint_http_refused');
CREATE FOREIGN TABLE endpoint_http_refused_one (dummy int)
    SERVER endpoint_http_refused OPTIONS (database 'system', table_name 'one');
SELECT * FROM pg_temp.error_filter($$SELECT * FROM endpoint_http_refused_one$$);
SELECT * FROM pg_temp.error_filter($$INSERT INTO endpoint_http_refused_one VALUES (1)$$);
SELECT * FROM pg_temp.error_filter($$CALL clickhouse_perform('endpoint_http_refused', 'SELECT 1')$$);

-- TLS against plaintext HTTP port fails handshake, once per minimum version
CREATE SERVER endpoint_bin_tls12 FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'binary', port '8123', secure 'on', min_tls_version 'TLSv1.2');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_bin_tls12;
SELECT clickhouse_server_version('endpoint_bin_tls12');
CREATE SERVER endpoint_bin_tls1 FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'binary', port '8123', secure 'on', min_tls_version 'TLSv1');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_bin_tls1;
SELECT clickhouse_server_version('endpoint_bin_tls1');
CREATE SERVER endpoint_http_tls13 FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'http', port '8123', secure 'on', min_tls_version 'TLSv1.3');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_http_tls13;
CREATE FOREIGN TABLE endpoint_http_tls13_one (dummy int)
    SERVER endpoint_http_tls13 OPTIONS (database 'system', table_name 'one');
SELECT * FROM pg_temp.error_filter($$SELECT * FROM endpoint_http_tls13_one$$);
CREATE SERVER endpoint_http_tls11 FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS (driver 'http', port '8123', secure 'on', min_tls_version 'TLSv1.1');
CREATE USER MAPPING FOR CURRENT_USER SERVER endpoint_http_tls11;
CREATE FOREIGN TABLE endpoint_http_tls11_one (dummy int)
    SERVER endpoint_http_tls11 OPTIONS (database 'system', table_name 'one');
SELECT * FROM pg_temp.error_filter($$SELECT * FROM endpoint_http_tls11_one$$);

DROP SERVER endpoint_bin, endpoint_http, endpoint_badport, endpoint_bin_refused,
    endpoint_http_refused, endpoint_bin_tls12, endpoint_bin_tls1,
    endpoint_http_tls13, endpoint_http_tls11 CASCADE;
