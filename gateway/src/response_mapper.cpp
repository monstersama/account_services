#include "response_mapper.hpp"

#include "order_mapper.hpp"

namespace acct_service::gateway {

namespace {

order_status_t map_event_kind_to_status(broker_api::event_kind kind) noexcept {
    switch (kind) {
        case broker_api::event_kind::BrokerAccepted:
            return order_status_t::BrokerAccepted;
        case broker_api::event_kind::BrokerRejected:
            return order_status_t::BrokerRejected;
        case broker_api::event_kind::MarketRejected:
            return order_status_t::MarketRejected;
        case broker_api::event_kind::Trade:
            return order_status_t::MarketAccepted;
        case broker_api::event_kind::Finished:
            return order_status_t::Finished;
        default:
            return order_status_t::Unknown;
    }
}

}  // namespace

bool map_broker_event_to_trade_response(
    const broker_api::broker_event& event, trade_response& out_response) noexcept {
    if (event.internal_order_id == 0) {
        return false;
    }

    const order_status_t mapped_status = map_event_kind_to_status(event.kind);
    if (mapped_status == order_status_t::Unknown) {
        return false;
    }

    out_response = trade_response{};
    out_response.internal_order_id = event.internal_order_id;
    out_response.broker_order_id = event.broker_order_id;
    out_response.internal_security_id = event.internal_security_id;
    out_response.trade_side = to_order_side(event.trade_side);
    out_response.new_status = mapped_status;
    out_response.volume_traded = event.volume_traded;
    out_response.dprice_traded = event.price_traded;
    out_response.dvalue_traded = event.value_traded;
    out_response.dfee = event.fee;
    out_response.md_time_traded = event.md_time_traded;
    out_response.recv_time_ns = (event.recv_time_ns != 0) ? event.recv_time_ns : now_ns();

    return true;
}

}  // namespace acct_service::gateway

