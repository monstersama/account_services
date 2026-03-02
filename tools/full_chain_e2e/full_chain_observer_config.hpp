#pragma once

#include <cstdint>
#include <string>

namespace acct_service {

// 观测器运行时配置（来自配置文件）。
struct full_chain_observer_config {
    std::string config_file{};
    std::string orders_shm_name{"/orders_shm"};
    std::string trading_day{"19700101"};
    std::string positions_shm_name{"/positions_shm"};
    std::string output_dir{"."};
    uint32_t poll_interval_ms{200};
    uint32_t timeout_ms{30000};
};

// 参数解析结果。
enum class observer_parse_result {
    Ok,
    Help,
    Error,
};

// 打印命令行帮助。
void print_usage(const char* program_name);

// 解析命令行参数并加载配置文件。
observer_parse_result parse_args(
    int argc, char* argv[], full_chain_observer_config* out_config, std::string* out_error);

}  // namespace acct_service
