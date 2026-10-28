-- TPC-H Q22 as DuckDB's compiler releases a query with a predicate over worlds: counter lists
-- and as_noised (the privacy repository's benchmark/tpch/tpch_as_queries/q22.sql uses the fused form).
SELECT cntrycode, CAST(as_noised(as_count(as_key)) AS BIGINT) AS numcust,
                  CAST(as_noised(as_sum(as_key, c_acctbal)) AS DECIMAL(38,2)) AS totacctbal
FROM (SELECT substring(c_phone FROM 1 FOR 2) AS cntrycode, c_acctbal,
             priv_select_gt(priv_hash(hash(c_custkey)),
                           c_acctbal,
                           (SELECT priv_div(as_sum(priv_hash(hash(c_custkey)), c_acctbal),
                                           as_count(priv_hash(hash(c_custkey)), c_acctbal))
                              FROM customer
                             WHERE c_acctbal > 0.00
                               AND substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17'))) AS as_key,
        FROM customer
        WHERE substring(c_phone FROM 1 FOR 2) IN ('13', '31', '23', '29', '30', '18', '17')
         AND as_key <> 0
         AND NOT EXISTS (FROM orders WHERE o_custkey = customer.c_custkey)) AS custsale
 GROUP BY ALL
 ORDER BY ALL;
