#pragma once

#include <memory>
#include <unordered_map>

#include "execution/execution_config.hpp"
#include "market_data/market_data_service.hpp"
#include "order/order_book.hpp"
#include "order/order_event_recorder.hpp"
#include "order/order_router.hpp"
#include "strategy/active_strategy.hpp"

namespace acct_service {

class ExecutionSession;

// 长期执行引擎：首版只托管需要时间片推进的 TWAP 父单。
class ExecutionEngine {
public:
    enum class SessionStartResult : uint8_t {
        Started = 0,
        Unsupported = 1,
        InvalidConfig = 2,
        Duplicate = 3,
        MarketDataUnavailable = 4,
        Unsplittable = 5,
    };

    ExecutionEngine(const split_config& split_config, OrderBook& order_book, order_router& order_router,
                    MarketDataService* market_data_service, OrderEventRecorder* order_event_recorder,
                    std::unique_ptr<ActiveStrategy> active_strategy);
    ~ExecutionEngine();

    ExecutionEngine(const ExecutionEngine&) = delete;
    ExecutionEngine& operator=(const ExecutionEngine&) = delete;

    // 判断该订单是否应进入长期执行会话，而不是走一次性拆单/直发路径。
    bool should_manage(const OrderRequest& request) const noexcept;

    // 当前服务是否启用了可发主动子单的覆盖策略。
    bool has_active_strategy() const noexcept;

    // 为受管父单创建执行会话；调用方根据结果决定拒绝还是错误。
    SessionStartResult start_session(OrderIndex parent_index, const OrderRequest& parent_request,
                                     StrategyId strategy_id, TimestampNs start_time_ns);

    // 推进所有活跃会话：先尝试主动覆盖，再在应发时点回落到被动 TWAP。
    void tick(TimestampNs now_ns_value);

    // 预留的回报通知入口；首版会话主要通过 OrderBook 轮询子单状态。
    void on_trade_response(const TradeResponse& response) noexcept;

private:
    split_config split_config_;
    OrderBook& order_book_;
    order_router& order_router_;
    MarketDataService* market_data_service_ = nullptr;
    OrderEventRecorder* order_event_recorder_ = nullptr;
    std::unique_ptr<ActiveStrategy> active_strategy_;
    std::unordered_map<InternalOrderId, std::unique_ptr<ExecutionSession>> sessions_;
};

}  // namespace acct_service
