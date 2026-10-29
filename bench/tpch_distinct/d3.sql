SELECT count(DISTINCT l_partkey) AS parts, sum(DISTINCT l_tax) AS taxes
FROM lineitem WHERE l_shipdate >= date '1996-06-01' AND l_shipdate < date '1996-06-08';
