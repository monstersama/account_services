#pragma once

#include <atomic>
#include <deque>

#include "broker_api/broker_api.hpp"
#include "gateway_config.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service::gateway {

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

class gateway_loop {
public:
    gateway_loop(const gateway_config& config, downstream_shm_layout* downstream_shm, trades_shm_layout* trades_shm,
        broker_api::IBrokerAdapter& adapter);

    int run();
    void stop() noexcept;

    const gateway_stats& stats() const noexcept;

private:
    struct retry_item {
        broker_api::broker_order_request request;
        uint32_t attempts = 0;
        timestamp_ns_t next_retry_at_ns = 0;
    };

    bool process_retry_queue();
    bool process_orders(std::size_t batch_limit);
    bool process_events(std::size_t batch_limit);

    void submit_request(const broker_api::broker_order_request& request, uint32_t attempts);
    bool push_response(const trade_response& response);
    void emit_trader_error(internal_order_id_t internal_order_id, internal_security_id_t internal_security_id,
        trade_side_t side_value);
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

