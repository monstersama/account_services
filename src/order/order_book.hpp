#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/constants.hpp"
#include "common/spinlock.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

// 订单条目（扩展订单信息）
struct OrderEntry {
    OrderRequest request;                  // 原始订单请求与可变成交状态
    TimestampNs submit_time_ns;          // 订单进入订单簿时间
    TimestampNs last_update_ns;          // 最近一次状态/成交更新时间
    StrategyId strategy_id;              // 来源策略ID
    RiskResult risk_result;              // 最近一次风控结果
    uint8_t retry_count;                    // 路由重试次数
    bool is_split_child;                    // 是否为拆单子单（含子撤单）
    InternalOrderId parent_order_id;    // 父单ID（非子单时为0）
    OrderIndex shm_order_index{kInvalidOrderIndex};  // 对应 orders_shm 槽位索引

    bool is_active() const noexcept;
    bool is_terminal() const noexcept;
};

enum class order_book_event_t : uint8_t {
    Added = 1,
    StatusUpdated = 2,
    TradeUpdated = 3,
    Archived = 4,
    ParentRefreshed = 5,
};

using order_change_callback_t = std::function<void(const OrderEntry&, order_book_event_t)>;

// 订单簿管理器
class OrderBook {
public:
    OrderBook();
    ~OrderBook() = default;

    // 禁止拷贝·
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // 添加新订单
    bool add_order(const OrderEntry& entry);

    // 根据内部订单ID查找订单
    OrderEntry* find_order(InternalOrderId order_id);
    const OrderEntry* find_order(InternalOrderId order_id) const;

    // 根据 broker_order_id 查找订单
    OrderEntry* find_by_broker_id(uint64_t broker_order_id);

    // 更新订单状态
    bool update_state(InternalOrderId order_id, OrderState new_state);

    // 更新订单成交信息
    bool update_trade(InternalOrderId order_id, Volume vol, DPrice px, DValue val, DValue fee);

    // 移除已完成订单（移到历史）
    bool archive_order(InternalOrderId order_id);

    // 获取所有活跃订单ID
    std::vector<InternalOrderId> get_active_order_ids() const;

    // 获取证券的所有订单
    std::vector<InternalOrderId> get_orders_by_security(InternalSecurityId security_id) const;

    // 获取父单关联的所有子单（包含子撤单）
    std::vector<InternalOrderId> get_children(InternalOrderId parent_id) const;

    // 反查子单对应父单
    bool try_get_parent(InternalOrderId child_id, InternalOrderId& out_parent_id) const noexcept;

    // 获取活跃订单数量
    std::size_t active_count() const noexcept;

    // 生成新的内部订单ID
    InternalOrderId next_order_id() noexcept;

    // 将内部订单ID生成器抬升到至少 next_id（避免重启恢复后分配冲突）
    void ensure_next_order_id_at_least(InternalOrderId next_id) noexcept;

    // 清空所有订单（仅用于初始化）
    void clear();

    // 设置订单变更回调（用于镜像同步）
    void set_change_callback(order_change_callback_t callback);

private:
    OrderEntry* find_order_nolock(InternalOrderId order_id);
    const OrderEntry* find_order_nolock(InternalOrderId order_id) const;
    void refresh_parent_from_children_nolock(InternalOrderId parent_id);

    std::array<OrderEntry, kMaxActiveOrders> orders_;  // 固定容量订单存储区
    std::unordered_map<InternalOrderId, std::size_t> id_to_index_;  // internal_order_id -> orders_ 下标
    std::unordered_map<uint64_t, InternalOrderId> broker_id_map_;  // broker_order_id -> internal_order_id
    std::unordered_map<InternalSecurityId, std::vector<InternalOrderId>>
        security_orders_;  // 证券 -> 该证券下订单ID集合
    std::unordered_map<InternalOrderId, std::vector<InternalOrderId>>
        parent_to_children_;  // 父单 -> 子单（含子撤单）
    std::unordered_map<InternalOrderId, InternalOrderId> child_to_parent_;  // 子单 -> 父单
    std::unordered_set<InternalOrderId> split_parent_error_latched_;  // 拆单父单错误锁存集合
    std::vector<std::size_t> free_slots_;  // orders_ 空闲槽位栈
    std::size_t active_count_ = 0;  // 当前活跃订单数量
    std::atomic<InternalOrderId> next_order_id_{1};  // 递增内部订单ID生成器
    mutable SpinLock lock_;  // 保护订单簿内部状态
    order_change_callback_t change_callback_;
};

}  // namespace acct_service
