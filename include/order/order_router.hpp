#pragma once

#include "common/types.hpp"
#include "order/order_book.hpp"
#include "order/order_splitter.hpp"
#include "shm/shm_layout.hpp"

#include <vector>

namespace acct {

// 路由统计
struct router_stats {
    uint64_t orders_received = 0;
    uint64_t orders_sent = 0;
    uint64_t orders_split = 0;
    uint64_t orders_rejected = 0;
    uint64_t queue_full_count = 0;
    timestamp_ns_t last_order_time = 0;
};

// 订单路由器
class order_router {
public:
    order_router(order_book& book, downstream_shm_layout* shm,
                 const split_config& config);
    ~order_router() = default;

    // 禁止拷贝
    order_router(const order_router&) = delete;
    order_router& operator=(const order_router&) = delete;

    // 路由订单（经过风控后调用）
    bool route_order(order_entry& entry);

    // 批量路由
    std::size_t route_orders(std::vector<order_entry*>& entries);

    // 处理撤单请求
    bool route_cancel(internal_order_id_t orig_id, internal_order_id_t cancel_id,
                      md_time_t time);

    // 获取统计信息
    const router_stats& stats() const noexcept;

    // 重置统计
    void reset_stats() noexcept;

private:
    bool send_to_downstream(const order_request& request);
    bool handle_split_order(order_entry& parent);

    order_book& order_book_;
    downstream_shm_layout* downstream_shm_;
    order_splitter splitter_;
    router_stats stats_;
};

}  // namespace acct
