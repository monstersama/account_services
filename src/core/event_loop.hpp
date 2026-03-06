#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include "common/types.hpp"
#include "core/config_manager.hpp"
#include "order/order_book.hpp"
#include "order/order_router.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

struct account_info;

// 事件循环统计
struct event_loop_stats {
    uint64_t total_iterations = 0;        // 事件循环总迭代次数
    uint64_t orders_processed = 0;        // 已处理上游订单总数
    uint64_t responses_processed = 0;     // 已处理下游成交回报总数
    uint64_t idle_iterations = 0;         // 空闲迭代次数（无订单且无回报）
    TimestampNs start_time = 0;        // 事件循环启动时间（Unix Epoch 纳秒）
    TimestampNs last_order_time = 0;   // 最近一次处理订单时间（Unix Epoch 纳秒）
    TimestampNs last_response_time = 0;  // 最近一次处理回报时间（Unix Epoch 纳秒）

    uint64_t min_latency_ns = UINT64_MAX;  // 单轮迭代最小耗时（纳秒）
    uint64_t max_latency_ns = 0;           // 单轮迭代最大耗时（纳秒）
    uint64_t total_latency_ns = 0;         // 单轮迭代耗时累计值（纳秒）
    uint64_t latency_samples = 0;          // 延迟统计样本数

    // 计算平均单轮迭代耗时（纳秒）
    double avg_latency_ns() const;
};

// 事件循环
class EventLoop {
public:
    // 构造事件循环并绑定各核心组件
    EventLoop(const EventLoopConfig& config, upstream_shm_layout* upstream_shm,
        downstream_shm_layout* downstream_shm, trades_shm_layout* trades_shm, orders_shm_layout* orders_shm,
        OrderBook& OrderBook, order_router& router, PositionManager& positions, RiskManager& risk,
        const account_info* account_info = nullptr);

    // 析构时会确保循环停止
    ~EventLoop();

    // 禁止拷贝
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // 运行事件循环（阻塞）
    void run();

    // 请求停止
    void stop() noexcept;

    // 是否正在运行
    bool is_running() const noexcept;

    // 获取统计信息
    const event_loop_stats& stats() const noexcept;

    // 重置统计
    void reset_stats() noexcept;

    // 注册信号处理
    void setup_signal_handlers();

private:
    // 执行单轮事件循环
    void loop_iteration();

    // 批量处理上游订单，返回本轮处理数量
    std::size_t process_upstream_orders();

    // 批量处理下游回报，返回本轮处理数量
    std::size_t process_downstream_responses();

    // 处理单笔上游订单请求
    void handle_order_request(OrderIndex index, OrderRequest& request);

    // 为新单补齐手续费估算，保证风控与冻结使用一致口径。
    void prepare_order_estimate(OrderRequest& request) const;

    // 在路由前冻结买单资金，防止并发订单超用可用资金。
    bool reserve_order_resources(OrderEntry& entry);

    // 在终态或发送失败时释放订单剩余冻结资金。
    void release_order_resources(OrderEntry& entry);

    // 优先从订单冻结资金结算买入成交；旧订单回退到可用资金结算。
    bool settle_buy_trade_fund(OrderEntry& entry, DValue amount, DValue fee);

    // 订单簿变更回调，回写订单池镜像
    void on_order_book_changed(const OrderEntry& entry, order_book_event_t event);

    // 处理单笔成交回报
    void handle_trade_response(const TradeResponse& response);

    // 处理到期的终态订单归档任务
    void process_pending_archives(TimestampNs now_ns_value);

    // 更新延迟统计
    void update_latency_stats(TimestampNs start, TimestampNs end);

    // 按周期打印统计信息
    void print_periodic_stats();

    // 绑定线程 CPU 亲和性（可选）
    void set_cpu_affinity(int core);

    EventLoopConfig config_;  // 事件循环配置快照

    upstream_shm_layout* upstream_shm_;      // 上游共享内存（策略->账户）
    downstream_shm_layout* downstream_shm_;  // 下游共享内存（账户->交易）
    trades_shm_layout* trades_shm_;  // 成交回报共享内存（交易->账户）
    orders_shm_layout* orders_shm_;  // 订单池共享内存（监控可见）

    OrderBook& order_book_;        // 订单簿组件
    order_router& router_;          // 路由组件
    PositionManager& positions_;   // 持仓/资金组件
    RiskManager& risk_;            // 风控组件
    const account_info* account_info_ = nullptr;  // 账户费率快照（可为空）

    std::atomic<bool> running_{false};  // 运行状态标志
    event_loop_stats stats_;            // 运行统计

    std::unordered_map<InternalOrderId, TimestampNs>
        pending_archive_deadlines_ns_;      // 终态订单 -> 延迟归档到期时间
    TimestampNs last_stats_time_ = 0;  // 最近一次打印统计的单调时钟时间
};

}  // namespace acct_service
