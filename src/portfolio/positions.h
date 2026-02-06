#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "common/fixed_string.hpp"

inline constexpr std::size_t kFundPositionIndex = 0;
inline constexpr std::size_t kFirstSecurityPositionIndex = 1;
inline constexpr const char* kFundPositionId = "FUND";

// 统一持仓结构：positions[0]=资金行，positions[1..position_count]=证券行
struct position {
    std::atomic<uint8_t> locked{0};  // 单行锁（股票行用）
    uint64_t available{0};           // 可用资金 (用于 positions[0])
    uint64_t volume_available_t0{0};
    uint64_t volume_available_t1{0};
    uint64_t volume_buy{0};
    uint64_t dvalue_buy{0};
    uint64_t volume_buy_traded{0};
    uint64_t dvalue_buy_traded{0};
    uint64_t volume_sell{0};
    uint64_t dvalue_sell{0};
    uint64_t volume_sell_traded{0};
    uint64_t dvalue_sell_traded{0};
    uint64_t count_order{0};
    fixed_string<16> id{};  // 资金:"FUND", 股票:"000001"等
    fixed_string<16> name{};
};

// 资金结构
struct fund_info {
    uint64_t total_asset{0};   // 总资产 (分)
    uint64_t available{0};     // 可用资金 (分)
    uint64_t frozen{0};        // 冻结资金 (分)
    uint64_t market_value{0};  // 持仓市值 (分)
};

inline uint64_t& fund_total_asset_field(position& fund_row) { return fund_row.volume_available_t0; }
inline const uint64_t& fund_total_asset_field(const position& fund_row) { return fund_row.volume_available_t0; }

inline uint64_t& fund_available_field(position& fund_row) { return fund_row.available; }
inline const uint64_t& fund_available_field(const position& fund_row) { return fund_row.available; }

inline uint64_t& fund_frozen_field(position& fund_row) { return fund_row.volume_available_t1; }
inline const uint64_t& fund_frozen_field(const position& fund_row) { return fund_row.volume_available_t1; }

inline uint64_t& fund_market_value_field(position& fund_row) { return fund_row.volume_buy; }
inline const uint64_t& fund_market_value_field(const position& fund_row) { return fund_row.volume_buy; }

inline fund_info load_fund_info(const position& fund_row) {
    fund_info fund;
    fund.total_asset = fund_total_asset_field(fund_row);
    fund.available = fund_available_field(fund_row);
    fund.frozen = fund_frozen_field(fund_row);
    fund.market_value = fund_market_value_field(fund_row);
    return fund;
}

inline void store_fund_info(position& fund_row, const fund_info& fund) {
    fund_total_asset_field(fund_row) = fund.total_asset;
    fund_available_field(fund_row) = fund.available;
    fund_frozen_field(fund_row) = fund.frozen;
    fund_market_value_field(fund_row) = fund.market_value;
}

// 资金行 atomic 访问（用于 positions[0]）
inline std::atomic<uint64_t> &fund_atomic(uint64_t &field) {
    return *reinterpret_cast<std::atomic<uint64_t> *>(&field);
}

// 单行锁（实盘股票行用，RAII）
struct position_lock {
    std::atomic<uint8_t> &f;
    position_lock(position &p) : f(p.locked) {
        while (f.exchange(1, std::memory_order_acquire)) std::this_thread::yield();
    }
    ~position_lock() { f.store(0, std::memory_order_release); }
    position_lock(const position_lock &) = delete;
    position_lock &operator=(const position_lock &) = delete;
};

inline std::size_t positions_bytes(std::size_t capacity) { return capacity * sizeof(position); }

inline std::size_t positions_capacity(std::size_t file_size) { return file_size / sizeof(position); }
