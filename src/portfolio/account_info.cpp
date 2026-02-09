#include "portfolio/account_info.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <string>

namespace acct_service {

namespace {

std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };

    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool parse_bool(std::string value, bool& out) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_u32(const std::string& value, uint32_t& out) {
    try {
        const unsigned long long parsed = std::stoull(trim_copy(value));
        if (parsed > static_cast<unsigned long long>(UINT32_MAX)) {
            return false;
        }
        out = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u64(const std::string& value, uint64_t& out) {
    try {
        out = static_cast<uint64_t>(std::stoull(trim_copy(value)));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double(const std::string& value, double& out) {
    try {
        out = std::stod(trim_copy(value));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_account_type(std::string value, account_type_t& out) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "1" || value == "stock") {
        out = account_type_t::Stock;
        return true;
    }
    if (value == "2" || value == "futures") {
        out = account_type_t::Futures;
        return true;
    }
    if (value == "3" || value == "option") {
        out = account_type_t::Option;
        return true;
    }
    return false;
}

bool apply_key(account_info& info, const std::string& key, const std::string& raw_value) {
    const std::string value = trim_copy(raw_value);

    if (key == "account.account_id") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            return false;
        }
        info.account_id = static_cast<account_id_t>(parsed);
        return true;
    }
    if (key == "account.account_type") {
        return parse_account_type(value, info.account_type);
    }
    if (key == "account.account_name") {
        info.account_name.assign(value);
        return true;
    }
    if (key == "account.broker_account") {
        info.broker_account.assign(value);
        return true;
    }
    if (key == "account.broker_code") {
        info.broker_code.assign(value);
        return true;
    }

    if (key == "account.can_buy") {
        return parse_bool(value, info.can_buy);
    }
    if (key == "account.can_sell") {
        return parse_bool(value, info.can_sell);
    }
    if (key == "account.can_short") {
        return parse_bool(value, info.can_short);
    }
    if (key == "account.can_margin") {
        return parse_bool(value, info.can_margin);
    }

    if (key == "account.commission_rate") {
        return parse_double(value, info.commission_rate);
    }
    if (key == "account.stamp_tax_rate") {
        return parse_double(value, info.stamp_tax_rate);
    }
    if (key == "account.transfer_fee_rate") {
        return parse_double(value, info.transfer_fee_rate);
    }
    if (key == "account.min_commission") {
        uint64_t parsed = 0;
        if (!parse_u64(value, parsed)) {
            return false;
        }
        info.min_commission = static_cast<dvalue_t>(parsed);
        return true;
    }

    if (key == "account.max_single_order") {
        uint64_t parsed = 0;
        if (!parse_u64(value, parsed)) {
            return false;
        }
        info.max_single_order = static_cast<dvalue_t>(parsed);
        return true;
    }
    if (key == "account.max_daily_amount") {
        uint64_t parsed = 0;
        if (!parse_u64(value, parsed)) {
            return false;
        }
        info.max_daily_amount = static_cast<dvalue_t>(parsed);
        return true;
    }

    return true;
}

}  // namespace

dvalue_t account_info::calculate_fee(trade_side_t side, dvalue_t traded_value) const {
    const double traded = static_cast<double>(traded_value);

    dvalue_t commission = static_cast<dvalue_t>(traded * commission_rate + 0.5);
    if (commission < min_commission) {
        commission = min_commission;
    }

    const dvalue_t transfer_fee = static_cast<dvalue_t>(traded * transfer_fee_rate + 0.5);
    const dvalue_t stamp_tax =
        (side == trade_side_t::Sell) ? static_cast<dvalue_t>(traded * stamp_tax_rate + 0.5) : 0;

    return commission + transfer_fee + stamp_tax;
}

bool account_info_manager::load_from_config(const std::string& config_path) {
    std::ifstream in(config_path);
    if (!in.is_open()) {
        return false;
    }

    account_info loaded = info_;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        line = trim_copy(std::move(line));
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = trim_copy(line.substr(1, line.size() - 2));
            continue;
        }

        const std::size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos) {
            continue;
        }

        std::string key = trim_copy(line.substr(0, equal_pos));
        std::string value = trim_copy(line.substr(equal_pos + 1));

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        const std::string full_key = section.empty() ? key : section + "." + key;
        if (!apply_key(loaded, full_key, value)) {
            return false;
        }
    }

    loaded.state = account_state_t::Ready;
    info_ = loaded;
    return true;
}

bool account_info_manager::load_from_db(const std::string& db_path, account_id_t account_id) {
    (void)db_path;
    info_.account_id = account_id;
    info_.state = account_state_t::Ready;
    return true;
}

const account_info& account_info_manager::info() const noexcept { return info_; }

account_info& account_info_manager::info() noexcept { return info_; }

bool account_info_manager::can_trade(trade_side_t side) const noexcept {
    switch (side) {
        case trade_side_t::Buy:
            return info_.can_buy;
        case trade_side_t::Sell:
            return info_.can_sell;
        default:
            return false;
    }
}

void account_info_manager::set_state(account_state_t state) { info_.state = state; }

}  // namespace acct_service
