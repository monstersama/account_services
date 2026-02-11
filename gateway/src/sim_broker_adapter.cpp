#include "sim_broker_adapter.hpp"

#include <algorithm>

#include "common/types.hpp"

namespace acct_service::gateway {

namespace {

broker_api::broker_event make_base_event(
    broker_api::event_kind kind, const broker_api::broker_order_request& request, uint32_t broker_order_id) {
    broker_api::broker_event event;
    event.kind = kind;
    event.internal_order_id = request.internal_order_id;
    event.broker_order_id = broker_order_id;
    event.internal_security_id = request.internal_security_id;
    event.trade_side = request.trade_side;
    event.md_time_traded = request.md_time;
    event.recv_time_ns = now_ns();
    return event;
}

}  // namespace

bool sim_broker_adapter::initialize(const broker_api::broker_runtime_config& config) {
    runtime_config_ = config;
    initialized_ = true;
    next_broker_order_id_ = 1;
    pending_events_.clear();
    return true;
}

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
        pending_events_.push_back(make_base_event(broker_api::event_kind::BrokerAccepted, request, broker_order_id));

        if (runtime_config_.auto_fill) {
            broker_api::broker_event trade_event = make_base_event(broker_api::event_kind::Trade, request, broker_order_id);
            trade_event.volume_traded = request.volume;
            trade_event.price_traded = request.price;
            trade_event.value_traded = calc_trade_value(request.volume, request.price);
            trade_event.fee = calc_fee(trade_event.value_traded);
            pending_events_.push_back(trade_event);
            pending_events_.push_back(make_base_event(broker_api::event_kind::Finished, request, broker_order_id));
        }

        return broker_api::send_result::ok();
    }

    if (request.type == broker_api::request_type::Cancel) {
        if (request.orig_internal_order_id == 0) {
            return broker_api::send_result::fatal_error(-103);
        }

        const uint32_t broker_order_id = next_broker_order_id_++;
        pending_events_.push_back(make_base_event(broker_api::event_kind::BrokerAccepted, request, broker_order_id));
        pending_events_.push_back(make_base_event(broker_api::event_kind::Finished, request, broker_order_id));
        return broker_api::send_result::ok();
    }

    return broker_api::send_result::fatal_error(-104);
}

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

void sim_broker_adapter::shutdown() noexcept {
    pending_events_.clear();
    initialized_ = false;
}

uint64_t sim_broker_adapter::calc_trade_value(uint64_t volume, uint64_t price) noexcept {
    if (volume == 0 || price == 0) {
        return 0;
    }
    return volume * price;
}

uint64_t sim_broker_adapter::calc_fee(uint64_t traded_value) noexcept {
    if (traded_value == 0) {
        return 0;
    }
    return std::max<uint64_t>(1, traded_value / 10000);
}

}  // namespace acct_service::gateway

