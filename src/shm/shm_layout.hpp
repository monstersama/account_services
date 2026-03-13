#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

#include "common/constants.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "portfolio/positions.h"
#include "shm/spsc_queue.hpp"

namespace acct_service {

// order共享内存头部
struct alignas(64) SHMHeader {
    uint32_t magic;                          // 魔数 0x41435354 ("ACST")
    uint32_t version;                        // 版本号
    TimestampNs create_time;              // 创建时间
    TimestampNs last_update;              // 最后更新时间
    std::atomic<uint32_t> next_order_id{1};  // 订单ID计数器（跨进程持久化）
    uint64_t reserved[4];                    // 预留字段

    static constexpr uint32_t kMagic = 0x41435354;
    static constexpr uint32_t kVersion = 3;
};

static_assert(sizeof(SHMHeader) == 64, "SHMHeader must be 64 bytes");

// 持仓共享内存头部
struct alignas(64) PositionsHeader {
    uint32_t magic;              // 魔数 0x41435354 ("ACST")
    uint32_t version;            // 账户服务版本号
    uint32_t header_size;        // 头部大小（字节）
    uint32_t total_size;         // 全部布局大小（字节）
    uint32_t capacity;           // positions[] 容量
    uint32_t init_state;         // 0=未完成初始化，1=可读
    TimestampNs create_time;  // 创建时间
    TimestampNs last_update;  // 最后更新时间
    std::atomic<uint32_t> id{1}; // 
    uint32_t reserved[3];        // 预留/对齐

    static constexpr uint32_t kMagic = 0x41435354;
    static constexpr uint32_t kVersion = 3;
};

static_assert(sizeof(PositionsHeader) == 64, "PositionsHeader must be 64 bytes");

// 成交回报（来自交易进程）
struct alignas(64) TradeResponse {
    InternalOrderId internal_order_id;
    InternalOrderId broker_order_id;
    InternalSecurityId internal_security_id;
    TradeSide trade_side;
    OrderState new_state;
    Volume volume_traded;
    DPrice dprice_traded;
    DValue dvalue_traded;
    DValue dfee;
    MdTime md_time_traded;
    uint32_t padding0;
    TimestampNs recv_time_ns;
};

static_assert(sizeof(TradeResponse) == 128, "TradeResponse must be 128 bytes");

using OrderIndex = uint32_t;
inline constexpr OrderIndex kInvalidOrderIndex = std::numeric_limits<OrderIndex>::max();

enum class OrderSlotState : uint8_t {
    Empty = 0,
    Reserved = 1,
    UpstreamQueued = 2,
    UpstreamDequeued = 3,
    RiskRejected = 4,
    DownstreamQueued = 5,
    DownstreamDequeued = 6,
    Terminal = 7,
    QueuePushFailed = 8,
};

enum class order_slot_source_t : uint8_t {
    Unknown = 0,
    Strategy = 1,
    AccountInternal = 2,
};

// 订单池共享内存头部（订单镜像+索引队列协议）
struct alignas(64) OrdersHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t total_size;
    uint32_t capacity;
    uint32_t init_state;  // 0=未完成初始化，1=可读
    TimestampNs create_time;
    TimestampNs last_update;
    std::atomic<OrderIndex> next_index{0};  // 当日已发布槽位上界（不复用）
    std::atomic<uint64_t> full_reject_count{0};
    char trading_day[9]{};
    uint8_t reserved0[7]{};
    uint64_t reserved[3]{};

    static constexpr uint32_t kMagic = 0x4143534F;  // "ACSO"
    static constexpr uint32_t kVersion = 2;
};

static_assert(alignof(OrdersHeader) == 64, "OrdersHeader must be 64-byte aligned");
static_assert(sizeof(OrdersHeader) == 128, "OrdersHeader must be 128 bytes");

// 订单槽位（seqlock 协议）
struct alignas(64) OrderSlot {
    std::atomic<uint64_t> seq{0};  // 偶数=稳定，奇数=写入中
    TimestampNs last_update_ns{0};
    OrderSlotState stage{OrderSlotState::Empty};
    order_slot_source_t source{order_slot_source_t::Unknown};
    uint16_t reserved0{0};
    uint32_t reserved1{0};
    OrderRequest request{};
};

static_assert(alignof(OrderSlot) == 64, "OrderSlot must be 64-byte aligned");
static_assert(sizeof(OrderSlot) % 64 == 0, "OrderSlot must be cache-line aligned");

// 成交回报共享内存（交易进程→账户服务）
struct trades_shm_layout {
    SHMHeader header;
    spsc_queue<TradeResponse, kResponseQueueCapacity> response_queue;

    static constexpr std::size_t total_size() { return sizeof(trades_shm_layout); }
};

// 上游共享内存（策略→账户服务）
struct upstream_shm_layout {
    SHMHeader header;
    spsc_queue<OrderIndex, kUpstreamOrderQueueCapacity> upstream_order_queue;

    static constexpr std::size_t total_size() { return sizeof(upstream_shm_layout); }
};

// 下游共享内存（账户服务→交易进程）
struct downstream_shm_layout {
    SHMHeader header;
    spsc_queue<OrderIndex, kDownstreamQueueCapacity> order_queue;

    static constexpr std::size_t total_size() { return sizeof(downstream_shm_layout); }
};

// 订单池共享内存（可被外部监控读取）
struct orders_shm_layout {
    OrdersHeader header;
    alignas(64) OrderSlot slots[kDailyOrderPoolCapacity];

    static constexpr std::size_t total_size() { return sizeof(orders_shm_layout); }
};

// 持仓共享内存（可被外部监控读取）
struct positions_shm_layout {
    PositionsHeader header;  // 使用专门的持仓头部
    alignas(64) std::atomic<std::size_t> position_count{0};
    alignas(64) position positions[kMaxPositions];

    static constexpr std::size_t total_size() { return sizeof(positions_shm_layout); }
};

}  // namespace acct_service
