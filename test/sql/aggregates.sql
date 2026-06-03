\unset ECHO
SET client_min_messages = notice;
SET datestyle = 'ISO';
SET session timezone = 'UTC';

-- Create servers for each engine.
CREATE SERVER agg_bin_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER agg_bin_svr;

CREATE SERVER agg_http_svr FOREIGN DATA WRAPPER clickhouse_fdw
    OPTIONS(dbname 'system', driver 'http');
CREATE USER MAPPING FOR CURRENT_USER SERVER agg_http_svr;

-- Create a ClickHouse table to query.
SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS agg_test');
SELECT clickhouse_raw_query('CREATE DATABASE agg_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE agg_test.hits (
        id          Int64               NOT NULL,
        uuid        UUID                NOT NULL,
        timestamp   DateTime64(6,'UTC') NOT NULL,
        datestamp   Date                NOT NULL,
        path        String              NOT NULL,
        duration    UInt32              NOT NULL,
        cost        Decimal(10, 2)      NOT NULL
    ) ENGINE = MergeTree ORDER BY (timestamp);
$$);

/* Queries used to generate the insert values below.

SELECT clickhouse_raw_query($$
    INSERT INTO agg_test.hits
    WITH gen AS (
        SELECT toDateTime64(concat('2025-12-19 10:42:00.', floor(randUniform(0, 999999))), 6, 'UTC') - (rand() % 86400 * 14) AS ts
        FROM numbers(50)
    )
    SELECT abs(rand()),
        generateUUIDv4(),
        ts,
        date(ts),
        ['/profile', '/users', '/users/1321945', '/users/283434', '/users/802683', '/users/1739238', '/users/7392323', '/widgets', '/search', '/search', '/search', '/widgets/omnis', '/widgets/natus', '/widgets/voluptatem', '/widgets/totam', '/widgets/aperiam'][1+rand()%16],
        round(randChiSquared(1) * 1000),
        cast(randChiSquared(1) * rand() %8+1 AS Decimal(10, 2))
    FROM gen;
$$);
SELECT clickhouse_raw_query('select * from agg_test.hits format values');

*/

-- Insert data.
SELECT clickhouse_raw_query($$
    INSERT INTO agg_test.hits VALUES
    (3498231651,'8cab5a51-8927-43f4-8104-b2c81ffc3d8a','2025-12-05 11:51:04.390884','2025-12-05','/users/283434',304,1.29),
    (1431223188,'691a0477-c3fa-404d-a478-d4caaaa24832','2025-12-05 12:22:20.135213','2025-12-05','/users/802683',541,8.38),
    (1331538905,'f44459f2-9bbd-4149-9ac8-69c7b22f1aa0','2025-12-05 14:31:50.452533','2025-12-05','/search',599,4.28),
    (3451631267,'148181e2-a573-481f-9e2a-02441d523034','2025-12-05 19:06:00.126014','2025-12-05','/users/283434',1829,6.51),
    (2757160710,'974f2e1d-6918-4cce-935c-d445428584cc','2025-12-05 19:07:10.399278','2025-12-05','/users/7392323',87,2.15),
    (4049681990,'720ad53d-db83-4091-a262-cfc7347f96ae','2025-12-05 20:07:22.673831','2025-12-05','/users/7392323',243,3.02),
    (3319320102,'b623608d-459d-43d2-90ac-d1a8de83ff17','2025-12-06 03:25:34.627161','2025-12-06','/users/7392323',615,5.94),
    (2178896781,'d205ee5d-dd47-4a63-be2f-198fcfb7403b','2025-12-06 05:50:56.818572','2025-12-06','/widgets/voluptatem',298,2.98),
    (2433222225,'1f1dd4dd-2b05-4876-b98c-cb5ba1f34b89','2025-12-06 13:02:50.972837','2025-12-06','/users',1409,5.6),
    (3948091185,'a77b5881-262a-47c4-8494-a211e48b9c6c','2025-12-07 15:42:06.881490','2025-12-07','/users',83,7.01),
    (3122655508,'aef92b53-0b2a-4777-8d9d-ce5a6443ef54','2025-12-07 17:28:58.124117','2025-12-07','/users/802683',1332,4.26),
    (3095867833,'99f9a13a-357a-40fd-b415-1966865468d3','2025-12-07 23:20:36.618385','2025-12-07','/search',2011,7.4),
    (2720322600,'5c740ee9-a7a2-43e4-8ab1-e8c147f78c8b','2025-12-08 01:59:58.417052','2025-12-08','/search',84,7.55),
    (2400403412,'d12fe0f1-9c4c-4039-9027-3e9f62d1da54','2025-12-08 03:00:24.806643','2025-12-08','/users/802683',3542,2.34),
    (3159416553,'7885ab07-3740-461f-ac0c-c8732c07d86e','2025-12-08 14:42:58.344220','2025-12-08','/search',234,7.57),
    (4160213552,'031a4434-e4f8-465d-8b13-7537ad578e93','2025-12-08 20:14:32.236350','2025-12-08','/profile',48,5.66),
    (3910581405,'1c0f3511-3f88-4d4a-a053-724449619420','2025-12-09 00:08:20.392948','2025-12-09','/widgets/voluptatem',2498,2.61),
    (3267604082,'d60a7c1d-dccf-427b-b685-9a6077a88867','2025-12-10 00:39:44.518284','2025-12-10','/users/1321945',1784,3.51),
    (3841934465,'c066fdd6-0172-46aa-9792-63455cb069b4','2025-12-10 00:49:32.950914','2025-12-10','/users',9,3.52),
    (4264738908,'9f420aa6-f015-4caa-a2f8-2ca0e7a652a6','2025-12-10 04:57:48.584719','2025-12-10','/widgets/natus',2556,4.43),
    (2005666276,'b8615e95-f95f-458e-aa87-b6e6634503d9','2025-12-10 09:29:52.666577','2025-12-10','/users/802683',772,5.55),
    (3824162205,'06fa959e-afe3-414d-9771-650547091079','2025-12-10 13:40:28.486178','2025-12-10','/widgets/voluptatem',253,5.24),
    (352088655,'f5468669-4391-4ed7-99be-82061f8f3f93','2025-12-10 16:24:16.877779','2025-12-10','/widgets/aperiam',64,5.1),
    (467263982,'77af0ea8-2fef-4a21-bee0-335f9aa995c6','2025-12-12 09:52:04.236867','2025-12-12','/widgets/totam',1056,1.99),
    (2890629064,'368679e8-8f2a-47dd-8bce-b491bdd09720','2025-12-12 20:46:06.572602','2025-12-12','/search',14,1.81),
    (4072815052,'90b90c80-8f8e-44e1-9468-e4d2cddd22bb','2025-12-12 23:50:54.686406','2025-12-12','/widgets/natus',148,6.8),
    (1008605146,'6da3afd4-4fa3-49b7-ab29-ffe1b2bb7082','2025-12-13 06:58:08.868876','2025-12-13','/search',963,3.54),
    (837437371,'b5628648-e429-4fb9-9d77-de539d32f29f','2025-12-13 10:27:26.390655','2025-12-13','/widgets/omnis',226,8.14),
    (3517179953,'3e4b5d8b-7b09-4d1c-9eb4-855f0ae108d2','2025-12-13 14:15:10.219085','2025-12-13','/users',2125,8.21),
    (2836779925,'5e57a6fe-0196-4100-865a-7d6dc55765c2','2025-12-13 14:54:36.174390','2025-12-13','/users/1739238',3839,4.55),
    (1470995555,'0d8c0e39-679f-4034-b48f-aa1595b02dfc','2025-12-13 17:08:04.502729','2025-12-13','/users/283434',4437,1.33),
    (3849384968,'533582ce-95eb-4fb6-804e-6256ea6adaac','2025-12-13 18:05:14.464410','2025-12-13','/search',2,7.42),
    (3334361077,'a04b2cbd-c5b9-4f65-8449-0827d6c7f712','2025-12-13 21:09:06.290341','2025-12-13','/users/1739238',2,2.23),
    (3258018379,'d5c34dc5-b5db-455d-9528-fe466bdadd2c','2025-12-14 11:21:28.869821','2025-12-14','/widgets/omnis',2092,5.24),
    (1750004811,'eda72c6e-3161-47dd-ba25-cf059bb995da','2025-12-14 17:21:16.478761','2025-12-14','/widgets/omnis',2478,6.04),
    (280308092,'68b5127e-9819-453f-812d-f1b90979f872','2025-12-14 22:32:04.449588','2025-12-14','/widgets/natus',15,8.53),
    (3231552675,'4886ebda-a378-4181-a291-c235ec1047a8','2025-12-15 07:35:16.557880','2025-12-15','/users/283434',1268,4.43),
    (392385086,'69229156-bb5a-450d-91a0-474f39a20bab','2025-12-15 16:41:30.608217','2025-12-15','/widgets/totam',66,3.57),
    (287518381,'1adf4190-5147-4e7e-b354-7715a5a81c40','2025-12-15 17:07:24.893024','2025-12-15','/widgets/voluptatem',7672,2.47),
    (1334989359,'52220ad9-02f0-4ad7-ad74-dc6add362a99','2025-12-15 18:34:26.171566','2025-12-15','/widgets/aperiam',54,1.2),
    (4093728327,'44cccdf8-d398-40c7-9e41-117a37e93b9e','2025-12-15 21:17:32.577173','2025-12-15','/widgets',3,3.55),
    (1406653594,'e636df93-8421-4926-bf6f-1d1ee36edd35','2025-12-16 09:13:10.107266','2025-12-16','/search',81,8.17),
    (366963527,'af62cc2f-e706-4d1d-a740-fd80c55a1250','2025-12-16 10:11:58.434566','2025-12-16','/widgets',916,8.24),
    (3432633864,'b50622a5-0e0a-4f66-8be1-84c7cc6851b6','2025-12-16 13:04:10.855999','2025-12-16','/search',4990,2.36),
    (104048445,'19b74725-9827-4d84-b4dc-b86788193002','2025-12-16 16:17:36.473802','2025-12-16','/widgets/voluptatem',501,4.79),
    (914876110,'2ddfb5b7-9ae5-428b-9351-48e823230672','2025-12-17 02:07:56.978358','2025-12-17','/widgets/totam',31,6.75),
    (1982637736,'969e2c53-c735-4649-a6a3-3107ed089cb0','2025-12-17 11:09:02.207676','2025-12-17','/search',165,8.9),
    (41607833,'ad89a796-f7c6-4549-bd2e-ced0e586a5a5','2025-12-18 16:36:18.437350','2025-12-18','/search',47,7.69),
    (3814959307,'70525fca-b870-4976-9377-fc959353302a','2025-12-18 22:57:20.264686','2025-12-18','/widgets/omnis',148,8.09),
    (1538392900,'7e3f2fe1-5312-4257-ba3e-54662ba14b56','2025-12-19 07:24:50.960198','2025-12-19','/users/802683',216,5.9)
$$);

CREATE SCHEMA agg_bin;
CREATE SCHEMA agg_http;
IMPORT FOREIGN SCHEMA "agg_test" FROM SERVER agg_bin_svr INTO agg_bin;
\d agg_bin.hits
IMPORT FOREIGN SCHEMA "agg_test" FROM SERVER agg_http_svr INTO agg_http;
\d agg_http.hits

\set id_limit 1000000000

-- avg()
\echo -- AVG(UInt64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(id) FROM agg_bin.hits;
SELECT avg(id) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(id) FROM agg_http.hits;
SELECT avg(id) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(id) FROM agg_bin.hits WHERE id < :id_limit;
SELECT avg(id) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(id) FROM agg_http.hits WHERE id < :id_limit;
SELECT avg(id) FROM agg_http.hits WHERE id < :id_limit;

\echo -- AVG(UInt32)
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(duration) FROM agg_bin.hits;
SELECT avg(duration) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(duration) FROM agg_http.hits;
SELECT avg(duration) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(duration) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT avg(duration) FROM agg_bin.hits WHERE duration < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(duration) FROM agg_http.hits WHERE duration < :id_limit;
SELECT avg(duration) FROM agg_http.hits WHERE duration < :id_limit;

\echo -- AVG(Decimal)
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(cost) FROM agg_bin.hits;
SELECT avg(cost) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(cost) FROM agg_http.hits;
SELECT avg(cost) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(cost) FROM agg_bin.hits WHERE cost < :id_limit;
SELECT avg(cost) FROM agg_bin.hits WHERE cost < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT avg(cost) FROM agg_http.hits WHERE cost < :id_limit;
SELECT avg(cost) FROM agg_http.hits WHERE cost < :id_limit;

-- array_agg()
\echo -- array_agg(UInt64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(id) FROM agg_bin.hits;
SELECT array_agg(id) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(id) FROM agg_http.hits;
SELECT array_agg(id) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(id) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT array_agg(id) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(id) FROM agg_http.hits WHERE duration < :id_limit;
SELECT array_agg(id) FROM agg_http.hits WHERE id < :id_limit;

\echo -- array_agg(UUID)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(uuid) FROM agg_bin.hits;
SELECT array_agg(uuid) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(uuid) FROM agg_http.hits;
SELECT array_agg(uuid) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(uuid) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT array_agg(uuid) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(uuid) FROM agg_http.hits WHERE duration < :id_limit;
SELECT array_agg(uuid) FROM agg_http.hits WHERE id < :id_limit;

\echo -- array_agg(DateTime64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(timestamp) FROM agg_bin.hits;
SELECT array_agg(timestamp) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(timestamp) FROM agg_http.hits;
SELECT array_agg(timestamp) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(timestamp) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT array_agg(timestamp) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(timestamp) FROM agg_http.hits WHERE duration < :id_limit;
SELECT array_agg(timestamp) FROM agg_http.hits WHERE id < :id_limit;

\echo -- array_agg(Date)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(datestamp) FROM agg_bin.hits;
SELECT array_agg(datestamp) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(datestamp) FROM agg_http.hits;
SELECT array_agg(datestamp) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(datestamp) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT array_agg(datestamp) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(datestamp) FROM agg_http.hits WHERE duration < :id_limit;
SELECT array_agg(datestamp) FROM agg_http.hits WHERE id < :id_limit;

\echo -- array_agg(String)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(path) FROM agg_bin.hits;
SELECT array_agg(path) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(path) FROM agg_http.hits;
SELECT array_agg(path) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(path) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT array_agg(path) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(path) FROM agg_http.hits WHERE duration < :id_limit;
SELECT array_agg(path) FROM agg_http.hits WHERE id < :id_limit;

\echo -- array_agg(UInt32)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(duration) FROM agg_bin.hits;
SELECT array_agg(duration) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(duration) FROM agg_http.hits;
SELECT array_agg(duration) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(duration) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT array_agg(duration) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(duration) FROM agg_http.hits WHERE duration < :id_limit;
SELECT array_agg(duration) FROM agg_http.hits WHERE id < :id_limit;

\echo -- array_agg(Decimal)
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(cost) FROM agg_bin.hits;
SELECT array_agg(cost) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(cost) FROM agg_http.hits;
SELECT array_agg(cost) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(cost) FROM agg_bin.hits WHERE cost < :id_limit;
SELECT array_agg(cost) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT array_agg(cost) FROM agg_http.hits WHERE cost < :id_limit;
SELECT array_agg(cost) FROM agg_http.hits WHERE id < :id_limit;

-- min()
\echo -- min(UInt64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(id) FROM agg_bin.hits;
SELECT min(id) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(id) FROM agg_http.hits;
SELECT min(id) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT min(id) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT min(id) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(id) FROM agg_http.hits WHERE duration < :id_limit;
SELECT min(id) FROM agg_http.hits WHERE id < :id_limit;

\echo -- min(DateTime64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(timestamp) FROM agg_bin.hits;
SELECT min(timestamp) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(timestamp) FROM agg_http.hits;
SELECT min(timestamp) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT min(timestamp) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT min(timestamp) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(timestamp) FROM agg_http.hits WHERE duration < :id_limit;
SELECT min(timestamp) FROM agg_http.hits WHERE id < :id_limit;

\echo -- min(Date)
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(datestamp) FROM agg_bin.hits;
SELECT min(datestamp) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(datestamp) FROM agg_http.hits;
SELECT min(datestamp) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT min(datestamp) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT min(datestamp) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(datestamp) FROM agg_http.hits WHERE duration < :id_limit;
SELECT min(datestamp) FROM agg_http.hits WHERE id < :id_limit;

\echo -- min(String)
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(path) FROM agg_bin.hits;
SELECT min(path) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(path) FROM agg_http.hits;
SELECT min(path) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT min(path) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT min(path) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(path) FROM agg_http.hits WHERE duration < :id_limit;
SELECT min(path) FROM agg_http.hits WHERE id < :id_limit;

\echo -- min(UInt32)
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(duration) FROM agg_bin.hits;
SELECT min(duration) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(duration) FROM agg_http.hits;
SELECT min(duration) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT min(duration) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT min(duration) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(duration) FROM agg_http.hits WHERE duration < :id_limit;
SELECT min(duration) FROM agg_http.hits WHERE id < :id_limit;

\echo -- min(Decimal)
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(cost) FROM agg_bin.hits;
SELECT min(cost) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(cost) FROM agg_http.hits;
SELECT min(cost) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT min(cost) FROM agg_bin.hits WHERE cost < :id_limit;
SELECT min(cost) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT min(cost) FROM agg_http.hits WHERE cost < :id_limit;
SELECT min(cost) FROM agg_http.hits WHERE id < :id_limit;

-- max()
\echo -- max(UInt64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(id) FROM agg_bin.hits;
SELECT max(id) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(id) FROM agg_http.hits;
SELECT max(id) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT max(id) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT max(id) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(id) FROM agg_http.hits WHERE duration < :id_limit;
SELECT max(id) FROM agg_http.hits WHERE id < :id_limit;

\echo -- max(DateTime64)
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(timestamp) FROM agg_bin.hits;
SELECT max(timestamp) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(timestamp) FROM agg_http.hits;
SELECT max(timestamp) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT max(timestamp) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT max(timestamp) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(timestamp) FROM agg_http.hits WHERE duration < :id_limit;
SELECT max(timestamp) FROM agg_http.hits WHERE id < :id_limit;

\echo -- max(Date)
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(datestamp) FROM agg_bin.hits;
SELECT max(datestamp) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(datestamp) FROM agg_http.hits;
SELECT max(datestamp) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT max(datestamp) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT max(datestamp) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(datestamp) FROM agg_http.hits WHERE duration < :id_limit;
SELECT max(datestamp) FROM agg_http.hits WHERE id < :id_limit;

\echo -- max(String)
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(path) FROM agg_bin.hits;
SELECT max(path) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(path) FROM agg_http.hits;
SELECT max(path) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT max(path) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT max(path) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(path) FROM agg_http.hits WHERE duration < :id_limit;
SELECT max(path) FROM agg_http.hits WHERE id < :id_limit;

\echo -- max(UInt32)
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(duration) FROM agg_bin.hits;
SELECT max(duration) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(duration) FROM agg_http.hits;
SELECT max(duration) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT max(duration) FROM agg_bin.hits WHERE duration < :id_limit;
SELECT max(duration) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(duration) FROM agg_http.hits WHERE duration < :id_limit;
SELECT max(duration) FROM agg_http.hits WHERE id < :id_limit;

\echo -- max(Decimal)
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(cost) FROM agg_bin.hits;
SELECT max(cost) FROM agg_bin.hits;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(cost) FROM agg_http.hits;
SELECT max(cost) FROM agg_http.hits;

EXPLAIN (VERBOSE, COSTS OFF) SELECT max(cost) FROM agg_bin.hits WHERE cost < :id_limit;
SELECT max(cost) FROM agg_bin.hits WHERE id < :id_limit;
EXPLAIN (VERBOSE, COSTS OFF) SELECT max(cost) FROM agg_http.hits WHERE cost < :id_limit;
SELECT max(cost) FROM agg_http.hits WHERE id < :id_limit;

-- string_agg() → groupConcat()
\echo -- string_agg pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
    SELECT string_agg(path, '|') FROM agg_bin.hits
    WHERE datestamp = '2025-12-05';
SELECT string_agg(path, '|') FROM agg_bin.hits
    WHERE datestamp = '2025-12-05';

\echo -- string_agg pushdown (http)
EXPLAIN (VERBOSE, COSTS OFF)
    SELECT string_agg(path, '') FROM agg_http.hits
    WHERE datestamp = '2025-12-05';
SELECT string_agg(path, '') FROM agg_http.hits
    WHERE datestamp = '2025-12-05';

\echo -- string_agg DISTINCT pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
    SELECT string_agg(DISTINCT path, ', ') FROM agg_bin.hits
    WHERE datestamp = '2025-12-05';

\echo -- string_agg with ORDER BY falls back to local execution
EXPLAIN (VERBOSE, COSTS OFF)
    SELECT string_agg(path, ', ' ORDER BY path) FROM agg_bin.hits
    WHERE datestamp = '2025-12-05';

\echo -- string_agg per-group pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
    SELECT datestamp, string_agg(path, ', ') FROM agg_bin.hits
    WHERE datestamp BETWEEN '2025-12-05' AND '2025-12-07'
    GROUP BY datestamp ORDER BY datestamp;
SELECT datestamp, string_agg(path, ', ') FROM agg_bin.hits
    WHERE datestamp BETWEEN '2025-12-05' AND '2025-12-07'
    GROUP BY datestamp ORDER BY datestamp;

-- bool_and / bool_or / every pushdown
\echo -- bool_and pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bool_and(duration > 0) FROM agg_bin.hits;

SELECT bool_and(duration > 0) FROM agg_bin.hits;

\echo -- bool_or pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bool_or(duration > 5000) FROM agg_bin.hits;

SELECT bool_or(duration > 5000) FROM agg_bin.hits;

\echo -- every pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT every(cost > 0) FROM agg_bin.hits;

SELECT every(cost > 0) FROM agg_bin.hits;

\echo -- bool_and pushdown (http)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bool_and(duration > 0) FROM agg_http.hits;

SELECT bool_and(duration > 0) FROM agg_http.hits;

\echo -- bool_or pushdown (http)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bool_or(duration > 5000) FROM agg_http.hits;

SELECT bool_or(duration > 5000) FROM agg_http.hits;

-- bit_and / bit_or pushdown (groupBitAnd / groupBitOr)
\echo -- bit_and pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bit_and(duration::int4) FROM agg_bin.hits WHERE id = 3498231651;

SELECT bit_and(duration::int4) FROM agg_bin.hits WHERE id = 3498231651;

\echo -- bit_or pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bit_or(duration::int4) FROM agg_bin.hits WHERE id = 3498231651;

SELECT bit_or(duration::int4) FROM agg_bin.hits WHERE id = 3498231651;

-- bit_xor added in PG14, pushes down as groupBitXor.
SELECT current_setting('server_version_num')::int >= 140000 AS pg14 \gset
\if :pg14
\echo -- bit_xor pushdown (binary)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT bit_xor(duration::int4) FROM agg_bin.hits WHERE id = 3498231651;

SELECT bit_xor(duration::int4) FROM agg_bin.hits WHERE id = 3498231651;
\endif

-- regr_* local fallback (not pushed)
\echo -- regr_slope local fallback
EXPLAIN (VERBOSE, COSTS OFF)
SELECT regr_slope(cost::float8, duration::float8) FROM agg_bin.hits;

-- Clean up.
DROP USER MAPPING FOR CURRENT_USER SERVER agg_bin_svr;
DROP SERVER agg_bin_svr CASCADE;
DROP USER MAPPING FOR CURRENT_USER SERVER agg_http_svr;
DROP SERVER agg_http_svr CASCADE;
