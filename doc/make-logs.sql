/*
 * clickhouse client --queries-file doc/make-logs.sql
 *
 * psql:
 * CREATE SERVER ch_svr FOREIGN DATA WRAPPER clickhouse_fdw
 *     OPTIONS(dbname 'default', driver 'binary');
 * CREATE USER MAPPING FOR CURRENT_USER SERVER ch_svr;
 * CREATE SCHEMA docs;
 * IMPORT FOREIGN SCHEMA "default" FROM SERVER ch_svr INTO docs;
 */

CREATE TABLE logs (
    req_id    Int64                NOT NULL,
    start_at  DateTime64(6, 'UTC') NOT NULL,
    duration  IntervalMillisecond  NOT NULL,
    resource  Text                 NOT NULL,
    method    Enum8('GET' = 1, 'HEAD', 'POST', 'PUT', 'DELETE', 'CONNECT', 'OPTIONS', 'TRACE', 'PATCH', 'QUERY') NOT NULL,
    node_id   Int64                NOT NULL,
    response  Int32                NOT NULL
) ENGINE = MergeTree
  ORDER BY start_at;

CREATE TABLE nodes (
    node_id Int64 NOT NULL,
    name    Text  NOT NULL,
    region  Text  NOT NULL,
    arch    Text  NOT NULL,
    os      Text  NOT NULL
) ENGINE = MergeTree
  PRIMARY KEY node_id;

INSERT INTO nodes
VALUES (1, 'Weeping Somnambulist', 'us-east-1', 'amd64', 'Linux')
     , (2, 'Donager', 'us-east-2', 'amd64', 'Linux')
     , (3, 'Anubis', 'ca-central-1', 'arm64', 'macOS')
     , (4, 'Arbogast', 'ap-northeast-1', 'amd64', 'Windows')
     , (5, 'Barbapiccola', 'us-east-1', 'arm64', 'Linux')
     , (6, 'Rocinante', 'us-east-1', 'arm64', 'Linux')
     , (7, 'Giambattista', 'us-east-1', 'amd64', 'Linux')
     , (8, 'Nauvoo', 'us-east-1', 'arm64', 'Linux')
;

-- Change "14" "86400 * 14" in to cover a longer period of time.
-- Change "numbers(1000)" to change the number of records created.
INSERT INTO logs
SELECT rand(),
       toDateTime64(concat('2025-12-19 10:42:00.', floor(randUniform(0, 999999))), 6, 'UTC') - (rand() % 86400 * 14),
       floor(randChiSquared(192)),
       ['/profile', '/users', '/users/1321945', '/users/283434', '/users/802683', '/users/1739238', '/users/7392323', '/widgets', '/search', '/search', '/search', '/widgets/omnis', '/widgets/natus', '/widgets/voluptatem', '/widgets/totam', '/widgets/aperiam'][1+rand()%16],
       least(floor(randChiSquared(1)) + 1, 10),
       rand() %8+1,
       [200, 201, 204, 308, 400, 401, 403, 500][toInt32(1+ floor(randFisherF(1, 16)%8))]
FROM numbers(1000);
