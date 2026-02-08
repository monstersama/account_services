#include "order/order_splitter.hpp"

#include <algorithm>
#include <utility>

namespace acct_service {

namespace {

order_request make_child_request(
    const order_request& parent, internal_order_id_t child_id, volume_t child_volume) {
    order_request child = parent;
    child.internal_order_id = child_id;
    child.volume_entrust = child_volume;
    child.volume_remain = child_volume;
    child.volume_traded = 0;
    child.dvalue_traded = 0;
    child.dprice_traded = 0;
    child.dfee_estimate = 0;
    child.dfee_executed = 0;
    child.md_time_traded_first = 0;
    child.md_time_traded_latest = 0;
    child.md_time_broker_response = 0;
    child.md_time_market_response = 0;
    child.broker_order_id.as_uint = 0;
    child.orig_internal_order_id = 0;
    return child;
}

}  // namespace

order_splitter::order_splitter(const split_config& config) : config_(config) {}

void order_splitter::set_order_id_generator(order_id_generator_t gen) { id_generator_ = std::move(gen); }

split_result order_splitter::split(const order_request& parent_order) {
    if (!should_split(parent_order)) {
        return split_result{true, {}, {}};
    }

    switch (config_.strategy) {
        case split_strategy_t::FixedSize:
            return split_fixed_size(parent_order);
        case split_strategy_t::Iceberg:
            return split_iceberg(parent_order);
        case split_strategy_t::TWAP:
            return split_twap(parent_order);
        default:
            return split_result{false, {}, "unsupported split strategy"};
    }
}

bool order_splitter::should_split(const order_request& order) const {
    if (order.order_type != order_type_t::New) {
        return false;
    }
    if (config_.strategy == split_strategy_t::None) {
        return false;
    }
    if (config_.max_child_volume == 0) {
        return false;
    }
    return order.volume_entrust > config_.max_child_volume;
}

void order_splitter::update_config(const split_config& config) { config_ = config; }

const split_config& order_splitter::config() const noexcept { return config_; }

split_result order_splitter::split_fixed_size(const order_request& parent) {
    if (!id_generator_) {
        return split_result{false, {}, "order id generator is not set"};
    }
    if (config_.max_child_volume == 0) {
        return split_result{false, {}, "max_child_volume is zero"};
    }

    split_result result;
    result.success = true;

    volume_t remaining = parent.volume_entrust;
    while (remaining > 0) {
        if (result.child_orders.size() >= config_.max_child_count) {
            result.success = false;
            result.error_msg = "child count exceeds max_child_count";
            result.child_orders.clear();
            return result;
        }

        volume_t child_volume = std::min(remaining, config_.max_child_volume);
        if (child_volume == 0) {
            result.success = false;
            result.error_msg = "invalid child volume";
            result.child_orders.clear();
            return result;
        }

        if (remaining > child_volume && config_.min_child_volume > 0 && child_volume < config_.min_child_volume &&
            !result.child_orders.empty()) {
            // 将过小余量并入最后一个子单，避免中间子单低于最小拆单数量。
            order_request& last = result.child_orders.back();
            last.volume_entrust += child_volume;
            last.volume_remain += child_volume;
            remaining -= child_volume;
            continue;
        }

        const internal_order_id_t child_id = id_generator_();
        if (child_id == 0) {
            result.success = false;
            result.error_msg = "generated child order id is zero";
            result.child_orders.clear();
            return result;
        }

        result.child_orders.push_back(make_child_request(parent, child_id, child_volume));
        remaining -= child_volume;
    }

    return result;
}

split_result order_splitter::split_iceberg(const order_request& parent) {
    return split_fixed_size(parent);
}

split_result order_splitter::split_twap(const order_request& parent) {
    if (!id_generator_) {
        return split_result{false, {}, "order id generator is not set"};
    }
    if (config_.max_child_count == 0) {
        return split_result{false, {}, "max_child_count is zero"};
    }

    const volume_t total_volume = parent.volume_entrust;
    if (total_volume == 0) {
        return split_result{false, {}, "parent volume is zero"};
    }

    volume_t target = config_.max_child_volume;
    if (target == 0) {
        target = std::max<volume_t>(config_.min_child_volume, 1);
    }

    std::size_t child_count = static_cast<std::size_t>((total_volume + target - 1) / target);
    if (child_count == 0) {
        child_count = 1;
    }
    child_count = std::min<std::size_t>(child_count, config_.max_child_count);

    split_result result;
    result.success = true;
    result.child_orders.reserve(child_count);

    const volume_t base = total_volume / child_count;
    volume_t remainder = total_volume % child_count;

    for (std::size_t i = 0; i < child_count; ++i) {
        volume_t child_volume = base;
        if (remainder > 0) {
            ++child_volume;
            --remainder;
        }

        if (child_volume == 0) {
            continue;
        }

        const internal_order_id_t child_id = id_generator_();
        if (child_id == 0) {
            result.success = false;
            result.error_msg = "generated child order id is zero";
            result.child_orders.clear();
            return result;
        }

        result.child_orders.push_back(make_child_request(parent, child_id, child_volume));
    }

    if (result.child_orders.empty()) {
        return split_result{false, {}, "twap split produced no children"};
    }

    return result;
}

}  // namespace acct_service
