#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>

namespace acct_service {

using internal_order_id_t = uint32_t;
using internal_security_id_t = uint16_t;
using volume_t = uint64_t;
using dprice_t = uint64_t;   // 价格，单位：分 (2位小数精度)
using dvalue_t = uint64_t;   // 金额，单位：分
using md_time_t = uint32_t;  // 具体时间（毫秒）
using seconds_t = uint32_t;  // 时间（秒），非具体时间点

using account_id_t = uint32_t;
using strategy_id_t = uint16_t;
using sequence_t = uint64_t;
using timestamp_ns_t = uint64_t;  // Unix Epoch 纳秒时间戳

// ========== 枚举类型 ==========

// 风控结果码
enum class risk_result_t : uint8_t {
    Pass = 0,
    RejectInsufficientFund = 1,
    RejectInsufficientPosition = 2,
    RejectExceedMaxOrderValue = 3,
    RejectExceedMaxOrderVolume = 4,
    RejectExceedDailyLimit = 5,
    RejectPriceOutOfRange = 6,
    RejectSecurityNotAllowed = 7,
    RejectAccountFrozen = 8,
    RejectDuplicateOrder = 9,
    RejectUnknown = 0xFF,
};

// 账户状态
enum class account_state_t : uint8_t {
    Initializing = 0,
    Ready = 1,
    Trading = 2,
    Suspended = 3,
    Closed = 4,
};

// 账户类型
enum class account_type_t : uint8_t {
    Stock = 1,
    Futures = 2,
    Option = 3,
};

// 拆单策略
enum class split_strategy_t : uint8_t {
    None = 0,
    FixedSize = 1,
    TWAP = 2,
    VWAP = 3,
    Iceberg = 4,
};

// 持仓变动类型
enum class position_change_t : uint8_t {
    BuyEntrust = 1,
    BuyTraded = 2,
    BuyCancelled = 3,
    SellEntrust = 4,
    SellTraded = 5,
    SellCancelled = 6,
};

// ========== 工具函数 ==========

// 获取当前 Unix Epoch 纳秒时间戳
inline timestamp_ns_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<timestamp_ns_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

// 获取当前单调时钟纳秒值（适合做耗时统计/超时判断）
inline timestamp_ns_t now_monotonic_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<timestamp_ns_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

}  // namespace acct_service
