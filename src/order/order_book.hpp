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
struct order_entry {
    order_request request;                  // 原始订单请求与可变成交状态
    timestamp_ns_t submit_time_ns;          // 订单进入订单簿时间
    timestamp_ns_t last_update_ns;          // 最近一次状态/成交更新时间
    strategy_id_t strategy_id;              // 来源策略ID
    risk_result_t risk_result;              // 最近一次风控结果
    uint8_t retry_count;                    // 路由重试次数
    bool is_split_child;                    // 是否为拆单子单（含子撤单）
    internal_order_id_t parent_order_id;    // 父单ID（非子单时为0）
    order_index_t shm_order_index{kInvalidOrderIndex};  // 对应 orders_shm 槽位索引

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

using order_change_callback_t = std::function<void(const order_entry&, order_book_event_t)>;

// 订单簿管理器
class order_book {
public:
    order_book();
    ~order_book() = default;

    // 禁止拷贝·
    order_book(const order_book&) = delete;
    order_book& operator=(const order_book&) = delete;

    // 添加新订单
    bool add_order(const order_entry& entry);

    // 根据内部订单ID查找订单
    order_entry* find_order(internal_order_id_t order_id);
    const order_entry* find_order(internal_order_id_t order_id) const;

    // 根据 broker_order_id 查找订单
    order_entry* find_by_broker_id(uint64_t broker_order_id);

    // 更新订单状态
    bool update_status(internal_order_id_t order_id, order_status_t new_status);

    // 更新订单成交信息
    bool update_trade(internal_order_id_t order_id, volume_t vol, dprice_t px, dvalue_t val, dvalue_t fee);

    // 移除已完成订单（移到历史）
    bool archive_order(internal_order_id_t order_id);

    // 获取所有活跃订单ID
    std::vector<internal_order_id_t> get_active_order_ids() const;

    // 获取证券的所有订单
    std::vector<internal_order_id_t> get_orders_by_security(internal_security_id_t security_id) const;

    // 获取父单关联的所有子单（包含子撤单）
    std::vector<internal_order_id_t> get_children(internal_order_id_t parent_id) const;

    // 反查子单对应父单
    bool try_get_parent(internal_order_id_t child_id, internal_order_id_t& out_parent_id) const noexcept;

    // 获取活跃订单数量
    std::size_t active_count() const noexcept;

    // 生成新的内部订单ID
    internal_order_id_t next_order_id() noexcept;

    // 清空所有订单（仅用于初始化）
    void clear();

    // 设置订单变更回调（用于镜像同步）
    void set_change_callback(order_change_callback_t callback);

private:
    order_entry* find_order_nolock(internal_order_id_t order_id);
    const order_entry* find_order_nolock(internal_order_id_t order_id) const;
    void refresh_parent_from_children_nolock(internal_order_id_t parent_id);

    std::array<order_entry, kMaxActiveOrders> orders_;  // 固定容量订单存储区
    std::unordered_map<internal_order_id_t, std::size_t> id_to_index_;  // internal_order_id -> orders_ 下标
    std::unordered_map<uint64_t, internal_order_id_t> broker_id_map_;  // broker_order_id -> internal_order_id
    std::unordered_map<internal_security_id_t, std::vector<internal_order_id_t>>
        security_orders_;  // 证券 -> 该证券下订单ID集合
    std::unordered_map<internal_order_id_t, std::vector<internal_order_id_t>>
        parent_to_children_;  // 父单 -> 子单（含子撤单）
    std::unordered_map<internal_order_id_t, internal_order_id_t> child_to_parent_;  // 子单 -> 父单
    std::unordered_set<internal_order_id_t> split_parent_error_latched_;  // 拆单父单错误锁存集合
    std::vector<std::size_t> free_slots_;  // orders_ 空闲槽位栈
    std::size_t active_count_ = 0;  // 当前活跃订单数量
    std::atomic<internal_order_id_t> next_order_id_{1};  // 递增内部订单ID生成器
    mutable spinlock lock_;  // 保护订单簿内部状态
    order_change_callback_t change_callback_;
};

}  // namespace acct_service
