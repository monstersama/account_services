#pragma once

#include "broker_api/broker_api.hpp"
#include "order/order_request.hpp"

namespace acct_service::gateway {

// 买卖方向互转。
broker_api::side to_broker_side(trade_side_t side_value) noexcept;
trade_side_t to_order_side(broker_api::side side_value) noexcept;

// 将共享内存订单请求转换为 broker_api 请求。
bool map_order_request_to_broker(
    const order_request& request, broker_api::broker_order_request& out_request) noexcept;

}  // namespace acct_service::gateway
