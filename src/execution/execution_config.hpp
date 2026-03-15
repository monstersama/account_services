#pragma once

#include "common/types.hpp"

namespace acct_service {

// 执行算法默认参数来源；当前继续复用 split 配置字段，但语义已转为执行会话默认值。
struct split_config {
    SplitStrategy strategy = SplitStrategy::None;
    Volume max_child_volume = 0;
    Volume min_child_volume = 100;
    uint32_t max_child_count = 100;
    uint32_t interval_ms = 0;
    double randomize_factor = 0.0;
};

}  // namespace acct_service
