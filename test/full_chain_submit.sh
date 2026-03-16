#!/usr/bin/env bash
set -euo pipefail

# 仅负责批量发单，不启动 service/gateway/observer。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${1:-${SOURCE_DIR}/build}"
BUILD_DIR="$(cd "${BUILD_DIR}" && pwd)"

RUN_ID="$(date +%Y%m%d_%H%M%S)_$$"
RUN_DIR="${RUN_DIR:-${BUILD_DIR}/e2e_artifacts/submit_${RUN_ID}}"
mkdir -p "${RUN_DIR}"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"

SUBMIT_BIN="${BUILD_DIR}/tools/full_chain_e2e/order_submit_cli"

SAMPLE_DB_PATH="${SAMPLE_DB_PATH:-${SOURCE_DIR}/data/account_service.db}"
EXTRA_SECURITY_IDS="${EXTRA_SECURITY_IDS:-990001,990002}"
TRADING_DAY="${TRADING_DAY:-19700101}"
UPSTREAM_SHM="${UPSTREAM_SHM:-/upstream_order_shm}"
ORDERS_SHM_BASE="${ORDERS_SHM_BASE:-/orders_shm}"
ORDER_COUNT="${ORDER_COUNT:-100}"
SUBMIT_INTERVAL_SEC="${SUBMIT_INTERVAL_SEC:-0.1}"
SUBMIT_DURATION_SEC="${SUBMIT_DURATION_SEC:-60}"
RANDOM_SIDE="${RANDOM_SIDE:-1}"
VALID_SEC="${VALID_SEC:-0}"
PASSIVE_EXEC_ALGO="${PASSIVE_EXEC_ALGO:-default}"

SUBMIT_LOG="${SUBMIT_LOG:-${RUN_DIR}/order_submit.stderr.log}"
ORDER_IDS_CSV="${ORDER_IDS_CSV:-${RUN_DIR}/order_ids.csv}"
ORDER_IDS_TXT="${ORDER_IDS_TXT:-${RUN_DIR}/order_ids.txt}"
FIRST_ORDER_ID_OUT="${FIRST_ORDER_ID_OUT:-}"

# 统一校验发单参数，避免运行中报错中断。
if [[ ! -x "${SUBMIT_BIN}" ]]; then
  echo "[submit_only] missing binary: ${SUBMIT_BIN}" >&2
  exit 1
fi
if [[ ! -f "${SAMPLE_DB_PATH}" ]]; then
  echo "[submit_only] missing sample db: ${SAMPLE_DB_PATH}" >&2
  exit 1
fi
if [[ -n "${ORDER_COUNT}" ]] && ! [[ "${ORDER_COUNT}" =~ ^[1-9][0-9]*$ ]]; then
  echo "[submit_only] invalid ORDER_COUNT: ${ORDER_COUNT}" >&2
  exit 1
fi
if ! awk -v value="${SUBMIT_INTERVAL_SEC}" '
    BEGIN {
      is_number = (value ~ /^([0-9]+([.][0-9]+)?|[.][0-9]+)$/)
      exit((is_number && (value + 0) > 0) ? 0 : 1)
    }'; then
  echo "[submit_only] invalid SUBMIT_INTERVAL_SEC: ${SUBMIT_INTERVAL_SEC}" >&2
  exit 1
fi
if ! [[ "${SUBMIT_DURATION_SEC}" =~ ^[1-9][0-9]*$ ]]; then
  echo "[submit_only] invalid SUBMIT_DURATION_SEC: ${SUBMIT_DURATION_SEC}" >&2
  exit 1
fi
if ! [[ "${RANDOM_SIDE}" =~ ^[01]$ ]]; then
  echo "[submit_only] invalid RANDOM_SIDE: ${RANDOM_SIDE}" >&2
  exit 1
fi
if ! [[ "${VALID_SEC}" =~ ^[0-9]+$ ]]; then
  echo "[submit_only] invalid VALID_SEC: ${VALID_SEC}" >&2
  exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "[submit_only] python3 is required to load security universe from db" >&2
  exit 1
fi

mkdir -p "$(dirname "${SUBMIT_LOG}")" "$(dirname "${ORDER_IDS_CSV}")" "$(dirname "${ORDER_IDS_TXT}")"
if [[ -n "${FIRST_ORDER_ID_OUT}" ]]; then
  mkdir -p "$(dirname "${FIRST_ORDER_ID_OUT}")"
fi

# 从样本 DB 抽取可交易标的，作为默认发单池。
DB_SECURITY_ROWS=()
while IFS= read -r row; do
  [[ -n "${row}" ]] && DB_SECURITY_ROWS+=("${row}")
done < <(python3 - "${SAMPLE_DB_PATH}" <<'PY'
import sqlite3
import sys

db_path = sys.argv[1]
conn = sqlite3.connect(db_path)
cur = conn.cursor()

cur.execute("""
SELECT security_id, internal_security_id, volume_available_t0
FROM positions
ORDER BY internal_security_id, ID
""")

seen = set()
for security_id, internal_security_id, volume_available_t0 in cur.fetchall():
    internal = (internal_security_id or "").strip()
    if "." not in internal:
        continue
    market = internal.split(".", 1)[0].lower()
    if market not in {"sz", "sh", "bj", "hk"}:
        continue
    security = (security_id or "").strip()
    if not security:
        continue
    key = f"{market}|{security}"
    if key in seen:
        continue
    seen.add(key)
    print(f"{market}|{security}|{int(volume_available_t0 or 0)}")
conn.close()
PY
)

if (( ${#DB_SECURITY_ROWS[@]} == 0 )); then
  echo "[submit_only] no sz/sh/bj/hk securities found in sample db: ${SAMPLE_DB_PATH}" >&2
  exit 1
fi

DB_ORDER_KEYS=()
DB_SECURITY_T0_VOLUMES=()
declare -A DB_SECURITY_ID_SET
for row in "${DB_SECURITY_ROWS[@]}"; do
  market="${row%%|*}"
  rest="${row#*|}"
  security_id="${rest%%|*}"
  t0_volume="${rest##*|}"
  if ! [[ "${market}" =~ ^(sz|sh|bj|hk)$ ]]; then
    echo "[submit_only] invalid market in db row: ${row}" >&2
    exit 1
  fi
  if [[ -z "${security_id}" || ${#security_id} -gt 12 ]]; then
    echo "[submit_only] invalid security_id in db for market=${market}: ${security_id}" >&2
    exit 1
  fi
  if ! [[ "${t0_volume}" =~ ^[0-9]+$ ]]; then
    echo "[submit_only] invalid volume_available_t0 in db for ${market}.${security_id}: ${t0_volume}" >&2
    exit 1
  fi
  order_key="${market}|${security_id}"
  DB_ORDER_KEYS+=("${order_key}")
  DB_SECURITY_T0_VOLUMES+=("${t0_volume}")
  DB_SECURITY_ID_SET["${security_id}"]=1
done

IFS=',' read -r -a EXTRA_SECURITIES <<< "${EXTRA_SECURITY_IDS}"
if (( ${#EXTRA_SECURITIES[@]} != 2 )); then
  echo "[submit_only] EXTRA_SECURITY_IDS must contain exactly 2 ids (comma separated): ${EXTRA_SECURITY_IDS}" >&2
  exit 1
fi
EXTRA_ORDER_KEYS=()
declare -A EXTRA_SECURITY_SET
for i in "${!EXTRA_SECURITIES[@]}"; do
  sec="$(echo "${EXTRA_SECURITIES[$i]}" | tr -d '[:space:]')"
  EXTRA_SECURITIES["${i}"]="${sec}"
  if [[ -z "${sec}" || ${#sec} -gt 12 ]]; then
    echo "[submit_only] invalid extra security_id: ${sec}" >&2
    exit 1
  fi
  if [[ -n "${DB_SECURITY_ID_SET["${sec}"]:-}" ]]; then
    echo "[submit_only] extra security_id must not exist in db: ${sec}" >&2
    exit 1
  fi
  if [[ -n "${EXTRA_SECURITY_SET["${sec}"]:-}" ]]; then
    echo "[submit_only] duplicated extra security_id: ${sec}" >&2
    exit 1
  fi
  EXTRA_SECURITY_SET["${sec}"]=1
  EXTRA_ORDER_KEYS+=("sz|${sec}")
done

ORDER_SECURITY_POOL=("${DB_ORDER_KEYS[@]}" "${EXTRA_ORDER_KEYS[@]}")
ORDER_SECURITY_POOL_SIZE=${#ORDER_SECURITY_POOL[@]}
echo "[submit_only] db securities (market|security): ${DB_ORDER_KEYS[*]}" >&2
echo "[submit_only] extra non-db securities (market|security): ${EXTRA_ORDER_KEYS[*]}" >&2

# 发起订单流：支持固定笔数或固定时长两种模式。
: > "${SUBMIT_LOG}"
echo "internal_order_id,security_id,market,side,volume,price,valid_sec,passive_exec_algo" > "${ORDER_IDS_CSV}"
: > "${ORDER_IDS_TXT}"

first_order_id=""
submitted_count=0
submit_start_sec="${SECONDS}"
submit_deadline_sec=$((submit_start_sec + SUBMIT_DURATION_SEC))
offset=0
declare -A security_holdings
for i in "${!DB_ORDER_KEYS[@]}"; do
  security_holdings["${DB_ORDER_KEYS[$i]}"]="${DB_SECURITY_T0_VOLUMES[$i]}"
done

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

  security_key=""
  security_id=""
  security_market=""
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
      security_key="${sell_candidates[${picked_index}]}"
      available_volume=${security_holdings["${security_key}"]}
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
    security_key="${ORDER_SECURITY_POOL[$((offset % ORDER_SECURITY_POOL_SIZE))]}"
    volume=$((100 + (offset % 5) * 100))
  fi

  security_market="${security_key%%|*}"
  security_id="${security_key#*|}"
  if [[ -z "${security_market}" || -z "${security_id}" || "${security_market}" == "${security_id}" ]]; then
    echo "[submit_only] invalid security key resolved: ${security_key}" >&2
    exit 1
  fi

  order_id=""
  for _ in $(seq 1 30); do
    if order_id="$("${SUBMIT_BIN}" \
      --upstream-shm "${UPSTREAM_SHM}" \
      --orders-shm "${ORDERS_SHM_BASE}" \
      --trading-day "${TRADING_DAY}" \
      --security "${security_id}" \
      --side "${side}" \
      --market "${security_market}" \
      --volume "${volume}" \
      --price "${price}" \
      --valid-sec "${VALID_SEC}" \
      --passive-exec-algo "${PASSIVE_EXEC_ALGO}" 2>>"${SUBMIT_LOG}")"; then
      order_id="$(echo "${order_id}" | tr -d '\r\n')"
      if [[ "${order_id}" =~ ^[0-9]+$ ]]; then
        break
      fi
    fi
    order_id=""
    sleep 0.2
  done

  if [[ -z "${order_id}" ]]; then
    echo "[submit_only] failed to submit order for market=${security_market} security=${security_id}" >&2
    exit 1
  fi

  if [[ -z "${first_order_id}" ]]; then
    first_order_id="${order_id}"
  fi
  echo "${order_id},${security_id},${security_market},${side},${volume},${price},${VALID_SEC},${PASSIVE_EXEC_ALGO}" >> "${ORDER_IDS_CSV}"
  echo "${order_id}" >> "${ORDER_IDS_TXT}"
  echo "[submit_only] order_id=${order_id} market=${security_market} security=${security_id} side=${side} volume=${volume} price=${price} valid_sec=${VALID_SEC} passive_exec_algo=${PASSIVE_EXEC_ALGO}" >&2

  if [[ "${side}" == "buy" ]]; then
    prev_volume=${security_holdings["${security_key}"]:-0}
    security_holdings["${security_key}"]=$((prev_volume + volume))
  else
    prev_volume=${security_holdings["${security_key}"]:-0}
    next_volume=$((prev_volume - volume))
    if (( next_volume < 0 )); then
      next_volume=0
    fi
    security_holdings["${security_key}"]=${next_volume}
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
  echo "[submit_only] no orders submitted" >&2
  exit 1
fi

if [[ -n "${FIRST_ORDER_ID_OUT}" ]]; then
  echo "${first_order_id}" > "${FIRST_ORDER_ID_OUT}"
fi

echo "[submit_only] PASS order_count=${submitted_count}" >&2
