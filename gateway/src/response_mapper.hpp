#pragma once

#include "broker_api/broker_api.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service::gateway {

// 将适配器事件转换为 account_service 可消费的 trade_response。
bool map_broker_event_to_trade_response(
    const broker_api::broker_event& event, trade_response& out_response) noexcept;

}  // namespace acct_service::gateway
