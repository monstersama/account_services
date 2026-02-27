#pragma once

#include <array>
#include <string_view>

#include "common/types.hpp"
#include "order/order_request.hpp"

namespace acct_service {

// 将 market 枚举转换为内部证券键前缀（大写）。
inline std::string_view market_to_prefix(market_t market) noexcept {
    switch (market) {
        case market_t::SZ:
            return "SZ";
        case market_t::SH:
            return "SH";
        case market_t::BJ:
            return "BJ";
        case market_t::HK:
            return "HK";
        default:
            return {};
    }
}

// 构造 internal_security_id，格式固定为 "<MARKET>.<security_id>"。
inline bool build_internal_security_id(market_t market, std::string_view security_id, internal_security_id_t& out_id) {
    // 约束 security_id 最大长度，避免 16 字节定长字段截断。
    if (security_id.empty() || security_id.size() > 12) {
        return false;
    }

    const std::string_view prefix = market_to_prefix(market);
    if (prefix.empty()) {
        return false;
    }

    std::array<char, 16> buffer{};
    buffer[0] = prefix[0];
    buffer[1] = prefix[1];
    buffer[2] = '.';
    for (std::size_t i = 0; i < security_id.size(); ++i) {
        buffer[3 + i] = security_id[i];
    }

    out_id.assign(std::string_view(buffer.data(), 3 + security_id.size()));
    return true;
}

}  // namespace acct_service
