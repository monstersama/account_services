#pragma once

#include <string>

#include "common/types.hpp"

namespace acct_service::gateway {

struct gateway_config {
    account_id_t account_id = 1;
    std::string downstream_shm_name = "/downstream_order_shm";
    std::string trades_shm_name = "/trades_shm";
    std::string broker_type = "sim";
    bool create_if_not_exist = false;
    uint32_t poll_batch_size = 64;
    uint32_t idle_sleep_us = 50;
    uint32_t stats_interval_ms = 1000;
    uint32_t max_retry_attempts = 3;
    uint32_t retry_interval_us = 200;
};

enum class parse_result_t {
    Ok,
    Help,
    Error,
};

void print_usage(const char* program);
parse_result_t parse_args(int argc, char* argv[], gateway_config& config, std::string& error_message);

}  // namespace acct_service::gateway

