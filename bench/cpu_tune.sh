#!/usr/bin/env bash
# 計測用の CPU 設定を切り替える。
#   bench/cpu_tune.sh show            現在の設定を表示 (root 不要。結果と一緒に記録する)
#   sudo bench/cpu_tune.sh apply      計測用の設定にする
#   sudo bench/cpu_tune.sh restore    apply の前の設定に戻す
#
# apply でやること:
#   - 周波数ガバナーを performance にする (負荷によってクロックが変わらないように)
#   - Turbo を切る (温度や使うコア数でクロックが変わらないように)
#   - 起床にかかる時間が MAX_IDLE_LATENCY_US を超える C-state を使わせない
#     (低負荷のときに深いアイドル状態から起きる時間がレイテンシに乗らないように)
set -euo pipefail

MAX_IDLE_LATENCY_US=${MAX_IDLE_LATENCY_US:-10}
STATE_FILE=${STATE_FILE:-/var/tmp/mcxdp-cpu-tune.saved}
CPU_SYS=/sys/devices/system/cpu
NO_TURBO=$CPU_SYS/intel_pstate/no_turbo
BOOST=$CPU_SYS/cpufreq/boost

need_root() {
  if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0 $1" >&2
    exit 1
  fi
}

show() {
  echo "cpu_model:  $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "driver:     $(cat $CPU_SYS/cpu0/cpufreq/scaling_driver 2>/dev/null || echo n/a)"
  echo "governor:   $(cat $CPU_SYS/cpu*/cpufreq/scaling_governor 2>/dev/null | sort | uniq -c | xargs)"
  if [[ -r $NO_TURBO ]]; then echo "no_turbo:   $(cat $NO_TURBO)"; fi
  if [[ -r $BOOST ]]; then echo "boost:      $(cat $BOOST)"; fi
  echo "cur_freq:   $(cat $CPU_SYS/cpu*/cpufreq/scaling_cur_freq 2>/dev/null | awk '{printf "%d ", $1/1000}')MHz"
  echo "idle states (cpu0):"
  for s in $CPU_SYS/cpu0/cpuidle/state*; do
    printf '  %-6s latency=%4sus disabled=%s\n' "$(cat "$s/name")" "$(cat "$s/latency")" "$(cat "$s/disable")"
  done
}

apply() {
  need_root apply
  if [[ ! -f $STATE_FILE ]]; then
    {
      for c in $CPU_SYS/cpu[0-9]*; do
        if [[ -w $c/cpufreq/scaling_governor ]]; then
          echo "$c/cpufreq/scaling_governor $(cat "$c/cpufreq/scaling_governor")"
        fi
        for s in "$c"/cpuidle/state*; do
          if [[ -w $s/disable ]]; then echo "$s/disable $(cat "$s/disable")"; fi
        done
      done
      if [[ -w $NO_TURBO ]]; then echo "$NO_TURBO $(cat $NO_TURBO)"; fi
      if [[ -w $BOOST ]]; then echo "$BOOST $(cat $BOOST)"; fi
    } >"$STATE_FILE"
    echo "saved current settings to $STATE_FILE"
  else
    echo "$STATE_FILE already exists; keeping it (run restore first to re-save)"
  fi

  for g in $CPU_SYS/cpu[0-9]*/cpufreq/scaling_governor; do echo performance >"$g"; done
  if [[ -w $NO_TURBO ]]; then echo 1 >$NO_TURBO; fi
  if [[ -w $BOOST ]]; then echo 0 >$BOOST; fi
  for s in $CPU_SYS/cpu[0-9]*/cpuidle/state*; do
    if (( $(cat "$s/latency") > MAX_IDLE_LATENCY_US )); then echo 1 >"$s/disable"; else echo 0 >"$s/disable"; fi
  done
  show
}

restore() {
  need_root restore
  if [[ ! -f $STATE_FILE ]]; then
    echo "$STATE_FILE not found; nothing to restore" >&2
    exit 1
  fi
  while read -r path value; do
    echo "$value" >"$path"
  done <"$STATE_FILE"
  rm -f "$STATE_FILE"
  show
}

case "${1:-show}" in
  show) show ;;
  apply) apply ;;
  restore) restore ;;
  *) echo "usage: $0 {show|apply|restore}" >&2; exit 2 ;;
esac
