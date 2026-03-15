#include "sim_broker_adapter.hpp"

#include <algorithm>
#include <cstring>

#include "common/types.hpp"

namespace acct_service::gateway {

namespace {

// 生成回报事件的通用字段，避免重复填充。
broker_api::broker_event make_base_event(
    broker_api::event_kind kind, const broker_api::broker_order_request& request, uint32_t broker_order_id) {
    broker_api::broker_event event;
    event.kind = kind;
    event.internal_order_id = request.internal_order_id;
    event.broker_order_id = broker_order_id;
    std::memcpy(event.internal_security_id, request.internal_security_id, sizeof(event.internal_security_id));
    event.trade_side = request.trade_side;
    event.md_time_traded = request.md_time;
    event.recv_time_ns = now_ns();
    return event;
}

}  // namespace

// 生成自然完成事件，保持“完成但未撤单”的 cancelled_volume 为 0。
broker_api::broker_event sim_broker_adapter::make_natural_finish_event(const broker_api::broker_order_request& request,
                                                                       uint32_t broker_order_id) {
    broker_api::broker_event event = make_base_event(broker_api::event_kind::Finished, request, broker_order_id);
    event.cancelled_volume = 0;
    return event;
}

// 生成撤单完成事件，并把当前未成交剩余量作为权威 cancelled_volume 返回。
broker_api::broker_event sim_broker_adapter::make_cancel_finish_event(const broker_api::broker_order_request& request,
                                                                      const active_order_state& active_order) {
    broker_api::broker_order_request original_request{};
    original_request.internal_order_id = request.orig_internal_order_id;
    original_request.orig_internal_order_id = request.orig_internal_order_id;
    std::memcpy(original_request.internal_security_id, request.internal_security_id,
                sizeof(original_request.internal_security_id));
    original_request.type = broker_api::request_type::Cancel;
    original_request.trade_side = active_order.trade_side;
    original_request.order_market = request.order_market;
    original_request.volume = active_order.entrust_volume;
    original_request.price = active_order.price;
    original_request.md_time = request.md_time;

    broker_api::broker_event event = make_base_event(broker_api::event_kind::Finished, original_request,
                                                     active_order.broker_order_id);
    event.cancelled_volume =
        (active_order.traded_volume >= active_order.entrust_volume) ? 0 : (active_order.entrust_volume - active_order.traded_volume);
    return event;
}

// 初始化模拟适配器运行状态。
bool sim_broker_adapter::initialize(const broker_api::broker_runtime_config& config) {
    runtime_config_ = config;
    initialized_ = true;
    next_broker_order_id_ = 1;
    active_orders_.clear();
    pending_events_.clear();
    return true;
}

// 模拟 submit 行为：
// - New: 受理，按配置可自动产生成交+完成回报
// - Cancel: 受理并完成
broker_api::send_result sim_broker_adapter::submit(const broker_api::broker_order_request& request) {
    if (!initialized_) {
        return broker_api::send_result::fatal_error(-100);
    }
    if (request.internal_order_id == 0) {
        return broker_api::send_result::fatal_error(-101);
    }

    if (request.type == broker_api::request_type::New) {
        if (request.trade_side == broker_api::side::Unknown || request.order_market == broker_api::market::Unknown ||
            request.volume == 0 || request.price == 0 || request.security_id[0] == 0) {
            return broker_api::send_result::fatal_error(-102);
        }

        const uint32_t broker_order_id = next_broker_order_id_++;
        active_order_state active_order{};
        active_order.broker_order_id = broker_order_id;
        active_order.trade_side = request.trade_side;
        active_order.entrust_volume = request.volume;
        active_order.traded_volume = 0;
        active_order.price = request.price;
        active_order.md_time = request.md_time;
        active_orders_[request.internal_order_id] = active_order;
        pending_events_.push_back(make_base_event(broker_api::event_kind::BrokerAccepted, request, broker_order_id));

        if (runtime_config_.auto_fill) {
            broker_api::broker_event trade_event = make_base_event(broker_api::event_kind::Trade, request, broker_order_id);
            trade_event.volume_traded = request.volume;
            trade_event.price_traded = request.price;
            trade_event.value_traded = calc_trade_value(request.volume, request.price);
            trade_event.fee = calc_fee(trade_event.value_traded);
            active_orders_[request.internal_order_id].traded_volume = request.volume;
            pending_events_.push_back(trade_event);
            pending_events_.push_back(make_natural_finish_event(request, broker_order_id));
            active_orders_.erase(request.internal_order_id);
        }

        return broker_api::send_result::ok();
    }

    if (request.type == broker_api::request_type::Cancel) {
        if (request.orig_internal_order_id == 0) {
            return broker_api::send_result::fatal_error(-103);
        }

        const uint32_t broker_order_id = next_broker_order_id_++;
        pending_events_.push_back(make_base_event(broker_api::event_kind::BrokerAccepted, request, broker_order_id));

        const auto active_it = active_orders_.find(request.orig_internal_order_id);
        if (active_it != active_orders_.end()) {
            pending_events_.push_back(make_cancel_finish_event(request, active_it->second));
            active_orders_.erase(active_it);
        } else {
            broker_api::broker_event finish_event = make_base_event(broker_api::event_kind::Finished, request,
                                                                    broker_order_id);
            finish_event.cancelled_volume = 0;
            pending_events_.push_back(finish_event);
        }
        return broker_api::send_result::ok();
    }

    return broker_api::send_result::fatal_error(-104);
}

// 批量拉取回报事件给 gateway。
std::size_t sim_broker_adapter::poll_events(broker_api::broker_event* out_events, std::size_t max_events) {
    if (!initialized_ || !out_events || max_events == 0) {
        return 0;
    }

    const std::size_t count = std::min(max_events, pending_events_.size());
    for (std::size_t i = 0; i < count; ++i) {
        out_events[i] = pending_events_.front();
        pending_events_.pop_front();
    }
    return count;
}

// 关闭适配器并清理缓存事件。
void sim_broker_adapter::shutdown() noexcept {
    active_orders_.clear();
    pending_events_.clear();
    initialized_ = false;
}

// 简化的成交金额计算。
uint64_t sim_broker_adapter::calc_trade_value(uint64_t volume, uint64_t price) noexcept {
    if (volume == 0 || price == 0) {
        return 0;
    }
    return volume * price;
}

// 简化的手续费模型，保证有成交时费用至少为 1。
uint64_t sim_broker_adapter::calc_fee(uint64_t traded_value) noexcept {
    if (traded_value == 0) {
        return 0;
    }
    return std::max<uint64_t>(1, traded_value / 10000);
}

}  // namespace acct_service::gateway
