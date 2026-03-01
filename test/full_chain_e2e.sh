#!/usr/bin/env bash
set -euo pipefail

# 解析仓库和 build 目录，兼容手工执行与 ctest 执行。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${SOURCE_DIR}/build}"

RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
TRADING_DAY="$(date +%Y%m%d)"
TIMEOUT_SEC="${TIMEOUT_SEC:-180}"
ORDER_COUNT="${ORDER_COUNT:-}"
ORDER_START_SECURITY="${ORDER_START_SECURITY:-1}"
SUBMIT_INTERVAL_SEC="${SUBMIT_INTERVAL_SEC:-1}"
SUBMIT_DURATION_SEC="${SUBMIT_DURATION_SEC:-60}"
RANDOM_SIDE="${RANDOM_SIDE:-0}"
MONITOR_CONSOLE="${MONITOR_CONSOLE:-1}"

UPSTREAM_SHM="/acct_e2e_upstream_${RUN_ID}"
DOWNSTREAM_SHM="/acct_e2e_downstream_${RUN_ID}"
TRADES_SHM="/acct_e2e_trades_${RUN_ID}"
ORDERS_SHM_BASE="/acct_e2e_orders_${RUN_ID}"
POSITIONS_SHM="/acct_e2e_positions_${RUN_ID}"
ORDERS_DATED_SHM="${ORDERS_SHM_BASE}_${TRADING_DAY}"

ORDERS_SHM_PATH="/dev/shm/${ORDERS_DATED_SHM#/}"
POSITIONS_SHM_PATH="/dev/shm/${POSITIONS_SHM#/}"
UPSTREAM_SHM_PATH="/dev/shm/${UPSTREAM_SHM#/}"

RUN_DIR="${BUILD_DIR}/e2e_artifacts/${RUN_ID}"
mkdir -p "${RUN_DIR}"

SERVICE_BIN="${BUILD_DIR}/src/acct_service_main"
GATEWAY_BIN="${BUILD_DIR}/gateway/acct_broker_gateway_main"
OBSERVER_BIN="${BUILD_DIR}/tools/full_chain_e2e/full_chain_observer"
SUBMIT_BIN="${BUILD_DIR}/tools/full_chain_e2e/order_submit_cli"

SERVICE_CFG="${RUN_DIR}/account_service.yaml"
GATEWAY_CFG="${RUN_DIR}/gateway.yaml"

SERVICE_LOG="${RUN_DIR}/account_service.stdout.log"
GATEWAY_LOG="${RUN_DIR}/gateway.stdout.log"
OBSERVER_LOG="${RUN_DIR}/observer.stdout.log"
SUBMIT_LOG="${RUN_DIR}/order_submit.stderr.log"

ORDERS_CSV="${RUN_DIR}/orders_events.csv"
POSITIONS_CSV="${RUN_DIR}/positions_events.csv"
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

  # 统一清理本次运行创建的 SHM 名称，避免污染后续手工调试。
  rm -f "${UPSTREAM_SHM_PATH}" "${DOWNSTREAM_SHM_PATH}" "${TRADES_SHM_PATH}" "${ORDERS_SHM_PATH}" "${POSITIONS_SHM_PATH}" || true

  if [[ "${exit_code}" -ne 0 ]]; then
    echo "[e2e] failed, artifacts: ${RUN_DIR}" >&2
    [[ -f "${SERVICE_LOG}" ]] && { echo "--- account_service (tail) ---" >&2; tail -n 80 "${SERVICE_LOG}" >&2; }
    [[ -f "${GATEWAY_LOG}" ]] && { echo "--- gateway (tail) ---" >&2; tail -n 80 "${GATEWAY_LOG}" >&2; }
    [[ -f "${OBSERVER_LOG}" ]] && { echo "--- observer (tail) ---" >&2; tail -n 80 "${OBSERVER_LOG}" >&2; }
    [[ -f "${ORDERS_CSV}" ]] && { echo "--- orders_events.csv (tail) ---" >&2; tail -n 20 "${ORDERS_CSV}" >&2; }
    [[ -f "${POSITIONS_CSV}" ]] && { echo "--- positions_events.csv (tail) ---" >&2; tail -n 20 "${POSITIONS_CSV}" >&2; }
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

# 校验发单参数。
if [[ -n "${ORDER_COUNT}" ]] && ! [[ "${ORDER_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
  echo "[e2e] invalid ORDER_COUNT: ${ORDER_COUNT}" >&2
  exit 1
fi
if ! [[ "${ORDER_START_SECURITY}" =~ ^[1-9][0-9]*$ ]]; then
  echo "[e2e] invalid ORDER_START_SECURITY: ${ORDER_START_SECURITY}" >&2
  exit 1
fi
if ! [[ "${SUBMIT_INTERVAL_SEC}" =~ ^[1-9][0-9]*$ ]]; then
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

if [[ -n "${ORDER_COUNT}" ]]; then
  min_timeout_sec=$((ORDER_COUNT * SUBMIT_INTERVAL_SEC + 60))
else
  min_timeout_sec=$((SUBMIT_DURATION_SEC + 60))
fi
if (( TIMEOUT_SEC < min_timeout_sec )); then
  TIMEOUT_SEC="${min_timeout_sec}"
fi

# 生成 account_service 临时配置，使用唯一 SHM 名称隔离运行。
cat > "${SERVICE_CFG}" <<YAML
account_id: 1001
trading_day: "${TRADING_DAY}"

shm:
  upstream_shm_name: "${UPSTREAM_SHM}"
  downstream_shm_name: "${DOWNSTREAM_SHM}"
  trades_shm_name: "${TRADES_SHM}"
  orders_shm_name: "${ORDERS_SHM_BASE}"
  positions_shm_name: "${POSITIONS_SHM}"
  create_if_not_exist: true

event_loop:
  busy_polling: false
  poll_batch_size: 64
  idle_sleep_us: 50
  stats_interval_ms: 1000
  pin_cpu: false
  cpu_core: -1

risk:
  max_order_value: 0
  max_order_volume: 0
  max_daily_turnover: 0
  max_orders_per_second: 0
  enable_price_limit_check: false
  enable_duplicate_check: false
  enable_fund_check: false
  enable_position_check: true
  duplicate_window_ns: 100000000

split:
  strategy: "none"
  max_child_volume: 0
  min_child_volume: 100
  max_child_count: 100
  interval_ms: 0
  randomize_factor: 0.0

log:
  log_dir: "${RUN_DIR}"
  log_level: "info"
  async_logging: true
  async_queue_size: 8192

db:
  db_path: ""
  enable_persistence: false
  sync_interval_ms: 1000
YAML

# 生成 gateway 临时配置，与 account_service 的 SHM 名称保持一致。
cat > "${GATEWAY_CFG}" <<YAML
account_id: 1001
downstream_shm: "${DOWNSTREAM_SHM}"
trades_shm: "${TRADES_SHM}"
orders_shm: "${ORDERS_SHM_BASE}"
trading_day: "${TRADING_DAY}"

broker_type: "sim"
adapter_so: ""

create_if_not_exist: true
poll_batch_size: 64
idle_sleep_us: 50
stats_interval_ms: 1000
max_retries: 3
retry_interval_us: 200
YAML

# 启动账户服务、网关和观测器三进程。
"${SERVICE_BIN}" --config "${SERVICE_CFG}" >"${SERVICE_LOG}" 2>&1 &
SERVICE_PID="$!"

"${GATEWAY_BIN}" --config "${GATEWAY_CFG}" >"${GATEWAY_LOG}" 2>&1 &
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

"${OBSERVER_BIN}" \
  --orders-shm "${ORDERS_SHM_BASE}" \
  --trading-day "${TRADING_DAY}" \
  --positions-shm "${POSITIONS_SHM}" \
  --poll-ms 100 \
  --timeout-ms $((TIMEOUT_SEC * 1000)) \
  --output-dir "${RUN_DIR}" >"${OBSERVER_LOG}" 2>&1 &
OBSERVER_PID="$!"

if [[ "${MONITOR_CONSOLE}" -eq 1 ]]; then
  start_console_log_follow "observer" "${OBSERVER_LOG}" OBSERVER_TAIL_PID
fi

# 等待观测文件创建，确保后续轮询有输入源。
wait_for_file "${ORDERS_CSV}" 20 || { echo "[e2e] orders csv not ready" >&2; exit 1; }
wait_for_file "${POSITIONS_CSV}" 20 || { echo "[e2e] positions csv not ready" >&2; exit 1; }

# 发起订单流：默认 1 秒 1 单，持续 60 秒，随机买卖方向。
: > "${SUBMIT_LOG}"
echo "internal_order_id,security_id,market,side,volume,price" > "${ORDER_IDS_CSV}"
: > "${ORDER_IDS_TXT}"

FIRST_ORDER_ID=""
submitted_count=0
submit_start_sec="${SECONDS}"
submit_deadline_sec=$((submit_start_sec + SUBMIT_DURATION_SEC))
offset=0
next_buy_security_num="${ORDER_START_SECURITY}"
declare -A security_holdings

while :; do
  if [[ -n "${ORDER_COUNT}" ]]; then
    if (( submitted_count >= ORDER_COUNT )); then
      break
    fi
  else
    if (( SECONDS >= submit_deadline_sec )); then
      break
    fi
  fi

  security_id=""
  volume=100
  price="$(printf "10.%02d" $(((offset + 1) % 100)))"
  side="buy"

  can_sell=0
  for sec in "${!security_holdings[@]}"; do
    if (( security_holdings["${sec}"] > 0 )); then
      can_sell=1
      break
    fi
  done

  if [[ "${RANDOM_SIDE}" -eq 1 && "${can_sell}" -eq 1 ]]; then
    if (( RANDOM % 2 == 1 )); then
      side="sell"
    fi
  fi

  if [[ "${side}" == "sell" ]]; then
    sell_candidates=()
    for sec in "${!security_holdings[@]}"; do
      if (( security_holdings["${sec}"] > 0 )); then
        sell_candidates+=("${sec}")
      fi
    done
    if (( ${#sell_candidates[@]} > 0 )); then
      picked_index=$((RANDOM % ${#sell_candidates[@]}))
      security_id="${sell_candidates[${picked_index}]}"
      available_volume=${security_holdings["${security_id}"]}
      if (( available_volume >= 100 )); then
        volume=100
      else
        volume=${available_volume}
      fi
      if (( volume <= 0 )); then
        side="buy"
      fi
    else
      side="buy"
    fi
  fi

  if [[ "${side}" == "buy" ]]; then
    security_id="$(printf "%06d" "${next_buy_security_num}")"
    next_buy_security_num=$((next_buy_security_num + 1))
    volume=$((100 + (offset % 5) * 100))
  fi

  order_id=""
  for _ in $(seq 1 30); do
    if order_id="$("${SUBMIT_BIN}" \
        --upstream-shm "${UPSTREAM_SHM}" \
        --orders-shm "${ORDERS_SHM_BASE}" \
        --trading-day "${TRADING_DAY}" \
        --security "${security_id}" \
        --side "${side}" \
        --market sz \
        --volume "${volume}" \
        --price "${price}" \
        --no-cleanup-shm-on-exit 2>>"${SUBMIT_LOG}")"; then
      order_id="$(echo "${order_id}" | tr -d '\r\n')"
      if [[ "${order_id}" =~ ^[0-9]+$ ]]; then
        break
      fi
    fi
    order_id=""
    sleep 0.2
  done

  if [[ -z "${order_id}" ]]; then
    echo "[e2e] failed to submit order for security=${security_id}" >&2
    exit 1
  fi

  if [[ -z "${FIRST_ORDER_ID}" ]]; then
    FIRST_ORDER_ID="${order_id}"
  fi
  echo "${order_id},${security_id},sz,${side},${volume},${price}" >> "${ORDER_IDS_CSV}"
  echo "${order_id}" >> "${ORDER_IDS_TXT}"
  echo "[submit] order_id=${order_id} security=${security_id} side=${side} volume=${volume} price=${price}" >&2

  if [[ "${side}" == "buy" ]]; then
    prev_volume=${security_holdings["${security_id}"]:-0}
    security_holdings["${security_id}"]=$((prev_volume + volume))
  else
    prev_volume=${security_holdings["${security_id}"]:-0}
    next_volume=$((prev_volume - volume))
    if (( next_volume < 0 )); then
      next_volume=0
    fi
    security_holdings["${security_id}"]=${next_volume}
  fi

  submitted_count=$((submitted_count + 1))
  offset=$((offset + 1))

  if [[ -n "${ORDER_COUNT}" ]]; then
    if (( submitted_count >= ORDER_COUNT )); then
      break
    fi
  else
    if (( SECONDS >= submit_deadline_sec )); then
      break
    fi
  fi
  sleep "${SUBMIT_INTERVAL_SEC}"
done

if (( submitted_count == 0 )); then
  echo "[e2e] no orders submitted" >&2
  exit 1
fi

echo "${FIRST_ORDER_ID}" > "${RUN_DIR}/order_id.txt"

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
      NR>1 {
        if (($4 in want) && ($10+0)>0 && ($11+0)==0) {
          seen[$4]=1
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
      NR==FNR {
        if (NR>1) {
          key = "SZ." $2
          if (!(key in want)) {
            want[key]=1
            total+=1
          }
        }
        next
      }
      NR>1 {
        kind=$2
        key=$3
        gsub(/"/, "", kind)
        gsub(/"/, "", key)
        if (kind=="position" && (key in want) && (($16+0)>0 || ($17+0)>0)) {
          seen[key]=1
        }
      }
      END {
        matched=0
        for (id in seen) {
          matched+=1
        }
        exit(matched==total ? 0 : 1)
      }' "${ORDER_IDS_CSV}" "${POSITIONS_CSV}"; then
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
