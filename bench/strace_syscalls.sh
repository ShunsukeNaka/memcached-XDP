#!/usr/bin/env bash
# memcached のワーカーが GET の処理で使うシステムコールを確かめる (要 root)。
# UDP と TCP それぞれで負荷をかけながら strace -c で集計する。
#   sudo bench/strace_syscalls.sh [SECONDS]
#
# 図の計測点 P4 (受信システムコールから戻る) と P5 (送信システムコールに入る) で
# どのシステムコールの tracepoint を使うかを、この結果で決める。
# strace 自体の負荷は大きいので、ここで出るスループットや時間は参考にしないこと。
set -euo pipefail

SECS=${1:-3}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MCLOAD="$ROOT/bench/loadgen/mcload"

if [[ $EUID -ne 0 ]]; then
  echo "run as root: sudo $0" >&2
  exit 1
fi
[[ -x $MCLOAD ]] || { echo "build the load generator first: make -C bench/loadgen" >&2; exit 1; }

PID=$(docker inspect -f '{{.State.Pid}}' mcxdp-memcached)
echo "memcached host pid: $PID"
echo "threads:"
for t in /proc/"$PID"/task/*; do echo "  ${t##*/} $(cat "$t/comm")"; done

for proto in udp tcp; do
  echo
  echo "===== $proto ====="
  "$MCLOAD" -P "$proto" -c 1 -w 0.5 -d $((SECS + 2)) >/dev/null &
  LOAD=$!
  sleep 1
  # -f: 全スレッドを追う。ワーカー以外のスレッドの呼び出しも混ざるので、-c の集計とは別に
  # 呼び出しの並びも少しだけ表示して、1 リクエストあたりの流れを確認する。
  timeout -s INT "$SECS" strace -f -c -p "$PID" 2>&1 | head -25 || true
  echo "--- sequence (first 30 calls) ---"
  timeout -s INT 1 strace -f -p "$PID" 2>&1 | grep -v -e '^strace: Process' | head -30 || true
  wait "$LOAD" || true
done
