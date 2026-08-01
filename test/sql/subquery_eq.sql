-- Test for TPC-H Q2 style subquery pushdown
-- TPC-H schema references:
-- ch: https://clickhouse.com/docs/getting-started/example-datasets/tpch#data-generation-and-import
-- pg: https://raw.githubusercontent.com/Vonng/pgtpc/refs/heads/master/tpch/ddl/schema.ddl
SET datestyle = 'ISO';

-- SubPlan pushdown is gated on ClickHouse 25.8+ (older analyzers reject the
-- correlated SQL we generate), so the whole test aborts on older servers
-- rather than carry version-specific expected output.
CREATE SERVER sub_eq_admin FOREIGN DATA WRAPPER clickhouse_fdw;
CREATE USER MAPPING FOR CURRENT_USER SERVER sub_eq_admin;

SELECT clickhouse_server_version('sub_eq_admin') AS ch_version \gset
SELECT (split_part(:'ch_version', '.', 1)::int,
        split_part(:'ch_version', '.', 2)::int) < (25, 8) AS no_ch258 \gset
\if :no_ch258
\echo 'SKIP: SubPlan pushdown requires ClickHouse 25.8 or higher'
\quit
\endif

CREATE SERVER sub_eq_svr FOREIGN DATA WRAPPER clickhouse_fdw
       OPTIONS(dbname 'sub_eq_test', driver 'binary');
CREATE USER MAPPING FOR CURRENT_USER SERVER sub_eq_svr;

CALL clickhouse_perform('sub_eq_admin', 'DROP DATABASE IF EXISTS sub_eq_test');
CALL clickhouse_perform('sub_eq_admin', 'CREATE DATABASE sub_eq_test');

CALL clickhouse_perform('sub_eq_admin', $$
    CREATE TABLE sub_eq_test.region (
        r_regionkey  Int32,
        r_name       String,
        r_comment    String)
    ENGINE = MergeTree ORDER BY (r_regionkey);
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    INSERT INTO sub_eq_test.region
    VALUES (0,'AFRICA','lar deposits.')
         , (1,'AMERICA','hs use ironic, even requests. s')
         , (2,'ASIA','ges. thinly even pinto beans ca')
         , (3,'EUROPE','ly final courts cajole furiously final excuse')
         , (4,'MIDDLE EAST','quickly special accounts cajole carefully blithely close requests.')
$$);

-- Create and load TPC-H Tables.
CALL clickhouse_perform('sub_eq_admin', $$
    CREATE TABLE sub_eq_test.nation (
        n_nationkey  Int32,
        n_name       String,
        n_regionkey  Int32,
        n_comment    String)
    ENGINE = MergeTree ORDER BY (n_nationkey);
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    INSERT INTO sub_eq_test.nation
    VALUES (6,'FRANCE',3,'ruefully final requests. regular, ironi')
         , (7,'GERMANY',3,'platelets.')
         , (19,'ROMANIA',3,'asymptotes are about the furious multipliers.')
         , (22,'RUSSIA',3,'requests against the platelets.')
         , (23,'UNITED KINGDOM',3,'means boost carefully special requests.')
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    CREATE TABLE sub_eq_test.part (
        p_partkey     Int32,
        p_name        String,
        p_mfgr        String,
        p_brand       String,
        p_type        String,
        p_size        Int32,
        p_container   String,
        p_retailprice Decimal(15,2),
        p_comment     String)
    ENGINE = MergeTree ORDER BY (p_partkey);
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    INSERT INTO sub_eq_test.part
    VALUES (20428,'chocolate ivory lace aquamarine spring','Manufacturer#4','Brand#43','LARGE BRUSHED BRASS',15,'MED BOX',1348.42,'al foxes. irony')
         , (70284,'slate chartreuse metallic firebrick plum','Manufacturer#4','Brand#44','STANDARD PLATED BRASS',15,'MED BAG',1254.28,'s wake silently a')
         , (73936,'cyan light indian salmon goldenrod','Manufacturer#4','Brand#45','ECONOMY PLATED BRASS',15,'JUMBO DRUM',1909.93,'foxes kin')
         , (89732,'maroon midnight indian rose deep','Manufacturer#4','Brand#42','PROMO BRUSHED BRASS',15,'JUMBO DRUM',1721.73,'ironic')
         , (105582,'orchid lime cornflower sienna firebrick','Manufacturer#4','Brand#44','STANDARD POLISHED BRASS',15,'WRAP CASE',1587.58,'mass ruthlessly.')
         , (109220,'violet snow steel purple turquoise','Manufacturer#4','Brand#41','ECONOMY BURNISHED BRASS',15,'WRAP PACK',1229.22,'cites')
         , (170979,'lawn blue steel burnished cream','Manufacturer#4','Brand#45','PROMO BRUSHED BRASS',15,'MED BAG',2049.97,'are busily')
         , (186694,'chocolate sandy seashell indian forest','Manufacturer#4','Brand#45','PROMO POLISHED BRASS',15,'SM BAG',1780.69,'e the carefully re')
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    CREATE TABLE sub_eq_test.supplier (
        s_suppkey     Int32,
        s_name        String,
        s_address     String,
        s_nationkey   Int32,
        s_phone       String,
        s_acctbal     Decimal(15,2),
        s_comment     String)
    ENGINE = MergeTree ORDER BY (s_suppkey);
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    INSERT INTO sub_eq_test.supplier
    VALUES (1731,'Supplier#000001731','Dqy8LQtY5i8GygrdOC1lt,OVsIgrGoL8Z3PMs',7,'17-115-638-8685',686.5,'lar requests. final, final platelets around the carefully even deposit')
         , (2931,'Supplier#000002931','aUivhoesqMqv0FmJcPBMxBSl8DJvXBGj',7,'17-905-318-3455',555.18,'t the fluffily ironic packages wake furiously')
         , (3113,'Supplier#000003113','HjX8M2Bjlz7pAcLzpyKT9 wNb',7,'17-164-471-2650',-604.88,'he ruthlessly final requests. express requests cajole quick')
         , (3497,'Supplier#000003497','k,,DNvZ8XHvkepAky ,22QHj4MAoxhd',7,'17-762-516-4410',60.5,'s breach accounts. express dolphins along the quickly ironic deposits hinder furiously')
         , (3937,'Supplier#000003937','kqEOwdVW,qJsJdcv6PwDJ6ii14mugDK3OgZN ngI',7,'17-621-453-7063',-63.88,'y pending asymptotes. foxes are. deposits sleep quickly b')
         , (5299,'Supplier#000005299','m7Y2G8Pg,kl5AoMPK',7,'17-904-495-9057',-752.27,'. carefully close foxes x-ray. carefully even package')
         , (9733,'Supplier#000009733','XIkUGlZFKq4IiZsAIRxFwzVBw7D',7,'17-789-292-3060',-271.69,'ions. boldly regular requests play furiously. furiously busy')
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    CREATE TABLE sub_eq_test.partsupp (
        ps_partkey     Int32,
        ps_suppkey     Int32,
        ps_availqty    Int32,
        ps_supplycost  Decimal(15,2),
        ps_comment     String)
    ENGINE = MergeTree ORDER BY (ps_partkey, ps_suppkey);
$$);

CALL clickhouse_perform('sub_eq_admin', $$
    INSERT INTO sub_eq_test.partsupp
    VALUES (20428,429,5391,624.88,'among the furiously pending deposits. slyly even instruction')
         , (20428,2931,5672,97.08,'lly final ideas. dolphins are slyly.')
         , (20428,5433,990,457,'ly. carefully regular packages wake never.')
         , (20428,7935,861,720.45,'the pending packages.')
         , (70284,285,2793,955.99,'nic theodolites. final requests detect blithely a')
         , (70284,2792,4590,515.88,'requests atop the carefully dogged dependencies.')
         , (70284,5299,1792,294.91,'dependencies nag furiously brave packages.')
         , (70284,7806,4716,687.41,'slyly regular multipliers.')
         , (73936,1458,4896,760.33,'ly even accounts.')
         , (73936,3937,2588,390.55,'quickly across the carefully even instructions.')
         , (73936,6444,8672,661.92,'e requests.')
         , (73936,8951,6131,186.26,'fly among the blithely silent theodolites.')
         , (89732,2241,151,374.03,'have to sleep slyly slyly express instructions.')
         , (89732,4749,4751,735.52,'my express deposits nod quickly.')
         , (89732,7257,7219,339.4,'alongside of the fluffily final accounts.')
         , (89732,9733,4427,206.2,'ants affix.')
         , (105582,603,1045,812.12,'my even packages.')
         , (105582,3113,3592,39.54,'its ironic deposits cajole quickly quickly even hockey players.')
         , (105582,5583,655,579.13,'detect blithely ironic deposits.')
         , (105582,8093,6553,282.73,'slyly ironic courts use fluffily asymptotes.')
         , (109220,1731,1962,412.32,'slyly about the carefully silent requests.')
         , (109220,4241,2353,991.99,'the carefully regular gifts.')
         , (109220,6751,5304,85.84,'the ideas.')
         , (109220,9221,7351,458.11,'to beans are around the unusual platelets.')
         , (170979,980,4286,96.1,'blithely special instructions.')
         , (170979,3497,3129,97.58,'ally final dependencies.')
         , (170979,6014,7721,888.68,'excuses slyly special theodolites kindle quickly.')
         , (170979,8531,5892,139.36,'the furiously regular excuses.')
         , (186694,1731,1885,345.87,'seriously silent foxes through the special foxes.')
         , (186694,4249,9871,999.65,'all asymptotes cajole furiously ironic, pending dependencies.')
         , (186694,6695,9308,448.14,'serve closely above the even deposits.')
         , (186694,9213,154,593.87,'forges hang furiously pending deposits.')
$$);

-- Import the foreign tables.
CREATE SCHEMA sub_eq_test;
IMPORT FOREIGN SCHEMA sub_eq_test FROM SERVER sub_eq_svr INTO sub_eq_test;
SET SESSION search_path = sub_eq_test,public;

-- Query 2 (TPC-H): pin the deparsed plan via EXPLAIN, then execute.
SELECT $$
select
	s_acctbal,
	s_name,
	n_name,
	p_partkey,
	p_mfgr,
	s_address,
	s_phone,
	s_comment
from
	part,
	supplier,
	partsupp,
	nation,
	region
where
	p_partkey = ps_partkey
	and s_suppkey = ps_suppkey
	and p_size = 15
	and p_type like '%BRASS'
	and s_nationkey = n_nationkey
	and n_regionkey = r_regionkey
	and r_name = 'EUROPE'
	and ps_supplycost = (
		select
			min(ps_supplycost)
		from
			partsupp,
			supplier,
			nation,
			region
		where
			p_partkey = ps_partkey
			and s_suppkey = ps_suppkey
			and s_nationkey = n_nationkey
			and n_regionkey = r_regionkey
			and r_name = 'EUROPE'
	)
order by
	s_acctbal desc,
	n_name,
	s_name,
	p_partkey
LIMIT 100;
$$ AS query2 \gset

EXPLAIN (VERBOSE, COSTS OFF) :query2
:query2

-- Cleanup
CALL clickhouse_perform('sub_eq_admin', 'DROP DATABASE sub_eq_test');
DROP USER MAPPING FOR CURRENT_USER SERVER sub_eq_svr;
DROP SERVER sub_eq_svr CASCADE;
