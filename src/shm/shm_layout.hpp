#pragma once

#include "common/constants.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "portfolio/positions.h"
#include "shm/spsc_queue.hpp"

#include <atomic>
#include <cstdint>

namespace acct {

// 共享内存头部
struct alignas(64) shm_header {
    uint32_t magic;                        // 魔数 0x41435354 ("ACST")
    uint32_t version;                      // 版本号
    timestamp_ns_t create_time;            // 创建时间
    timestamp_ns_t last_update;            // 最后更新时间
    uint64_t reserved[6];                  // 预留字段

    static constexpr uint32_t kMagic = 0x41435354;
    static constexpr uint32_t kVersion = 1;
};

// 成交回报（来自交易进程）
struct alignas(64) trade_response {
    internal_order_id_t internal_order_id;
    internal_order_id_t broker_order_id;
    internal_security_id_t internal_security_id;
    trade_side_t trade_side;
    order_status_t new_status;
    volume_t volume_traded;
    dprice_t dprice_traded;
    dvalue_t dvalue_traded;
    dvalue_t dfee;
    md_time_t md_time_traded;
    uint32_t padding0;
    timestamp_ns_t recv_time_ns;
};

static_assert(sizeof(trade_response) == 64, "trade_response must be 64 bytes");

// 上游共享内存（策略→账户服务）
struct upstream_shm_layout {
    shm_header header;
    spsc_queue<order_request, kStrategyOrderQueueCapacity> strategy_order_queue;

    static constexpr std::size_t total_size() { return sizeof(upstream_shm_layout); }
};

// 下游共享内存（账户服务→交易进程）
struct downstream_shm_layout {
    shm_header header;
    spsc_queue<order_request, kDownstreamQueueCapacity> order_queue;
    spsc_queue<trade_response, kResponseQueueCapacity> response_queue;

    static constexpr std::size_t total_size() { return sizeof(downstream_shm_layout); }
};

// 持仓共享内存（可被外部监控读取）
struct positions_shm_layout {
    shm_header header;
    alignas(64) fund_info fund;
    alignas(64) std::atomic<std::size_t> position_count{0};
    alignas(64) position positions[kMaxPositions];

    static constexpr std::size_t total_size() { return sizeof(positions_shm_layout); }
};

}  // namespace acct
