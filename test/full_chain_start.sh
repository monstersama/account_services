#!/usr/bin/env bash
set -euo pipefail

# 仅启动 account_service / gateway / observer 三进程，不做发单与断言。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${SOURCE_DIR}/build}"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"

RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
RUN_DIR="${RUN_DIR:-${BUILD_DIR}/e2e_artifacts/${RUN_ID}}"
mkdir -p "${RUN_DIR}"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"

MONITOR_CONSOLE="${MONITOR_CONSOLE:-1}"
TRADING_DAY="${TRADING_DAY:-19700101}"
ORDERS_SHM_BASE="${ORDERS_SHM_BASE:-/orders_shm}"
POSITIONS_SHM="${POSITIONS_SHM:-/positions_shm}"
OBSERVER_TIMEOUT_MS="${OBSERVER_TIMEOUT_MS:-0}"
ORDERS_DATED_SHM="${ORDERS_SHM_BASE}_${TRADING_DAY}"
ORDERS_SHM_PATH="/dev/shm/${ORDERS_DATED_SHM#/}"
POSITIONS_SHM_PATH="/dev/shm/${POSITIONS_SHM#/}"

SERVICE_BIN="${BUILD_DIR}/src/acct_service_main"
GATEWAY_BIN="${BUILD_DIR}/gateway/acct_broker_gateway_main"
OBSERVER_BIN="${BUILD_DIR}/tools/full_chain_e2e/full_chain_observer"

SERVICE_CFG="${SERVICE_CFG:-${SOURCE_DIR}/config/default.yaml}"
GATEWAY_CFG="${GATEWAY_CFG:-${SOURCE_DIR}/config/gateway.yaml}"
OBSERVER_CFG="${OBSERVER_CFG:-${SOURCE_DIR}/config/observer.yaml}"
OBSERVER_RUNTIME_CFG="${RUN_DIR}/observer.runtime.yaml"

SERVICE_LOG="${RUN_DIR}/account_service.stdout.log"
GATEWAY_LOG="${RUN_DIR}/gateway.stdout.log"
OBSERVER_LOG="${RUN_DIR}/observer.stdout.log"

SERVICE_PID=""
GATEWAY_PID=""
OBSERVER_PID=""
SERVICE_TAIL_PID=""
GATEWAY_TAIL_PID=""
OBSERVER_TAIL_PID=""

# 轮询路径存在性，避免 observer 过早启动。
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

# 控制台实时跟随日志，便于观察三进程状态。
start_console_log_follow() {
  local prefix="$1"
  local file="$2"
  local out_pid_var="$3"
  touch "${file}"
  tail -n 0 -F "${file}" | sed -u "s/^/[${prefix}] /" &
  printf -v "${out_pid_var}" '%s' "$!"
}

# 使用运行时配置覆盖 observer 超时，避免 start_only 场景被默认 30 秒自动终止。
write_observer_runtime_config() {
  local source_cfg="$1"
  local target_cfg="$2"
  local timeout_ms="$3"
  awk -v timeout_ms="${timeout_ms}" '
    BEGIN {
      replaced = 0
    }
    /^[[:space:]]*timeout_ms[[:space:]]*:/ {
      print "timeout_ms: " timeout_ms
      replaced = 1
      next
    }
    {
      print
    }
    END {
      if (!replaced) {
        print "timeout_ms: " timeout_ms
      }
    }
  ' "${source_cfg}" > "${target_cfg}"
}

# 统一回收后台进程与日志 tail 进程。
cleanup() {
  if [[ -n "${OBSERVER_PID}" ]] && kill -0 "${OBSERVER_PID}" 2>/dev/null; then
    kill "${OBSERVER_PID}" 2>/dev/null || true
  fi
  if [[ -n "${GATEWAY_PID}" ]] && kill -0 "${GATEWAY_PID}" 2>/dev/null; then
    kill "${GATEWAY_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVICE_PID}" ]] && kill -0 "${SERVICE_PID}" 2>/dev/null; then
    kill "${SERVICE_PID}" 2>/dev/null || true
  fi

  wait "${OBSERVER_PID}" 2>/dev/null || true
  wait "${GATEWAY_PID}" 2>/dev/null || true
  wait "${SERVICE_PID}" 2>/dev/null || true

  if [[ -n "${OBSERVER_TAIL_PID}" ]] && kill -0 "${OBSERVER_TAIL_PID}" 2>/dev/null; then
    kill "${OBSERVER_TAIL_PID}" 2>/dev/null || true
  fi
  if [[ -n "${GATEWAY_TAIL_PID}" ]] && kill -0 "${GATEWAY_TAIL_PID}" 2>/dev/null; then
    kill "${GATEWAY_TAIL_PID}" 2>/dev/null || true
  fi
  if [[ -n "${SERVICE_TAIL_PID}" ]] && kill -0 "${SERVICE_TAIL_PID}" 2>/dev/null; then
    kill "${SERVICE_TAIL_PID}" 2>/dev/null || true
  fi

  wait "${OBSERVER_TAIL_PID}" 2>/dev/null || true
  wait "${GATEWAY_TAIL_PID}" 2>/dev/null || true
  wait "${SERVICE_TAIL_PID}" 2>/dev/null || true
}
trap cleanup EXIT

# 启动前校验依赖二进制与配置文件。
for bin in "${SERVICE_BIN}" "${GATEWAY_BIN}" "${OBSERVER_BIN}"; do
  if [[ ! -x "${bin}" ]]; then
    echo "[start_only] missing binary: ${bin}" >&2
    exit 1
  fi
done
for cfg in "${SERVICE_CFG}" "${GATEWAY_CFG}" "${OBSERVER_CFG}"; do
  if [[ ! -f "${cfg}" ]]; then
    echo "[start_only] missing config file: ${cfg}" >&2
    exit 1
  fi
done

# 使用进程启动参数传入配置路径，不在脚本内写配置。
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

if ! [[ "${OBSERVER_TIMEOUT_MS}" =~ ^[0-9]+$ ]]; then
  echo "[start_only] invalid OBSERVER_TIMEOUT_MS: ${OBSERVER_TIMEOUT_MS}" >&2
  exit 1
fi

wait_for_path "${ORDERS_SHM_PATH}" 10 || { echo "[start_only] orders shm not ready: ${ORDERS_SHM_PATH}" >&2; exit 1; }
wait_for_path "${POSITIONS_SHM_PATH}" 10 || { echo "[start_only] positions shm not ready: ${POSITIONS_SHM_PATH}" >&2; exit 1; }
sleep 0.3

write_observer_runtime_config "${OBSERVER_CFG}" "${OBSERVER_RUNTIME_CFG}" "${OBSERVER_TIMEOUT_MS}"

(
  cd "${RUN_DIR}"
  exec "${OBSERVER_BIN}" --config "${OBSERVER_RUNTIME_CFG}" >"${OBSERVER_LOG}" 2>&1
) &
OBSERVER_PID="$!"

if [[ "${MONITOR_CONSOLE}" -eq 1 ]]; then
  start_console_log_follow "observer" "${OBSERVER_LOG}" OBSERVER_TAIL_PID
fi

echo "[start_only] started." >&2
echo "[start_only] run_dir=${RUN_DIR}" >&2
echo "[start_only] observer_timeout_ms=${OBSERVER_TIMEOUT_MS} observer_cfg=${OBSERVER_RUNTIME_CFG}" >&2
echo "[start_only] service_pid=${SERVICE_PID} gateway_pid=${GATEWAY_PID} observer_pid=${OBSERVER_PID}" >&2
echo "[start_only] press Ctrl+C to stop all processes." >&2

# 任一进程退出则结束脚本，触发统一清理。
if wait -n "${SERVICE_PID}" "${GATEWAY_PID}" "${OBSERVER_PID}"; then
  echo "[start_only] one process exited normally." >&2
else
  rc=$?
  echo "[start_only] one process exited with rc=${rc}" >&2
  exit "${rc}"
fi
