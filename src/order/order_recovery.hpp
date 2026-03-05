#pragma once

#include <cstddef>

#include "order/order_book.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

// 重启恢复统计，用于观测扫描/过滤/恢复效果。
struct order_recovery_stats {
    std::size_t scanned = 0;
    std::size_t eligible = 0;
    std::size_t restored = 0;
    std::size_t unreadable = 0;
    std::size_t dedup_dropped = 0;
    std::size_t add_failed = 0;
    internal_order_id_t next_order_seed = 1;
};

// 从 orders_shm 恢复“已进入下游且未终态”的订单到 order_book。
bool recover_downstream_active_orders_from_shm(
    const orders_shm_layout* orders_shm, const upstream_shm_layout* upstream_shm, order_book& book);

}  // namespace acct_service
