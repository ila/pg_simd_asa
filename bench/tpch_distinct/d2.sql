SELECT o_orderpriority, count(DISTINCT l_suppkey) AS suppliers, sum(DISTINCT l_quantity) AS qty,
       avg(DISTINCT l_discount) AS disc, sum(l_extendedprice) AS rev
FROM orders, lineitem
WHERE l_orderkey = o_orderkey AND o_orderdate < date '1992-03-01'
GROUP BY o_orderpriority ORDER BY o_orderpriority;
