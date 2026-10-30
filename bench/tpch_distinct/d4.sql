SELECT n_name, count(DISTINCT o_orderdate) AS days
FROM nation, customer, orders
WHERE n_nationkey = c_nationkey AND c_custkey = o_custkey AND o_totalprice > 450000
GROUP BY n_name ORDER BY n_name;
