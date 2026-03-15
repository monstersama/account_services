#pragma once

#include <deque>
#include <unordered_map>

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
    struct active_order_state {
        uint32_t broker_order_id = 0;
        broker_api::side trade_side = broker_api::side::Unknown;
        uint64_t entrust_volume = 0;
        uint64_t traded_volume = 0;
        uint64_t price = 0;
        uint32_t md_time = 0;
        bool finalized = false;
    };

    // 生成自然完成事件，明确 cancelled_volume 为 0。
    static broker_api::broker_event make_natural_finish_event(const broker_api::broker_order_request& request,
                                                              uint32_t broker_order_id);

    // 生成撤单完成事件，并带上权威剩余撤销量。
    static broker_api::broker_event make_cancel_finish_event(const broker_api::broker_order_request& request,
                                                             const active_order_state& active_order);

    // 计算成交金额（volume * price，单位保持为分）。
    static uint64_t calc_trade_value(uint64_t volume, uint64_t price) noexcept;
    // 计算简化手续费（MVP 近似模型）。
    static uint64_t calc_fee(uint64_t traded_value) noexcept;

    broker_api::broker_runtime_config runtime_config_{};
    bool initialized_ = false;
    // 模拟柜台订单号自增序列。
    uint32_t next_broker_order_id_ = 1;
    // 跟踪在途新单，供撤单完成事件回填权威剩余撤销量。
    std::unordered_map<uint32_t, active_order_state> active_orders_;
    // 待吐出的回报队列（由 poll_events 批量取走）。
    std::deque<broker_api::broker_event> pending_events_;
};

}  // namespace acct_service::gateway
