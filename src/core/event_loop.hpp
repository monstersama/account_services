#pragma once

#include <atomic>
#include <cstdint>

#include "common/types.hpp"
#include "core/config_manager.hpp"
#include "order/order_book.hpp"
#include "order/order_router.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

// 事件循环统计
struct event_loop_stats {
    uint64_t total_iterations = 0;
    uint64_t orders_processed = 0;
    uint64_t responses_processed = 0;
    uint64_t idle_iterations = 0;
    timestamp_ns_t start_time = 0;
    timestamp_ns_t last_order_time = 0;
    timestamp_ns_t last_response_time = 0;

    // 延迟统计（纳秒）
    uint64_t min_latency_ns = UINT64_MAX;
    uint64_t max_latency_ns = 0;
    uint64_t total_latency_ns = 0;
    uint64_t latency_samples = 0;

    double avg_latency_ns() const;
};

// 事件循环
class event_loop {
public:
    event_loop(const event_loop_config& config, upstream_shm_layout* upstream_shm,
        downstream_shm_layout* downstream_shm, order_book& order_book, order_router& router,
        position_manager& positions, risk_manager& risk);
    ~event_loop();

    // 禁止拷贝
    event_loop(const event_loop&) = delete;
    event_loop& operator=(const event_loop&) = delete;

    // 运行事件循环（阻塞）
    void run();

    // 请求停止
    void stop() noexcept;

    // 是否正在运行
    bool is_running() const noexcept;

    // 获取统计信息
    const event_loop_stats& stats() const noexcept;

    // 重置统计
    void reset_stats() noexcept;

    // 注册信号处理
    void setup_signal_handlers();

    // 注入成交回报共享内存（可选）
    void set_trades_shm(trades_shm_layout* trades_shm) noexcept;

private:
    void loop_iteration();
    std::size_t process_upstream_orders();
    std::size_t process_downstream_responses();
    void handle_order_request(order_request& request);
    void handle_trade_response(const trade_response& response);
    void update_latency_stats(timestamp_ns_t start, timestamp_ns_t end);
    void print_periodic_stats();
    void set_cpu_affinity(int core);

    event_loop_config config_;

    upstream_shm_layout* upstream_shm_;
    downstream_shm_layout* downstream_shm_;
    trades_shm_layout* trades_shm_ = nullptr;

    order_book& order_book_;
    order_router& router_;
    position_manager& positions_;
    risk_manager& risk_;

    std::atomic<bool> running_{false};
    event_loop_stats stats_;

    timestamp_ns_t last_stats_time_ = 0;
};

}  // namespace acct_service
