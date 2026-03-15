#pragma once

#include <memory>

#include "core/config_manager.hpp"
#include "market_data/market_data_service.hpp"
#include "order/order_request.hpp"

namespace acct_service {

// 主动策略在单个时间片内的统一输出；首版只支持直接生成一个子单。
struct ActiveDecision {
    bool should_submit = false;
    Volume volume = 0;
    DPrice price = 0;
};

// 主动策略评估时拿到的最小上下文：父单、行情/预测和当前片预算。
struct ActiveStrategyContext {
    const OrderRequest& parent_request;
    const MarketDataView& market_data;
    Volume budget_volume = 0;
};

// 主动策略抽象：在每个 TWAP 时间片内决定是否直接给出子单。
class ActiveStrategy {
public:
    virtual ~ActiveStrategy() = default;

    ActiveStrategy(const ActiveStrategy&) = delete;
    ActiveStrategy& operator=(const ActiveStrategy&) = delete;

    // 返回稳定的策略名，供日志与配置比对。
    virtual const char* name() const noexcept = 0;

    // 基于当前行情/预测和片预算做一次评估。
    virtual ActiveDecision evaluate(const ActiveStrategyContext& context) = 0;

protected:
    ActiveStrategy() = default;
};

// 内建主动策略工厂：首版按名字创建编译进仓库的实现。
class StrategyRegistry {
public:
    // 根据配置创建服务级主动策略；未启用或显式 none 时返回空指针。
    static std::unique_ptr<ActiveStrategy> create(const ActiveStrategyConfig& config);
};

}  // namespace acct_service
