#include "response_mapper.hpp"

#include <cstring>
#include <string_view>

#include "order_mapper.hpp"

namespace acct_service::gateway {

namespace {

// broker_event 类型与 order_status 的映射关系。
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

// 将 broker_event 映射为 trade_response；如果事件不合法或无法识别则返回 false。
bool map_broker_event_to_trade_response(
    const broker_api::broker_event& event, trade_response& out_response) noexcept {
    // internal_order_id 是上游关联主键，不能为空。
    if (event.internal_order_id == 0) {
        return false;
    }

    const order_status_t mapped_status = map_event_kind_to_status(event.kind);
    // 仅允许可识别状态继续下发，避免写入协议外状态值。
    if (mapped_status == order_status_t::Unknown) {
        return false;
    }

    // 字段逐项映射，保持结构体协议不变。
    out_response = trade_response{};
    out_response.internal_order_id = event.internal_order_id;
    out_response.broker_order_id = event.broker_order_id;
    const std::size_t key_len = ::strnlen(event.internal_security_id, sizeof(event.internal_security_id));
    out_response.internal_security_id.assign(std::string_view(event.internal_security_id, key_len));
    out_response.trade_side = to_order_side(event.trade_side);
    out_response.new_status = mapped_status;
    out_response.volume_traded = event.volume_traded;
    out_response.dprice_traded = event.price_traded;
    out_response.dvalue_traded = event.value_traded;
    out_response.dfee = event.fee;
    out_response.md_time_traded = event.md_time_traded;
    // 若 broker 未提供接收时间，使用本地时间兜底。
    out_response.recv_time_ns = (event.recv_time_ns != 0) ? event.recv_time_ns : now_ns();

    return true;
}

}  // namespace acct_service::gateway
