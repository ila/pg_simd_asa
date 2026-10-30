SELECT o_orderpriority, count(DISTINCT o_clerk) AS clerks, count(*) AS n
FROM orders WHERE o_orderdate >= date '1995-01-01' AND o_orderdate < date '1995-02-01'
GROUP BY o_orderpriority ORDER BY o_orderpriority;
