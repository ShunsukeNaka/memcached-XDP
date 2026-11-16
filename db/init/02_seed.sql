-- 初期データ: key:0000000000 〜 key:0000099999 の 10 万件、値は 64 バイト。
-- キー数・値サイズを変えたい場合は scripts/seed_db.sh で入れ直す。
USE kvstore;

SET SESSION cte_max_recursion_depth = 1000000;

INSERT INTO kv (k, v)
WITH RECURSIVE seq (n) AS (
  SELECT 0
  UNION ALL
  SELECT n + 1 FROM seq WHERE n + 1 < 100000
)
SELECT CONCAT('key:', LPAD(n, 10, '0')),
       RPAD(CONCAT('val:', LPAD(n, 10, '0'), ':'), 64, 'x')
FROM seq;
