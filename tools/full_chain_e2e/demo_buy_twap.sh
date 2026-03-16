#!/usr/bin/env bash
set -euo pipefail

# 演示脚本：提交一笔 SH 600000 买入 TWAP 订单。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/build}"
SUBMIT_BIN="${BUILD_DIR}/tools/full_chain_e2e/order_submit_cli"

if [[ ! -x "${SUBMIT_BIN}" ]]; then
  echo "[demo_submit_twap] missing binary: ${SUBMIT_BIN}" >&2
  echo "[demo_submit_twap] build the project first, for example:" >&2
  echo "  CC=clang CXX=clang++ cmake -S . -B build" >&2
  echo "  cmake --build build -j4 --target order_submit_cli" >&2
  exit 1
fi

cmd=(
  "${SUBMIT_BIN}"
  --security 600000
  --side buy
  --market sh
  --volume 500
  --price 10
  --passive-exec-algo twap
)

echo "[demo_submit_twap] running: ${cmd[*]}"
"${cmd[@]}"
