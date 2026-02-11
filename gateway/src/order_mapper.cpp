#include "order_mapper.hpp"

#include <algorithm>
#include <cstring>

namespace acct_service::gateway {

namespace {

broker_api::request_type to_broker_request_type(order_type_t type_value) noexcept {
    switch (type_value) {
        case order_type_t::New:
            return broker_api::request_type::New;
        case order_type_t::Cancel:
            return broker_api::request_type::Cancel;
        default:
            return broker_api::request_type::Unknown;
    }
}

broker_api::market to_broker_market(market_t market_value) noexcept {
    switch (market_value) {
        case market_t::SZ:
            return broker_api::market::SZ;
        case market_t::SH:
            return broker_api::market::SH;
        case market_t::BJ:
            return broker_api::market::BJ;
        case market_t::HK:
            return broker_api::market::HK;
        default:
            return broker_api::market::Unknown;
    }
}

void copy_security_id(const fixed_string<SECURITY_ID_SIZE>& source, char* destination, std::size_t capacity) {
    if (capacity == 0) {
        return;
    }

    std::memset(destination, 0, capacity);
    const std::string_view sec_id = source.view();
    const std::size_t copy_size = std::min(sec_id.size(), capacity - 1);
    if (copy_size > 0) {
        std::memcpy(destination, sec_id.data(), copy_size);
    }
}

}  // namespace

broker_api::side to_broker_side(trade_side_t side_value) noexcept {
    switch (side_value) {
        case trade_side_t::Buy:
            return broker_api::side::Buy;
        case trade_side_t::Sell:
            return broker_api::side::Sell;
        default:
            return broker_api::side::Unknown;
    }
}

trade_side_t to_order_side(broker_api::side side_value) noexcept {
    switch (side_value) {
        case broker_api::side::Buy:
            return trade_side_t::Buy;
        case broker_api::side::Sell:
            return trade_side_t::Sell;
        default:
            return trade_side_t::NotSet;
    }
}

bool map_order_request_to_broker(
    const order_request& request, broker_api::broker_order_request& out_request) noexcept {
    if (request.internal_order_id == 0) {
        return false;
    }

    const broker_api::request_type mapped_type = to_broker_request_type(request.order_type);
    if (mapped_type == broker_api::request_type::Unknown) {
        return false;
    }

    out_request = broker_api::broker_order_request{};
    out_request.internal_order_id = request.internal_order_id;
    out_request.orig_internal_order_id = request.orig_internal_order_id;
    out_request.internal_security_id = request.internal_security_id;
    out_request.type = mapped_type;
    out_request.trade_side = to_broker_side(request.trade_side);
    out_request.order_market = to_broker_market(request.market);
    out_request.volume = request.volume_entrust;
    out_request.price = request.dprice_entrust;
    out_request.md_time = (request.md_time_entrust != 0) ? request.md_time_entrust : request.md_time_driven;

    if (out_request.type == broker_api::request_type::New) {
        if (out_request.trade_side == broker_api::side::Unknown || out_request.order_market == broker_api::market::Unknown ||
            out_request.volume == 0 || out_request.price == 0) {
            return false;
        }
        copy_security_id(request.security_id, out_request.security_id, sizeof(out_request.security_id));
        if (out_request.security_id[0] == 0) {
            return false;
        }
    }

    return true;
}

}  // namespace acct_service::gateway

