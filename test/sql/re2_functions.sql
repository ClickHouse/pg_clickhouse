SELECT NOT EXISTS(SELECT 1 FROM pg_available_extensions WHERE name = 're2') AS no_re2 \gset
\if :no_re2
\echo 'SKIP: re2 extension not available'
\quit
\endif

CREATE SERVER re2_svr FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 're2_test');
CREATE USER MAPPING FOR CURRENT_USER SERVER re2_svr;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS re2_test');
SELECT clickhouse_raw_query('CREATE DATABASE re2_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE re2_test.t1 (
        id   Int32,
        val  String
    ) ENGINE = MergeTree ORDER BY id
$$);
SELECT clickhouse_raw_query($$
    INSERT INTO re2_test.t1 VALUES
        (1, 'POSIX uses BRE and ERE'),
        (2, 're2 uses finite automata'),
        (3, 'PCRE supports backtracking')
$$);

CREATE SCHEMA re2_test;
IMPORT FOREIGN SCHEMA re2_test FROM SERVER re2_svr INTO re2_test;
SET search_path = re2_test, public;

CREATE EXTENSION re2;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2match(val, 're2');
SELECT * FROM t1 WHERE re2match(val, 're2');

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2extract(val, '(re2)') = 're2';
SELECT * FROM t1 WHERE re2extract(val, '(re2)') = 're2';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2extractall(val, '[A-Z]+') = ARRAY['POSIX','BRE','ERE'];
SELECT * FROM t1 WHERE re2extractall(val, '[A-Z]+') = ARRAY['POSIX','BRE','ERE'];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2regexpextract(val, '(re2)', 1) = 're2';
SELECT * FROM t1 WHERE re2regexpextract(val, '(re2)', 1) = 're2';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2extractgroups(val, '(POSIX) uses (BRE)') = ARRAY['POSIX','BRE'];
SELECT * FROM t1 WHERE re2extractgroups(val, '(POSIX) uses (BRE)') = ARRAY['POSIX','BRE'];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2replaceregexpone(val, 'POSIX', 're2') = 're2 uses BRE and ERE';
SELECT * FROM t1 WHERE re2replaceregexpone(val, 'POSIX', 're2') = 're2 uses BRE and ERE';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2replaceregexpall(val, ' ', '-') = 're2-uses-finite-automata';
SELECT * FROM t1 WHERE re2replaceregexpall(val, ' ', '-') = 're2-uses-finite-automata';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2countmatches(val, 'e') > 0;
SELECT * FROM t1 WHERE re2countmatches(val, 'e') > 0;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2countmatchescaseinsensitive(val, 'E') > 0;
SELECT * FROM t1 WHERE re2countmatchescaseinsensitive(val, 'E') > 0;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2multimatchany(val, 'POSIX','PCRE');
SELECT * FROM t1 WHERE re2multimatchany(val, 'POSIX','PCRE');
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2multimatchany(val, VARIADIC ARRAY['POSIX','PCRE']);
SELECT * FROM t1 WHERE re2multimatchany(val, VARIADIC ARRAY['POSIX','PCRE']);

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2multimatchanyindex(val, 'POSIX','PCRE') > 0;
SELECT * FROM t1 WHERE re2multimatchanyindex(val, 'POSIX','PCRE') > 0;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2multimatchanyindex(val, VARIADIC ARRAY['POSIX','PCRE']) > 0;
SELECT * FROM t1 WHERE re2multimatchanyindex(val, 'POSIX','PCRE') > 0;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2multimatchallindices(val, 'POSIX','PCRE') = ARRAY[1];
SELECT * FROM t1 WHERE re2multimatchallindices(val, 'POSIX','PCRE') = ARRAY[1];
EXPLAIN (VERBOSE, COSTS OFF)
SELECT * FROM t1 WHERE re2multimatchallindices(val, VARIADIC ARRAY['POSIX','PCRE']) = ARRAY[1];
SELECT * FROM t1 WHERE re2multimatchallindices(val, VARIADIC ARRAY['POSIX','PCRE']) = ARRAY[1];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE re2regexpquotemeta(val || '.') = val || '\.';
SELECT id FROM t1 WHERE re2regexpquotemeta(val || '.') = val || '\.';

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE re2splitbyregexp(' ', val) = ARRAY['re2','uses','finite','automata'];
SELECT id FROM t1 WHERE re2splitbyregexp(' ', val) = ARRAY['re2','uses','finite','automata'];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE re2splitbyregexp(' ', val, 2) = ARRAY['re2','uses'];
SELECT id FROM t1 WHERE re2splitbyregexp(' ', val, 2) = ARRAY['re2','uses'];

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE array_length(re2extractallgroupsvertical(val, '(\w+) (\w+)'), 1) > 0;
SELECT id FROM t1 WHERE array_length(re2extractallgroupsvertical(val, '(\w+) (\w+)'), 1) > 0;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id FROM t1 WHERE array_length(re2extractallgroupshorizontal(val, '(\w+) (\w+)'), 1) = 2;
SELECT id FROM t1 WHERE array_length(re2extractallgroupshorizontal(val, '(\w+) (\w+)'), 1) = 2;

DROP EXTENSION re2;
DROP USER MAPPING FOR CURRENT_USER SERVER re2_svr;
SELECT clickhouse_raw_query('DROP DATABASE re2_test');
DROP SERVER re2_svr CASCADE;
