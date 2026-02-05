#pragma once

#include <array>
#include <atomic>
#include <unordered_map>
#include <vector>

#include "common/constants.hpp"
#include "common/spinlock.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"

namespace acct_service {

// 订单条目（扩展订单信息）
struct order_entry {
    order_request request;
    timestamp_ns_t submit_time_ns;
    timestamp_ns_t last_update_ns;
    strategy_id_t strategy_id;
    risk_result_t risk_result;
    uint8_t retry_count;
    bool is_split_child;
    internal_order_id_t parent_order_id;

    bool is_active() const noexcept;
    bool is_terminal() const noexcept;
};

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

    // 获取活跃订单数量
    std::size_t active_count() const noexcept;

    // 生成新的内部订单ID
    internal_order_id_t next_order_id() noexcept;

    // 清空所有订单（仅用于初始化）
    void clear();

private:
    std::array<order_entry, kMaxActiveOrders> orders_;
    std::unordered_map<internal_order_id_t, std::size_t> id_to_index_;
    std::unordered_map<uint64_t, internal_order_id_t> broker_id_map_;
    std::unordered_map<internal_security_id_t, std::vector<internal_order_id_t>> security_orders_;
    std::vector<std::size_t> free_slots_;
    std::size_t active_count_ = 0;
    std::atomic<internal_order_id_t> next_order_id_{1};
    mutable spinlock lock_;
};

}  // namespace acct_service
