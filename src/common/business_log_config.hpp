#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace acct_service {

// 业务订单日志配置：生产记录稳定订单事件，调试构建额外落详细父子单 trace。
struct BusinessLogConfig {
    bool enabled = true;
    std::string output_dir = "./logs";
    std::size_t queue_capacity = 65536;
    uint32_t flush_interval_ms = 100;
};

}  // namespace acct_service
