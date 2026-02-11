#pragma once

#include <deque>

#include "broker_api/broker_api.hpp"

namespace acct_service::gateway {

class sim_broker_adapter final : public broker_api::IBrokerAdapter {
public:
    bool initialize(const broker_api::broker_runtime_config& config) override;
    broker_api::send_result submit(const broker_api::broker_order_request& request) override;
    std::size_t poll_events(broker_api::broker_event* out_events, std::size_t max_events) override;
    void shutdown() noexcept override;

private:
    static uint64_t calc_trade_value(uint64_t volume, uint64_t price) noexcept;
    static uint64_t calc_fee(uint64_t traded_value) noexcept;

    broker_api::broker_runtime_config runtime_config_{};
    bool initialized_ = false;
    uint32_t next_broker_order_id_ = 1;
    std::deque<broker_api::broker_event> pending_events_;
};

}  // namespace acct_service::gateway

