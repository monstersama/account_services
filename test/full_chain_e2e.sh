#!/usr/bin/env bash
set -euo pipefail

# 解析仓库和 build 目录，兼容手工执行与 ctest 执行。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${SOURCE_DIR}/build}"
if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "[e2e] build dir not found: ${BUILD_DIR}" >&2
  exit 1
fi
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"

RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
TRADING_DAY="${TRADING_DAY:-19700101}"
TIMEOUT_SEC="${TIMEOUT_SEC:-180}"
ORDER_COUNT="${ORDER_COUNT:-100}"
SUBMIT_INTERVAL_SEC="${SUBMIT_INTERVAL_SEC:-0.1}"
SUBMIT_DURATION_SEC="${SUBMIT_DURATION_SEC:-60}"
RANDOM_SIDE="${RANDOM_SIDE:-1}"
MONITOR_CONSOLE="${MONITOR_CONSOLE:-1}"
# 固定样本 DB：默认使用仓库 data 目录下的数据库，可通过环境变量覆盖。
SAMPLE_DB_PATH="${SAMPLE_DB_PATH:-${SOURCE_DIR}/data/account_service.db}"
# 默认追加 2 个“非数据库内”的 SZ 标的，用于覆盖新标的创建路径。
EXTRA_SECURITY_IDS="${EXTRA_SECURITY_IDS:-990001,990002}"

# 与配置文件保持一致的 SHM/交易日默认值，可通过环境变量覆盖。
UPSTREAM_SHM="${UPSTREAM_SHM:-/strategy_order_shm}"
DOWNSTREAM_SHM="${DOWNSTREAM_SHM:-/downstream_order_shm}"
TRADES_SHM="${TRADES_SHM:-/trades_shm}"
ORDERS_SHM_BASE="${ORDERS_SHM_BASE:-/orders_shm}"
POSITIONS_SHM="${POSITIONS_SHM:-/positions_shm}"
ORDERS_DATED_SHM="${ORDERS_SHM_BASE}_${TRADING_DAY}"

ORDERS_SHM_PATH="/dev/shm/${ORDERS_DATED_SHM#/}"
POSITIONS_SHM_PATH="/dev/shm/${POSITIONS_SHM#/}"
UPSTREAM_SHM_PATH="/dev/shm/${UPSTREAM_SHM#/}"

RUN_DIR="${BUILD_DIR}/e2e_artifacts/${RUN_ID}"
mkdir -p "${RUN_DIR}"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"

SERVICE_BIN="${BUILD_DIR}/src/acct_service_main"
GATEWAY_BIN="${BUILD_DIR}/gateway/acct_broker_gateway_main"
OBSERVER_BIN="${BUILD_DIR}/tools/full_chain_e2e/full_chain_observer"
SUBMIT_BIN="${BUILD_DIR}/tools/full_chain_e2e/order_submit_cli"
SUBMIT_SCRIPT="${SUBMIT_SCRIPT:-${SOURCE_DIR}/test/full_chain_submit.sh}"

SERVICE_CFG="${SERVICE_CFG:-${SOURCE_DIR}/config/default.yaml}"
GATEWAY_CFG="${GATEWAY_CFG:-${SOURCE_DIR}/config/gateway.yaml}"
OBSERVER_CFG="${OBSERVER_CFG:-${SOURCE_DIR}/config/observer.yaml}"

SERVICE_LOG="${RUN_DIR}/account_service.stdout.log"
GATEWAY_LOG="${RUN_DIR}/gateway.stdout.log"
OBSERVER_LOG="${RUN_DIR}/observer.stdout.log"
SUBMIT_LOG="${RUN_DIR}/order_submit.stderr.log"

ORDERS_CSV="${RUN_DIR}/orders_final.csv"
POSITIONS_CSV="${RUN_DIR}/positions_final.csv"
ORDER_IDS_CSV="${RUN_DIR}/order_ids.csv"
ORDER_IDS_TXT="${RUN_DIR}/order_ids.txt"

SERVICE_PID=""
GATEWAY_PID=""
OBSERVER_PID=""
SERVICE_TAIL_PID=""
GATEWAY_TAIL_PID=""
OBSERVER_TAIL_PID=""

DOWNSTREAM_SHM_PATH="/dev/shm/${DOWNSTREAM_SHM#/}"
TRADES_SHM_PATH="/dev/shm/${TRADES_SHM#/}"

# 统一终止后台进程并在失败时回捞关键日志。
cleanup() {
  local exit_code="$1"

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

  # 清理本次运行使用的 SHM 名称，避免污染后续手工调试。
  rm -f "${UPSTREAM_SHM_PATH}" "${DOWNSTREAM_SHM_PATH}" "${TRADES_SHM_PATH}" "${ORDERS_SHM_PATH}" "${POSITIONS_SHM_PATH}" || true

  if [[ "${exit_code}" -ne 0 ]]; then
    echo "[e2e] failed, artifacts: ${RUN_DIR}" >&2
    [[ -f "${SERVICE_LOG}" ]] && { echo "--- account_service (tail) ---" >&2; tail -n 80 "${SERVICE_LOG}" >&2; }
    [[ -f "${GATEWAY_LOG}" ]] && { echo "--- gateway (tail) ---" >&2; tail -n 80 "${GATEWAY_LOG}" >&2; }
    [[ -f "${OBSERVER_LOG}" ]] && { echo "--- observer (tail) ---" >&2; tail -n 80 "${OBSERVER_LOG}" >&2; }
    [[ -f "${ORDERS_CSV}" ]] && { echo "--- orders_final.csv (tail) ---" >&2; tail -n 20 "${ORDERS_CSV}" >&2; }
    [[ -f "${POSITIONS_CSV}" ]] && { echo "--- positions_final.csv (tail) ---" >&2; tail -n 20 "${POSITIONS_CSV}" >&2; }
  fi
}
trap 'cleanup $?' EXIT

# 轮询文件存在性，避免进程尚未完成初始化。
wait_for_file() {
  local path="$1"
  local timeout_sec="$2"
  local deadline=$((SECONDS + timeout_sec))
  while (( SECONDS < deadline )); do
    if [[ -f "${path}" ]]; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

# 轮询路径存在性，适配 /dev/shm 对象就绪检测。
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

# 实时输出日志到控制台，便于观测各进程行为。
start_console_log_follow() {
  local prefix="$1"
  local file="$2"
  local out_pid_var="$3"
  touch "${file}"
  tail -n 0 -F "${file}" | sed -u "s/^/[${prefix}] /" &
  printf -v "${out_pid_var}" '%s' "$!"
}

# 校验依赖二进制是否已构建。
for bin in "${SERVICE_BIN}" "${GATEWAY_BIN}" "${OBSERVER_BIN}" "${SUBMIT_BIN}"; do
  if [[ ! -x "${bin}" ]]; then
    echo "[e2e] missing binary: ${bin}" >&2
    exit 1
  fi
done
for cfg in "${SERVICE_CFG}" "${GATEWAY_CFG}" "${OBSERVER_CFG}"; do
  if [[ ! -f "${cfg}" ]]; then
    echo "[e2e] missing config file: ${cfg}" >&2
    exit 1
  fi
done
if [[ ! -x "${SUBMIT_SCRIPT}" ]]; then
  echo "[e2e] missing executable script: ${SUBMIT_SCRIPT}" >&2
  exit 1
fi

# 从 observer 配置中读取 output_dir，定位 CSV 产物路径。
OBSERVER_OUTPUT_DIR="$(awk -F':' '
  /^[[:space:]]*output_dir[[:space:]]*:/ {
    sub(/^[^:]*:[[:space:]]*/, "", $0)
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", $0)
    gsub(/^"|"$/, "", $0)
    gsub(/^'\''|'\''$/, "", $0)
    print
    exit
  }' "${OBSERVER_CFG}")"
if [[ -z "${OBSERVER_OUTPUT_DIR}" ]]; then
  OBSERVER_OUTPUT_DIR="."
fi
if [[ "${OBSERVER_OUTPUT_DIR}" = /* ]]; then
  OBSERVER_OUTPUT_PATH="${OBSERVER_OUTPUT_DIR}"
else
  OBSERVER_OUTPUT_PATH="${RUN_DIR}/${OBSERVER_OUTPUT_DIR}"
fi
ORDERS_CSV="${OBSERVER_OUTPUT_PATH}/orders_final.csv"
POSITIONS_CSV="${OBSERVER_OUTPUT_PATH}/positions_final.csv"

# 校验发单参数。
if [[ -n "${ORDER_COUNT}" ]] && ! [[ "${ORDER_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
  echo "[e2e] invalid ORDER_COUNT: ${ORDER_COUNT}" >&2
  exit 1
fi
if ! awk -v value="${SUBMIT_INTERVAL_SEC}" '
    BEGIN {
      is_number = (value ~ /^([0-9]+([.][0-9]+)?|[.][0-9]+)$/)
      exit((is_number && (value + 0) > 0) ? 0 : 1)
    }'; then
  echo "[e2e] invalid SUBMIT_INTERVAL_SEC: ${SUBMIT_INTERVAL_SEC}" >&2
  exit 1
fi
if ! [[ "${SUBMIT_DURATION_SEC}" =~ ^[1-9][0-9]*$ ]]; then
  echo "[e2e] invalid SUBMIT_DURATION_SEC: ${SUBMIT_DURATION_SEC}" >&2
  exit 1
fi
if ! [[ "${RANDOM_SIDE}" =~ ^[01]$ ]]; then
  echo "[e2e] invalid RANDOM_SIDE: ${RANDOM_SIDE}" >&2
  exit 1
fi
if ! [[ "${MONITOR_CONSOLE}" =~ ^[01]$ ]]; then
  echo "[e2e] invalid MONITOR_CONSOLE: ${MONITOR_CONSOLE}" >&2
  exit 1
fi

# 发单细节由独立脚本 full_chain_submit.sh 负责。

if [[ -n "${ORDER_COUNT}" ]]; then
  min_timeout_sec="$(awk -v order_count="${ORDER_COUNT}" -v interval_sec="${SUBMIT_INTERVAL_SEC}" '
    BEGIN {
      # 对小数发单间隔按秒向上取整，给 observer 留足等待窗口。
      raw_timeout = order_count * interval_sec + 60
      min_timeout = int(raw_timeout)
      if ((raw_timeout - min_timeout) > 1e-9) {
        min_timeout += 1
      }
      print min_timeout
    }')"
else
  min_timeout_sec=$((SUBMIT_DURATION_SEC + 60))
fi
if (( TIMEOUT_SEC < min_timeout_sec )); then
  TIMEOUT_SEC="${min_timeout_sec}"
fi

# 通过启动参数传入配置文件路径，避免脚本内写入配置内容。
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

# 等待关键 SHM 对象创建完成，再启动 observer，避免 open 竞争失败。
wait_for_path "${ORDERS_SHM_PATH}" 10 || { echo "[e2e] orders shm not ready: ${ORDERS_SHM_PATH}" >&2; exit 1; }
wait_for_path "${POSITIONS_SHM_PATH}" 10 || { echo "[e2e] positions shm not ready: ${POSITIONS_SHM_PATH}" >&2; exit 1; }
wait_for_path "${UPSTREAM_SHM_PATH}" 10 || { echo "[e2e] upstream shm not ready: ${UPSTREAM_SHM_PATH}" >&2; exit 1; }
sleep 0.3

# observer 默认 output_dir 为 "."，切换到 RUN_DIR 启动可将 CSV 保持在本次产物目录。
(
  cd "${RUN_DIR}"
  exec "${OBSERVER_BIN}" --config "${OBSERVER_CFG}" >"${OBSERVER_LOG}" 2>&1
) &
OBSERVER_PID="$!"

if [[ "${MONITOR_CONSOLE}" -eq 1 ]]; then
  start_console_log_follow "observer" "${OBSERVER_LOG}" OBSERVER_TAIL_PID
fi

# 等待快照 CSV 创建，确保后续轮询有输入源。
wait_for_file "${ORDERS_CSV}" 20 || { echo "[e2e] orders csv not ready" >&2; exit 1; }
wait_for_file "${POSITIONS_CSV}" 20 || { echo "[e2e] positions csv not ready" >&2; exit 1; }

# 发单改为调用独立脚本，避免 e2e runner 内嵌提交细节。
SAMPLE_DB_PATH="${SAMPLE_DB_PATH}" \
EXTRA_SECURITY_IDS="${EXTRA_SECURITY_IDS}" \
TRADING_DAY="${TRADING_DAY}" \
UPSTREAM_SHM="${UPSTREAM_SHM}" \
ORDERS_SHM_BASE="${ORDERS_SHM_BASE}" \
ORDER_COUNT="${ORDER_COUNT}" \
SUBMIT_INTERVAL_SEC="${SUBMIT_INTERVAL_SEC}" \
SUBMIT_DURATION_SEC="${SUBMIT_DURATION_SEC}" \
RANDOM_SIDE="${RANDOM_SIDE}" \
RUN_DIR="${RUN_DIR}" \
SUBMIT_LOG="${SUBMIT_LOG}" \
ORDER_IDS_CSV="${ORDER_IDS_CSV}" \
ORDER_IDS_TXT="${ORDER_IDS_TXT}" \
FIRST_ORDER_ID_OUT="${RUN_DIR}/order_id.txt" \
"${SUBMIT_SCRIPT}" "${BUILD_DIR}"

submitted_count="$(wc -l < "${ORDER_IDS_TXT}")"
submitted_count="${submitted_count//[[:space:]]/}"
if [[ -z "${submitted_count}" || "${submitted_count}" == "0" ]]; then
  echo "[e2e] no orders submitted" >&2
  exit 1
fi

# 轮询订单/持仓 CSV，直到两侧条件同时满足。
SUCCESS=0
DEADLINE=$((SECONDS + TIMEOUT_SEC))
while (( SECONDS < DEADLINE )); do
  ORDERS_OK=0
  POSITIONS_OK=0

  if awk -F',' '
      NR==FNR {
        if (NR>1) {
          want[$1]=1
          total+=1
        }
        next
      }
      FNR==1 {
        for (i=1; i<=NF; ++i) {
          orders_col[$i]=i
        }
        if (!("internal_order_id" in orders_col) || !("stage" in orders_col)) {
          exit 2
        }
        next
      }
      {
        order_id=$(orders_col["internal_order_id"])
        order_stage=$(orders_col["stage"])
        if ((order_id in want) && (order_stage+0)>=4) {
          seen[order_id]=1
        }
      }
      END {
        matched=0
        for (id in seen) {
          matched+=1
        }
        exit(matched==total ? 0 : 1)
      }' "${ORDER_IDS_CSV}" "${ORDERS_CSV}"; then
    ORDERS_OK=1
  fi

  if awk -F',' '
      FILENAME==ARGV[1] {
        if (FNR>1) {
          want_order[$1]=1
        }
        next
      }
      FILENAME==ARGV[2] && FNR==1 {
        for (i=1; i<=NF; ++i) {
          orders_col[$i]=i
        }
        if (!("internal_order_id" in orders_col) ||
            !("volume_traded" in orders_col) ||
            !("internal_security_id" in orders_col)) {
          exit 2
        }
        next
      }
      FILENAME==ARGV[2] {
        order_id=$(orders_col["internal_order_id"])
        volume_traded=$(orders_col["volume_traded"])
        if ((order_id in want_order) && ((volume_traded+0)>0)) {
          key=$(orders_col["internal_security_id"])
          gsub(/"/, "", key)
          want_position[key]=1
        }
        next
      }
      FILENAME==ARGV[3] && FNR==1 {
        for (i=1; i<=NF; ++i) {
          positions_col[$i]=i
        }
        if (!("event_kind" in positions_col) ||
            !("row_key" in positions_col) ||
            !("position_volume_buy_traded" in positions_col) ||
            !("position_volume_sell_traded" in positions_col)) {
          exit 2
        }
        next
      }
      FILENAME==ARGV[3] {
        kind=$(positions_col["event_kind"])
        key=$(positions_col["row_key"])
        gsub(/"/, "", kind)
        gsub(/"/, "", key)
        buy_traded=$(positions_col["position_volume_buy_traded"])
        sell_traded=$(positions_col["position_volume_sell_traded"])
        if (kind=="position" && (key in want_position) && ((buy_traded+0)>0 || (sell_traded+0)>0)) {
          seen[key]=1
        }
        next
      }
      END {
        total=0
        matched=0
        for (id in want_position) {
          total+=1
          if (id in seen) {
            matched+=1
          }
        }
        if (total==0) {
          exit 0
        }
        exit(matched==total ? 0 : 1)
      }' "${ORDER_IDS_CSV}" "${ORDERS_CSV}" "${POSITIONS_CSV}"; then
    POSITIONS_OK=1
  fi

  if [[ "${ORDERS_OK}" -eq 1 && "${POSITIONS_OK}" -eq 1 ]]; then
    SUCCESS=1
    break
  fi

  sleep 0.2
done

if [[ "${SUCCESS}" -ne 1 ]]; then
  echo "[e2e] timeout waiting for expected order/position updates" >&2
  exit 1
fi

echo "[e2e] PASS order_count=${submitted_count} artifacts=${RUN_DIR}" >&2
