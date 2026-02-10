#include "risk/risk_checker.hpp"

#include <string>
#include <utility>

namespace acct_service {

namespace {

uint64_t make_order_fingerprint(const order_request& order) {
    return static_cast<uint64_t>(order.internal_order_id);
}

bool is_new_order(const order_request& order) {
    return order.order_type == order_type_t::New;
}

}  // namespace

bool risk_check_result::passed() const noexcept { return code == risk_result_t::Pass; }

risk_check_result risk_check_result::pass() { return risk_check_result{risk_result_t::Pass, "pass"}; }

risk_check_result risk_check_result::reject(risk_result_t code, std::string msg) {
    return risk_check_result{code, std::move(msg)};
}

bool risk_rule::enabled() const noexcept { return enabled_; }

void risk_rule::set_enabled(bool enabled) { enabled_ = enabled; }

const char* fund_check_rule::name() const noexcept { return "fund_check"; }

risk_check_result fund_check_rule::check(const order_request& order, const position_manager& positions) {
    if (!enabled_ || !is_new_order(order) || order.trade_side != trade_side_t::Buy) {
        return risk_check_result::pass();
    }

    const dvalue_t available = positions.get_available_fund();
    const __uint128_t required =
        static_cast<__uint128_t>(order.volume_entrust) * static_cast<__uint128_t>(order.dprice_entrust);
    if (required > static_cast<__uint128_t>(available)) {
        return risk_check_result::reject(risk_result_t::RejectInsufficientFund, "insufficient available fund");
    }

    return risk_check_result::pass();
}

const char* position_check_rule::name() const noexcept { return "position_check"; }

risk_check_result position_check_rule::check(const order_request& order, const position_manager& positions) {
    if (!enabled_ || !is_new_order(order) || order.trade_side != trade_side_t::Sell) {
        return risk_check_result::pass();
    }

    const volume_t sellable = positions.get_sellable_volume(order.internal_security_id);
    if (sellable < order.volume_entrust) {
        return risk_check_result::reject(risk_result_t::RejectInsufficientPosition, "insufficient sellable position");
    }

    return risk_check_result::pass();
}

max_order_value_rule::max_order_value_rule(dvalue_t max_value) : max_value_(max_value) {}

const char* max_order_value_rule::name() const noexcept { return "max_order_value"; }

risk_check_result max_order_value_rule::check(const order_request& order, const position_manager& positions) {
    (void)positions;
    if (!enabled_ || !is_new_order(order) || max_value_ == 0) {
        return risk_check_result::pass();
    }

    const __uint128_t value =
        static_cast<__uint128_t>(order.volume_entrust) * static_cast<__uint128_t>(order.dprice_entrust);
    if (value > static_cast<__uint128_t>(max_value_)) {
        return risk_check_result::reject(risk_result_t::RejectExceedMaxOrderValue, "order value exceeds limit");
    }

    return risk_check_result::pass();
}

void max_order_value_rule::set_max_value(dvalue_t max_value) { max_value_ = max_value; }

max_order_volume_rule::max_order_volume_rule(volume_t max_volume) : max_volume_(max_volume) {}

const char* max_order_volume_rule::name() const noexcept { return "max_order_volume"; }

risk_check_result max_order_volume_rule::check(const order_request& order, const position_manager& positions) {
    (void)positions;
    if (!enabled_ || !is_new_order(order) || max_volume_ == 0) {
        return risk_check_result::pass();
    }

    if (order.volume_entrust > max_volume_) {
        return risk_check_result::reject(risk_result_t::RejectExceedMaxOrderVolume, "order volume exceeds limit");
    }

    return risk_check_result::pass();
}

void max_order_volume_rule::set_max_volume(volume_t max_volume) { max_volume_ = max_volume; }

const char* price_limit_rule::name() const noexcept { return "price_limit"; }

risk_check_result price_limit_rule::check(const order_request& order, const position_manager& positions) {
    (void)positions;
    if (!enabled_ || !is_new_order(order)) {
        return risk_check_result::pass();
    }

    const auto it = limits_.find(order.internal_security_id);
    if (it == limits_.end()) {
        return risk_check_result::pass();
    }

    const dprice_t limit_up = it->second.first;
    const dprice_t limit_down = it->second.second;
    if ((limit_up != 0 && order.dprice_entrust > limit_up) ||
        (limit_down != 0 && order.dprice_entrust < limit_down)) {
        return risk_check_result::reject(risk_result_t::RejectPriceOutOfRange, "price is out of limit range");
    }

    return risk_check_result::pass();
}

void price_limit_rule::set_price_limits(internal_security_id_t security_id, dprice_t limit_up, dprice_t limit_down) {
    limits_[security_id] = std::make_pair(limit_up, limit_down);
}

void price_limit_rule::clear_price_limits() { limits_.clear(); }

const char* duplicate_order_rule::name() const noexcept { return "duplicate_order"; }

risk_check_result duplicate_order_rule::check(const order_request& order, const position_manager& positions) {
    (void)positions;
    if (!enabled_ || !is_new_order(order)) {
        return risk_check_result::pass();
    }

    const timestamp_ns_t now = now_ns();
    const uint64_t key = make_order_fingerprint(order);

    const auto it = recent_orders_.find(key);
    if (it != recent_orders_.end() && (now >= it->second) && (now - it->second) <= time_window_ns_) {
        return risk_check_result::reject(risk_result_t::RejectDuplicateOrder, "duplicate order within time window");
    }

    recent_orders_[key] = now;
    return risk_check_result::pass();
}

void duplicate_order_rule::record_order(const order_request& order) { recent_orders_[make_order_fingerprint(order)] = now_ns(); }

void duplicate_order_rule::clear_history() { recent_orders_.clear(); }

void duplicate_order_rule::set_time_window_ns(timestamp_ns_t window_ns) { time_window_ns_ = window_ns; }

rate_limit_rule::rate_limit_rule(uint32_t max_orders_per_second) : max_orders_per_second_(max_orders_per_second) {}

const char* rate_limit_rule::name() const noexcept { return "rate_limit"; }

risk_check_result rate_limit_rule::check(const order_request& order, const position_manager& positions) {
    (void)positions;
    if (!enabled_ || !is_new_order(order) || max_orders_per_second_ == 0) {
        return risk_check_result::pass();
    }

    constexpr timestamp_ns_t kSecondNs = 1000000000ULL;
    const timestamp_ns_t now = now_ns();
    const timestamp_ns_t window_start = current_second_start_.load(std::memory_order_relaxed);

    if (window_start == 0 || now < window_start || (now - window_start) >= kSecondNs) {
        current_second_start_.store(now, std::memory_order_relaxed);
        current_second_count_.store(0, std::memory_order_relaxed);
    }

    const uint32_t count = current_second_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count > max_orders_per_second_) {
        return risk_check_result::reject(risk_result_t::RejectUnknown, "order rate exceeds limit");
    }

    return risk_check_result::pass();
}

void rate_limit_rule::set_max_orders_per_second(uint32_t max) { max_orders_per_second_ = max; }

void rate_limit_rule::reset_counter() {
    current_second_count_.store(0, std::memory_order_relaxed);
    current_second_start_.store(0, std::memory_order_relaxed);
}

}  // namespace acct_service
