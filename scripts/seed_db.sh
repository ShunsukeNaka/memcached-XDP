#!/usr/bin/env bash
# MySQL の kv テーブルを作り直す。
#   usage: scripts/seed_db.sh [NUM_KEYS] [VALUE_BYTES]
# 例: scripts/seed_db.sh 1000000 256
set -euo pipefail

NUM_KEYS=${1:-100000}
VALUE_BYTES=${2:-64}
DB=${MYSQL_DATABASE:-kvstore}

if (( VALUE_BYTES < 16 || VALUE_BYTES > 8192 )); then
  echo "VALUE_BYTES must be in [16, 8192]" >&2
  exit 1
fi

docker exec -i mcxdp-mysql sh -c 'exec mysql -uroot -p"$MYSQL_ROOT_PASSWORD" "'"$DB"'"' <<SQL
SET SESSION cte_max_recursion_depth = $NUM_KEYS + 1;
TRUNCATE TABLE kv;
INSERT INTO kv (k, v)
WITH RECURSIVE seq (n) AS (
  SELECT 0 UNION ALL SELECT n + 1 FROM seq WHERE n + 1 < $NUM_KEYS
)
SELECT CONCAT('key:', LPAD(n, 10, '0')),
       RPAD(CONCAT('val:', LPAD(n, 10, '0'), ':'), $VALUE_BYTES, 'x')
FROM seq;
SELECT COUNT(*) AS rows_, MIN(LENGTH(v)) AS value_bytes FROM kv;
SQL
