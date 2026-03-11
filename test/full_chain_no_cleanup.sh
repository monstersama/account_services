#!/usr/bin/env bash
set -euo pipefail

# 启动 account_service + gateway 后固定发送 200 条订单，并保持运行以便持续保留共享内存。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${SOURCE_DIR}/build}"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"

RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
RUN_DIR="${RUN_DIR:-${BUILD_DIR}/e2e_artifacts/submit_200_${RUN_ID}}"
mkdir -p "${RUN_DIR}"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"

TRADING_DAY="${TRADING_DAY:-19700101}"
UPSTREAM_SHM="${UPSTREAM_SHM:-/strategy_order_shm}"
ORDERS_SHM_BASE="${ORDERS_SHM_BASE:-/orders_shm}"
POSITIONS_SHM="${POSITIONS_SHM:-/positions_shm}"
ORDERS_DATED_SHM="${ORDERS_SHM_BASE}_${TRADING_DAY}"
UPSTREAM_SHM_PATH="/dev/shm/${UPSTREAM_SHM#/}"
ORDERS_SHM_PATH="/dev/shm/${ORDERS_DATED_SHM#/}"
POSITIONS_SHM_PATH="/dev/shm/${POSITIONS_SHM#/}"
MONITOR_CONSOLE="${MONITOR_CONSOLE:-1}"

SERVICE_BIN="${BUILD_DIR}/src/acct_service_main"
GATEWAY_BIN="${BUILD_DIR}/gateway/acct_broker_gateway_main"
SERVICE_CFG="${SERVICE_CFG:-${SOURCE_DIR}/config/default.yaml}"
GATEWAY_CFG="${GATEWAY_CFG:-${SOURCE_DIR}/config/gateway.yaml}"
SERVICE_LOG="${RUN_DIR}/account_service.stdout.log"
GATEWAY_LOG="${RUN_DIR}/gateway.stdout.log"

SUBMIT_SCRIPT="${SCRIPT_DIR}/full_chain_submit.sh"
SERVICE_PID=""
GATEWAY_PID=""
SERVICE_TAIL_PID=""
GATEWAY_TAIL_PID=""

wait_for_path() {
  local path="$1"
  local timeout_sec="$2"
  local deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    if [[ -e "${path}" ]]; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

start_console_log_follow() {
  local prefix="$1"
  local file="$2"
  local out_pid_var="$3"
  touch "${file}"
  tail -n 0 -F "${file}" | sed -u "s/^/[${prefix}] /" &
  printf -v "${out_pid_var}" '%s' "$!"
}

cleanup() {
  if [[ -n "${GATEWAY_TAIL_PID}" ]] && kill -0 "${GATEWAY_TAIL_PID}" 2>/dev/null; then
    kill "${GATEWAY_TAIL_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVICE_TAIL_PID}" ]] && kill -0 "${SERVICE_TAIL_PID}" 2>/dev/null; then
    kill "${SERVICE_TAIL_PID}" 2>/dev/null || true
  fi
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    kill "${GATEWAY_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVICE_PID}" ]] && kill -0 "${SERVICE_PID}" 2>/dev/null; then
    kill "${SERVICE_PID}" 2>/dev/null || true
  fi

  wait "${GATEWAY_TAIL_PID}" 2>/dev/null || true
  wait "${SERVICE_TAIL_PID}" 2>/dev/null || true
  wait "${GATEWAY_PID}" 2>/dev/null || true
  wait "${SERVICE_PID}" 2>/dev/null || true
}
trap cleanup EXIT

if [[ ! -x "${SERVICE_BIN}" ]]; then
  echo "[submit_200_no_cleanup] missing binary: ${SERVICE_BIN}" >&2
  exit 1
fi
if [[ ! -x "${GATEWAY_BIN}" ]]; then
  echo "[submit_200_no_cleanup] missing binary: ${GATEWAY_BIN}" >&2
  exit 1
fi
if [[ ! -f "${SERVICE_CFG}" ]]; then
  echo "[submit_200_no_cleanup] missing config file: ${SERVICE_CFG}" >&2
  exit 1
fi
if [[ ! -f "${GATEWAY_CFG}" ]]; then
  echo "[submit_200_no_cleanup] missing config file: ${GATEWAY_CFG}" >&2
  exit 1
fi
if [[ ! -x "${SUBMIT_SCRIPT}" ]]; then
  echo "[submit_200_no_cleanup] missing executable script: ${SUBMIT_SCRIPT}" >&2
  exit 1
fi

(
  cd "${SOURCE_DIR}"
  exec "${SERVICE_BIN}" --config "${SERVICE_CFG}" >"${SERVICE_LOG}" 2>&1
) &
SERVICE_PID="$!"

(
  cd "${SOURCE_DIR}"
  exec "${GATEWAY_BIN}" --config "${GATEWAY_CFG}" >"${GATEWAY_LOG}" 2>&1
) &
GATEWAY_PID="$!"

if [[ "${MONITOR_CONSOLE}" -eq 1 ]]; then
  start_console_log_follow "account_service" "${SERVICE_LOG}" SERVICE_TAIL_PID
  start_console_log_follow "gateway" "${GATEWAY_LOG}" GATEWAY_TAIL_PID
fi

wait_for_path "${UPSTREAM_SHM_PATH}" 10 || {
  echo "[submit_200_no_cleanup] upstream shm not ready: ${UPSTREAM_SHM_PATH}" >&2
  exit 1
}
wait_for_path "${ORDERS_SHM_PATH}" 10 || {
  echo "[submit_200_no_cleanup] orders shm not ready: ${ORDERS_SHM_PATH}" >&2
  exit 1
}
wait_for_path "${POSITIONS_SHM_PATH}" 10 || {
  echo "[submit_200_no_cleanup] positions shm not ready: ${POSITIONS_SHM_PATH}" >&2
  exit 1
}

# 固定发 200 单；共享内存保留由 full_chain_submit.sh 内置 no-cleanup 参数保证。
RUN_DIR="${RUN_DIR}" ORDER_COUNT=200 "${SUBMIT_SCRIPT}" "${BUILD_DIR}"

echo "[submit_200_no_cleanup] submitted 200 orders." >&2
echo "[submit_200_no_cleanup] run_dir=${RUN_DIR}" >&2
echo "[submit_200_no_cleanup] service_pid=${SERVICE_PID} gateway_pid=${GATEWAY_PID}" >&2
echo "[submit_200_no_cleanup] processes remain running; press Ctrl+C to stop." >&2

if wait -n "${SERVICE_PID}" "${GATEWAY_PID}"; then
  echo "[submit_200_no_cleanup] one process exited normally." >&2
else
  rc=$?
  echo "[submit_200_no_cleanup] one process exited with rc=${rc}" >&2
  exit "${rc}"
fi
