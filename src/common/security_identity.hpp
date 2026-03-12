#pragma once

#include <array>
#include <string_view>

#include "common/types.hpp"
#include "order/order_request.hpp"

namespace acct_service {

// 返回 market 对应的 MIC 前缀，供内部证券键统一编码使用。
inline std::string_view market_to_mic(Market market) noexcept {
    switch (market) {
        case Market::SZ:
            return "XSHE";
        case Market::SH:
            return "XSHG";
        case Market::BJ:
            return "BJSE";
        case Market::HK:
            return "XHKG";
        default:
            return {};
    }
}

// 解析旧格式前缀，兼容历史持久化数据中的 `SZ.000001` / `SH.600000`。
inline bool parse_legacy_market_prefix(std::string_view prefix, Market& market) noexcept {
    if (prefix == "SZ") {
        market = Market::SZ;
        return true;
    }
    if (prefix == "SH") {
        market = Market::SH;
        return true;
    }
    if (prefix == "BJ") {
        market = Market::BJ;
        return true;
    }
    if (prefix == "HK") {
        market = Market::HK;
        return true;
    }
    return false;
}

// 解析 MIC 前缀，供运行期识别新的标准内部证券键。
inline bool parse_mic_market_prefix(std::string_view prefix, Market& market) noexcept {
    if (prefix == "XSHE") {
        market = Market::SZ;
        return true;
    }
    if (prefix == "XSHG") {
        market = Market::SH;
        return true;
    }
    if (prefix == "BJSE") {
        market = Market::BJ;
        return true;
    }
    if (prefix == "XHKG") {
        market = Market::HK;
        return true;
    }
    return false;
}

// 按指定分隔符切开内部证券键，便于兼容旧格式和新格式。
inline bool split_internal_security_id(std::string_view internal_id, char delimiter, std::string_view& prefix,
                                       std::string_view& code) noexcept {
    const std::size_t separator = internal_id.find(delimiter);
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= internal_id.size()) {
        return false;
    }

    prefix = internal_id.substr(0, separator);
    code = internal_id.substr(separator + 1);
    return true;
}

// 把内部证券键解析成 market + code，同时兼容历史点号格式和新的 MIC 格式。
inline bool parse_internal_security_id(std::string_view internal_id, Market& market, std::string_view& code) noexcept {
    std::string_view prefix;
    if (split_internal_security_id(internal_id, '_', prefix, code)) {
        return !code.empty() && parse_mic_market_prefix(prefix, market);
    }
    if (split_internal_security_id(internal_id, '.', prefix, code)) {
        return !code.empty() && parse_legacy_market_prefix(prefix, market);
    }
    return false;
}

// 构造 canonical internal_security_id，格式固定为 "<MIC>_<security_id>"。
inline bool build_internal_security_id(Market market, std::string_view security_id, InternalSecurityId& out_id) {
    const std::string_view prefix = market_to_mic(market);
    if (prefix.empty()) {
        return false;
    }

    // 约束 security_id 长度，避免 MIC 前缀写入 16 字节定长字段时发生截断。
    constexpr std::size_t kInternalSecurityIdCapacity = kInternalSecurityIdSize - 1U;
    if (security_id.empty() || security_id.size() > (kInternalSecurityIdCapacity - prefix.size() - 1U)) {
        return false;
    }

    std::array<char, kInternalSecurityIdCapacity + 1U> buffer{};
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        buffer[i] = prefix[i];
    }
    buffer[prefix.size()] = '_';
    for (std::size_t i = 0; i < security_id.size(); ++i) {
        buffer[prefix.size() + 1U + i] = security_id[i];
    }

    out_id.assign(std::string_view(buffer.data(), prefix.size() + 1U + security_id.size()));
    return true;
}

// 把任意支持的内部证券键写回 canonical MIC 格式，供运行期统一落库和落 SHM。
inline bool normalize_internal_security_id(std::string_view internal_id, InternalSecurityId& out_id) {
    Market market = Market::NotSet;
    std::string_view code;
    if (!parse_internal_security_id(internal_id, market, code)) {
        return false;
    }
    return build_internal_security_id(market, code, out_id);
}

}  // namespace acct_service
