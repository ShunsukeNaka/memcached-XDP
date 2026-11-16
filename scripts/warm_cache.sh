#!/usr/bin/env bash
# MySQL の kv テーブルの内容を memcached に流し込む (キャッシュのウォームアップ)。
#   usage: scripts/warm_cache.sh [LIMIT]
# LIMIT を省略すると全件。値は ASCII を前提にしている (seed_db.sh が作るデータ)。
set -euo pipefail

LIMIT=${1:-}
HOST=${MEMCACHED_HOST:-127.0.0.1}
PORT=${MEMCACHED_PORT:-11211}
DB=${MYSQL_DATABASE:-kvstore}

SQL="SELECT k, v FROM kv ORDER BY k"
[[ -n "$LIMIT" ]] && SQL+=" LIMIT $LIMIT"

docker exec mcxdp-mysql sh -c 'exec mysql -uroot -p"$MYSQL_ROOT_PASSWORD" -N -B "'"$DB"'" -e "'"$SQL"'"' 2>/dev/null \
  | awk -F'\t' '{ printf "set %s 0 0 %d noreply\r\n%s\r\n", $1, length($2), $2 } END { printf "quit\r\n" }' \
  | nc -q 5 "$HOST" "$PORT"

echo "curr_items: $(printf 'stats\r\nquit\r\n' | nc -q 2 "$HOST" "$PORT" | awk '/STAT curr_items/ {print $3}' | tr -d '\r')"
