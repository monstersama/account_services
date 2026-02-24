#pragma once

#include <deque>

#include "broker_api/broker_api.hpp"

namespace acct_service::gateway {

// 模拟券商适配器：用于本地联调和端到端测试。
class sim_broker_adapter final : public broker_api::IBrokerAdapter {
public:
    bool initialize(const broker_api::broker_runtime_config& config) override;
    broker_api::send_result submit(const broker_api::broker_order_request& request) override;
    std::size_t poll_events(broker_api::broker_event* out_events, std::size_t max_events) override;
    void shutdown() noexcept override;

private:
    // 计算成交金额（volume * price，单位保持为分）。
    static uint64_t calc_trade_value(uint64_t volume, uint64_t price) noexcept;
    // 计算简化手续费（MVP 近似模型）。
    static uint64_t calc_fee(uint64_t traded_value) noexcept;

    broker_api::broker_runtime_config runtime_config_{};
    bool initialized_ = false;
    // 模拟柜台订单号自增序列。
    uint32_t next_broker_order_id_ = 1;
    // 待吐出的回报队列（由 poll_events 批量取走）。
    std::deque<broker_api::broker_event> pending_events_;
};

}  // namespace acct_service::gateway
