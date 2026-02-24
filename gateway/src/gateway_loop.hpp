#pragma once

#include <atomic>
#include <deque>

#include "broker_api/broker_api.hpp"
#include "gateway_config.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service::gateway {

// gateway 运行时指标，用于观察处理状态。
struct gateway_stats {
    uint64_t loop_iterations = 0;
    uint64_t idle_iterations = 0;
    uint64_t orders_received = 0;
    uint64_t orders_submitted = 0;
    uint64_t orders_failed = 0;
    uint64_t retries_scheduled = 0;
    uint64_t retries_exhausted = 0;
    uint64_t events_received = 0;
    uint64_t responses_pushed = 0;
    uint64_t responses_dropped = 0;
    uint64_t retry_queue_size = 0;
    timestamp_ns_t last_order_time_ns = 0;
};

// gateway 主循环：
// 读取下游订单 -> 调用适配器 -> 写回成交回报。
class gateway_loop {
public:
    gateway_loop(const gateway_config& config, downstream_shm_layout* downstream_shm, trades_shm_layout* trades_shm,
        broker_api::IBrokerAdapter& adapter);

    int run();
    void stop() noexcept;

    const gateway_stats& stats() const noexcept;

private:
    // 失败后等待重试的请求项。
    struct retry_item {
        broker_api::broker_order_request request;
        uint32_t attempts = 0;
        timestamp_ns_t next_retry_at_ns = 0;
    };

    // 处理到期重试请求。
    bool process_retry_queue();
    // 处理下游订单队列。
    bool process_orders(std::size_t batch_limit);
    // 拉取适配器事件并写回回报队列。
    bool process_events(std::size_t batch_limit);

    // 提交一次请求（含重试入队逻辑）。
    void submit_request(const broker_api::broker_order_request& request, uint32_t attempts);
    // 写 trade_response 到共享内存（带短重试）。
    bool push_response(const trade_response& response);
    // 发送 TraderError 回报（不可恢复失败兜底）。
    void emit_trader_error(internal_order_id_t internal_order_id, internal_security_id_t internal_security_id,
        trade_side_t side_value);
    // 周期打印核心指标。
    void print_periodic_stats();

    gateway_config config_;
    downstream_shm_layout* downstream_shm_ = nullptr;
    trades_shm_layout* trades_shm_ = nullptr;
    broker_api::IBrokerAdapter& adapter_;
    std::atomic<bool> running_{false};
    std::deque<retry_item> retry_queue_;
    gateway_stats stats_{};
    timestamp_ns_t last_stats_print_ns_ = 0;
};

}  // namespace acct_service::gateway
