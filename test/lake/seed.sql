-- Iceberg warehouse for test/sql/import_lake.sql, written by clickhouse local
SET allow_experimental_insert_into_iceberg = 1, write_full_path_in_iceberg_metadata = 1;

CREATE TABLE events (
    id Int64,
    name Nullable(String),
    score Float64,
    day Date32,
    ts DateTime64(6),
    tags Array(String),
    attrs Map(String, Int64),
    point Tuple(x Float64, y Float64)
) ENGINE = IcebergLocal(concat({warehouse:String}, '/analytics/events'));

INSERT INTO events VALUES
    (1, 'click', 1.5, '2024-01-01', '2024-01-01 10:00:00.123456', ['a', 'b'], {'k': 1}, (1, 2)),
    (2, NULL, -0.5, '1969-12-31', '2024-01-02 00:00:00', [], {}, (0, 0)),
    (3, 'view', 0, '2024-02-29', '2024-02-29 23:59:59.999999', ['c'], {'k': 2, 'j': 3}, (-1, 0.5));

CREATE TABLE deep (
    id Int64,
    label String
) ENGINE = IcebergLocal(concat({warehouse:String}, '/analytics/sub/deep'));

INSERT INTO deep VALUES (1, 'one'), (3, 'three');

-- Names past PostgreSQL's 63 byte identifier limit
CREATE TABLE long (
    id Int64,
    column_with_a_name_longer_than_postgres_allows_for_identifiers_too String
) ENGINE = IcebergLocal(concat({warehouse:String}, '/analytics/sub/table_with_a_name_longer_than_postgres_allows_for_identifiers'));

INSERT INTO long VALUES (1, 'long');
