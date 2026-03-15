#pragma once

#include "common/types.hpp"

namespace acct_service {

// 将单笔订单声明的被动算法收敛为内部 SplitStrategy，便于复用现有拆单实现。
inline SplitStrategy passive_execution_algo_to_split_strategy(PassiveExecutionAlgo algo) noexcept {
    switch (algo) {
        case PassiveExecutionAlgo::None:
            return SplitStrategy::None;
        case PassiveExecutionAlgo::FixedSize:
            return SplitStrategy::FixedSize;
        case PassiveExecutionAlgo::TWAP:
            return SplitStrategy::TWAP;
        case PassiveExecutionAlgo::VWAP:
            return SplitStrategy::VWAP;
        case PassiveExecutionAlgo::Iceberg:
            return SplitStrategy::Iceberg;
        case PassiveExecutionAlgo::Default:
        default:
            return SplitStrategy::None;
    }
}

// 把默认拆单配置映射成逐单算法枚举，供旧接口回退和监控输出复用。
inline PassiveExecutionAlgo split_strategy_to_passive_execution_algo(SplitStrategy strategy) noexcept {
    switch (strategy) {
        case SplitStrategy::FixedSize:
            return PassiveExecutionAlgo::FixedSize;
        case SplitStrategy::TWAP:
            return PassiveExecutionAlgo::TWAP;
        case SplitStrategy::VWAP:
            return PassiveExecutionAlgo::VWAP;
        case SplitStrategy::Iceberg:
            return PassiveExecutionAlgo::Iceberg;
        case SplitStrategy::None:
        default:
            return PassiveExecutionAlgo::None;
    }
}

// 解析订单最终应使用的被动算法：显式逐单声明优先，否则回落到服务默认值。
inline SplitStrategy resolve_passive_split_strategy(PassiveExecutionAlgo algo,
                                                    SplitStrategy default_strategy) noexcept {
    if (algo == PassiveExecutionAlgo::Default) {
        return default_strategy;
    }
    return passive_execution_algo_to_split_strategy(algo);
}

// 返回稳定的被动算法文本，便于日志与测试断言。
inline const char* passive_execution_algo_name(PassiveExecutionAlgo algo) noexcept {
    switch (algo) {
        case PassiveExecutionAlgo::Default:
            return "default";
        case PassiveExecutionAlgo::None:
            return "none";
        case PassiveExecutionAlgo::FixedSize:
            return "fixed_size";
        case PassiveExecutionAlgo::TWAP:
            return "twap";
        case PassiveExecutionAlgo::VWAP:
            return "vwap";
        case PassiveExecutionAlgo::Iceberg:
            return "iceberg";
        default:
            return "unknown";
    }
}

}  // namespace acct_service
