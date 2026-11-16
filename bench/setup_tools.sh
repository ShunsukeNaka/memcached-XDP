#!/usr/bin/env bash
# 計測ツールを入れる (要 root)。
#   sudo bench/setup_tools.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
  echo "run as root: sudo $0" >&2
  exit 1
fi

apt-get update
apt-get install -y bpftrace bpftool linux-perf strace linux-cpupower

# フレームグラフ用のスクリプト (stackcollapse-perf.pl / flamegraph.pl)
TOOLS_DIR="$(cd "$(dirname "$0")" && pwd)/tools"
if [[ ! -d "$TOOLS_DIR/FlameGraph" ]]; then
  mkdir -p "$TOOLS_DIR"
  git clone --depth 1 https://github.com/brendangregg/FlameGraph.git "$TOOLS_DIR/FlameGraph"
  chown -R "${SUDO_UID:-0}:${SUDO_GID:-0}" "$TOOLS_DIR"
fi

echo
bpftrace --version
bpftool version | head -1
perf version
strace -V | head -1
