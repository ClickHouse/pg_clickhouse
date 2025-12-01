CREATE SERVER where_sub_loopback FOREIGN DATA WRAPPER clickhouse_fdw OPTIONS(dbname 'where_sub_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER where_sub_loopback;

SELECT clickhouse_raw_query('DROP DATABASE IF EXISTS where_sub_test');
SELECT clickhouse_raw_query('CREATE DATABASE where_sub_test');
SELECT clickhouse_raw_query($$
    CREATE TABLE where_sub_test.orders  (
        id     Int32,
        date   Date,
        class  String
    ) ENGINE = MergeTree ORDER BY (id);
$$);

SELECT clickhouse_raw_query($$
    CREATE TABLE where_sub_test.lines (
        order_id    Int32,
        num         Int32,
        created_at  Date,
        updated_at  Date
    ) ENGINE = MergeTree ORDER BY (order_id, num);
$$);

CREATE SCHEMA where_sub;
IMPORT FOREIGN SCHEMA "where_sub_test" FROM SERVER where_sub_loopback INTO where_sub;

-- \d where_sub.orders
-- \d where_sub.lines

EXPLAIN (VERBOSE, COSTS OFF)
SELECT class, COUNT(*) AS order_count
  FROM where_sub.orders
 WHERE date >= date '2025-07-01'
   AND date < date(date '2025-07-01' + interval '3month')
   AND EXISTS (
       SELECT * FROM where_sub.lines
        WHERE order_id = id AND created_at < updated_at
   )
 GROUP BY class
 ORDER BY class;

SELECT clickhouse_raw_query('DROP DATABASE where_sub_test');
DROP USER MAPPING FOR CURRENT_USER SERVER where_sub_loopback;
DROP SERVER where_sub_loopback CASCADE;
