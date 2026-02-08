#include "risk/risk_manager.hpp"

#include <memory>
#include <string_view>
#include <utility>

namespace acct_service {

void risk_stats::reset() {
    total_checks = 0;
    passed = 0;
    rejected = 0;
    rejected_fund = 0;
    rejected_position = 0;
    rejected_price = 0;
    rejected_value = 0;
    rejected_volume = 0;
    rejected_duplicate = 0;
    rejected_rate_limit = 0;
    last_check_time = 0;
}

risk_manager::risk_manager(position_manager& positions, const risk_config& config) : positions_(positions), config_(config) {
    initialize_default_rules();
}

risk_check_result risk_manager::check_order(const order_request& order) {
    risk_check_result result = risk_check_result::pass();

    for (const auto& rule : rules_) {
        if (!rule || !rule->enabled()) {
            continue;
        }

        result = rule->check(order, positions_);
        if (!result.passed()) {
            break;
        }
    }

    update_stats(result);
    if (post_check_callback_) {
        post_check_callback_(order, result);
    }
    return result;
}

std::vector<risk_check_result> risk_manager::check_orders(const std::vector<order_request>& orders) {
    std::vector<risk_check_result> results;
    results.reserve(orders.size());

    for (const order_request& order : orders) {
        results.push_back(check_order(order));
    }

    return results;
}

void risk_manager::set_post_check_callback(post_check_callback_t callback) { post_check_callback_ = std::move(callback); }

void risk_manager::add_rule(std::unique_ptr<risk_rule> rule) {
    if (!rule) {
        return;
    }

    if (auto* as_price_limit = dynamic_cast<price_limit_rule*>(rule.get())) {
        price_limit_rule_ = as_price_limit;
    }
    if (auto* as_duplicate = dynamic_cast<duplicate_order_rule*>(rule.get())) {
        duplicate_rule_ = as_duplicate;
    }
    if (auto* as_rate_limit = dynamic_cast<rate_limit_rule*>(rule.get())) {
        rate_limit_rule_ = as_rate_limit;
    }

    rules_.push_back(std::move(rule));
}

bool risk_manager::remove_rule(const char* name) {
    if (!name) {
        return false;
    }

    const std::string_view target(name);
    for (auto it = rules_.begin(); it != rules_.end(); ++it) {
        if (*it && std::string_view((*it)->name()) == target) {
            if (it->get() == price_limit_rule_) {
                price_limit_rule_ = nullptr;
            }
            if (it->get() == duplicate_rule_) {
                duplicate_rule_ = nullptr;
            }
            if (it->get() == rate_limit_rule_) {
                rate_limit_rule_ = nullptr;
            }
            rules_.erase(it);
            return true;
        }
    }

    return false;
}

bool risk_manager::enable_rule(const char* name, bool enabled) {
    risk_rule* rule = get_rule(name);
    if (!rule) {
        return false;
    }
    rule->set_enabled(enabled);
    return true;
}

risk_rule* risk_manager::get_rule(const char* name) {
    if (!name) {
        return nullptr;
    }

    const std::string_view target(name);
    for (const auto& rule : rules_) {
        if (rule && std::string_view(rule->name()) == target) {
            return rule.get();
        }
    }

    return nullptr;
}

const risk_rule* risk_manager::get_rule(const char* name) const {
    if (!name) {
        return nullptr;
    }

    const std::string_view target(name);
    for (const auto& rule : rules_) {
        if (rule && std::string_view(rule->name()) == target) {
            return rule.get();
        }
    }

    return nullptr;
}

void risk_manager::update_price_limits(internal_security_id_t security_id, dprice_t limit_up, dprice_t limit_down) {
    if (price_limit_rule_) {
        price_limit_rule_->set_price_limits(security_id, limit_up, limit_down);
    }
}

void risk_manager::clear_price_limits() {
    if (price_limit_rule_) {
        price_limit_rule_->clear_price_limits();
    }
}

void risk_manager::update_config(const risk_config& config) {
    config_ = config;
    initialize_default_rules();
}

const risk_config& risk_manager::config() const noexcept { return config_; }

const risk_stats& risk_manager::stats() const noexcept { return stats_; }

void risk_manager::reset_stats() noexcept { stats_.reset(); }

void risk_manager::initialize_default_rules() {
    rules_.clear();
    price_limit_rule_ = nullptr;
    duplicate_rule_ = nullptr;
    rate_limit_rule_ = nullptr;

    if (config_.enable_fund_check) {
        add_rule(std::make_unique<fund_check_rule>());
    }

    if (config_.enable_position_check) {
        add_rule(std::make_unique<position_check_rule>());
    }

    if (config_.max_order_value > 0) {
        add_rule(std::make_unique<max_order_value_rule>(config_.max_order_value));
    }

    if (config_.max_order_volume > 0) {
        add_rule(std::make_unique<max_order_volume_rule>(config_.max_order_volume));
    }

    if (config_.enable_price_limit_check) {
        add_rule(std::make_unique<price_limit_rule>());
    }

    if (config_.enable_duplicate_check) {
        auto duplicate = std::make_unique<duplicate_order_rule>();
        duplicate->set_time_window_ns(config_.duplicate_window_ns);
        add_rule(std::move(duplicate));
    }

    if (config_.max_orders_per_second > 0) {
        add_rule(std::make_unique<rate_limit_rule>(config_.max_orders_per_second));
    }
}

void risk_manager::update_stats(const risk_check_result& result) {
    ++stats_.total_checks;
    stats_.last_check_time = now_ns();

    if (result.passed()) {
        ++stats_.passed;
        return;
    }

    ++stats_.rejected;
    switch (result.code) {
        case risk_result_t::RejectInsufficientFund:
            ++stats_.rejected_fund;
            break;
        case risk_result_t::RejectInsufficientPosition:
            ++stats_.rejected_position;
            break;
        case risk_result_t::RejectPriceOutOfRange:
            ++stats_.rejected_price;
            break;
        case risk_result_t::RejectExceedMaxOrderValue:
            ++stats_.rejected_value;
            break;
        case risk_result_t::RejectExceedMaxOrderVolume:
            ++stats_.rejected_volume;
            break;
        case risk_result_t::RejectDuplicateOrder:
            ++stats_.rejected_duplicate;
            break;
        default:
            ++stats_.rejected_rate_limit;
            break;
    }
}

}  // namespace acct_service
