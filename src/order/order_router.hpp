#pragma once

#include <vector>

#include "common/types.hpp"
#include "order/order_book.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

// 路由统计
struct router_stats {
    uint64_t orders_received = 0;
    uint64_t orders_sent = 0;
    uint64_t orders_rejected = 0;
    uint64_t queue_full_count = 0;
    TimestampNs last_order_time = 0;
};

// 订单路由器
class order_router {
public:
    order_router(OrderBook& book, downstream_shm_layout* downstream_shm, orders_shm_layout* orders_shm,
                 upstream_shm_layout* upstream_shm = nullptr);
    ~order_router() = default;

    // 禁止拷贝
    order_router(const order_router&) = delete;
    order_router& operator=(const order_router&) = delete;

    // 路由订单（经过风控后调用）
    bool route_order(OrderEntry& entry);

    // 批量路由
    std::size_t route_orders(std::vector<OrderEntry*>& entries);

    // 处理撤单请求
    bool route_cancel(InternalOrderId orig_id, InternalOrderId cancel_id, MdTime time);

    // 提交执行引擎生成的内部子单，复用统一的下游发送与订单簿登记路径。
    bool submit_internal_order(OrderEntry& entry);

    // 启动恢复：从 orders_shm 重建“已下游但未终态”订单到 OrderBook。
    bool recover_downstream_active_orders(const upstream_shm_layout* upstream_shm);

    // 获取统计信息
    const router_stats& stats() const noexcept;

    // 重置统计
    void reset_stats() noexcept;

private:
    InternalOrderId allocate_internal_order_id() noexcept;
    bool send_to_downstream(OrderIndex index);
    bool create_internal_order_slot(const OrderRequest& request, OrderSlotState stage, OrderIndex& out_index,
                                    order_slot_source_t source);

    OrderBook& order_book_;
    downstream_shm_layout* downstream_shm_;
    orders_shm_layout* orders_shm_;
    upstream_shm_layout* upstream_shm_;
    router_stats stats_;
};

}  // namespace acct_service
