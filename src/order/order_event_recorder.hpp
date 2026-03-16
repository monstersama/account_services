#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "common/business_log_config.hpp"
#include "common/types.hpp"
#include "order/order_book.hpp"

namespace acct_service {

// 编译期判定是否启用详细父子单 trace，供测试和调用点共享。
bool should_emit_debug_order_trace() noexcept;

// 异步记录订单业务事件：生产写稳定事件，调试构建追加详细父子单 trace。
class OrderEventRecorder {
public:
    OrderEventRecorder();
    ~OrderEventRecorder() noexcept;

    OrderEventRecorder(const OrderEventRecorder&) = delete;
    OrderEventRecorder& operator=(const OrderEventRecorder&) = delete;

    // 根据业务日志配置启动后台写线程并打开输出文件。
    bool init(const BusinessLogConfig& config, AccountId account_id, std::string_view trading_day);

    // 停止后台线程并尽力落完已入队事件。
    void shutdown() noexcept;

    // 返回 recorder 是否已成功启用。
    bool enabled() const noexcept;

    // 返回因队列满被丢弃的事件数量。
    uint64_t dropped_count() const noexcept;

    // 记录订单簿对外可见的稳定业务事件。
    void record_order_event(const OrderEntry& entry, order_book_event_t event) noexcept;

    // 记录执行会话创建成功，供调试构建查看父单接管时机。
    void record_session_started(const OrderRequest& parent_request, StrategyId strategy_id,
                                PassiveExecutionAlgo execution_algo) noexcept;

    // 记录执行会话启动被拒绝的原因，帮助调试父单未接管场景。
    void record_session_start_rejected(const OrderRequest& parent_request, StrategyId strategy_id,
                                       PassiveExecutionAlgo execution_algo, std::string_view reason) noexcept;

    // 记录子单提交前的预算和父单镜像，用于调试父子单推进细节。
    void record_child_submit_attempt(const OrderRequest& parent_request, StrategyId strategy_id,
                                     PassiveExecutionAlgo execution_algo, InternalOrderId child_order_id,
                                     Volume requested_volume, DPrice price, ExecutionState execution_state,
                                     Volume target_volume, Volume working_volume, Volume schedulable_volume,
                                     bool active_strategy_claimed) noexcept;

    // 记录子单提交结果，便于区分路由失败与正常派生。
    void record_child_submit_result(const OrderRequest& parent_request, StrategyId strategy_id,
                                    PassiveExecutionAlgo execution_algo, InternalOrderId child_order_id, bool success,
                                    std::string_view reason) noexcept;

    // 记录子单终态结算后的账本摘要，供调试构建追踪父子单收敛过程。
    void record_child_finalized(const OrderRequest& parent_request, StrategyId strategy_id,
                                PassiveExecutionAlgo execution_algo, InternalOrderId child_order_id,
                                OrderState order_state, Volume entrust_volume, Volume traded_volume,
                                Volume cancelled_volume, bool cancel_requested, ExecutionState execution_state,
                                Volume target_volume, Volume working_volume, Volume schedulable_volume) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

}  // namespace acct_service
