#include "order/order_recovery.hpp"

#include <chrono>
#include <limits>
#include <string>
#include <thread>
#include <unordered_map>

#include "common/log.hpp"
#include "shm/orders_shm.hpp"

namespace acct_service {

namespace {

struct recovered_order_candidate {
    OrderIndex index{kInvalidOrderIndex};
    order_slot_snapshot snapshot{};
};

constexpr uint32_t kRecoverReadMaxAttempts = 5;
constexpr uint32_t kRecoverReadBackoffUs = 50;

// 仅恢复已经进入下游队列的订单阶段，避免把上游未发出的订单误恢复到本地簿。
bool is_recoverable_stage(OrderSlotState stage) noexcept {
    return stage == OrderSlotState::DownstreamQueued || stage == OrderSlotState::DownstreamDequeued;
}

// 计算下一个可分配内部订单ID，达到上限后保持饱和避免溢出。
InternalOrderId saturated_next_order_id(InternalOrderId order_id) noexcept {
    constexpr InternalOrderId kMaxOrderId = std::numeric_limits<InternalOrderId>::max();
    return (order_id < kMaxOrderId) ? static_cast<InternalOrderId>(order_id + 1) : kMaxOrderId;
}

}  // namespace

// 扫描 orders_shm 并重建下游在途订单，保障重启后成交回报可继续命中 order_book。
bool recover_downstream_active_orders_from_shm(
    const orders_shm_layout* orders_shm, const upstream_shm_layout* upstream_shm, OrderBook& book) {
    if (!orders_shm) {
        return false;
    }

    const OrderIndex upper = orders_shm->header.next_index.load(std::memory_order_acquire);
    order_recovery_stats stats{};

    std::unordered_map<InternalOrderId, recovered_order_candidate> candidates;
    candidates.reserve(static_cast<std::size_t>(upper));

    // 读取 slot 快照时做短重试，规避并发写入窗口导致的瞬时不可读。
    auto read_snapshot_with_retry = [&](OrderIndex index, order_slot_snapshot& out) -> bool {
        for (uint32_t attempt = 0; attempt < kRecoverReadMaxAttempts; ++attempt) {
            if (orders_shm_read_snapshot(orders_shm, index, out)) {
                return true;
            }
            if (attempt + 1 < kRecoverReadMaxAttempts && kRecoverReadBackoffUs > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(kRecoverReadBackoffUs));
            }
        }
        return false;
    };

    // 第一阶段：筛选可恢复候选，并按 internal_order_id 去重保留最新快照。
    for (OrderIndex index = 0; index < upper; ++index) {
        ++stats.scanned;

        order_slot_snapshot snapshot;
        if (!read_snapshot_with_retry(index, snapshot)) {
            ++stats.unreadable;
            continue;
        }
        if (!is_recoverable_stage(snapshot.stage)) {
            continue;
        }

        const InternalOrderId order_id = snapshot.request.internal_order_id;
        if (order_id == 0) {
            continue;
        }

        const OrderState status = snapshot.request.order_state.load(std::memory_order_acquire);
        if (is_terminal_order_state(status)) {
            continue;
        }

        ++stats.eligible;
        auto [it, inserted] = candidates.try_emplace(order_id, recovered_order_candidate{index, snapshot});
        if (inserted) {
            continue;
        }

        ++stats.dedup_dropped;
        const order_slot_snapshot& existing = it->second.snapshot;
        const bool should_replace = (snapshot.last_update_ns > existing.last_update_ns) ||
                                    ((snapshot.last_update_ns == existing.last_update_ns) && (index > it->second.index));
        if (should_replace) {
            it->second = recovered_order_candidate{index, snapshot};
        }
    }

    // 第二阶段：写回 order_book，并记录最大恢复订单ID用于校正本地发号器。
    InternalOrderId max_recovered_order_id = 0;
    for (const auto& item : candidates) {
        const InternalOrderId order_id = item.first;
        const recovered_order_candidate& candidate = item.second;
        const TimestampNs recovered_time =
            (candidate.snapshot.last_update_ns != 0) ? candidate.snapshot.last_update_ns : now_ns();

        OrderEntry entry{};
        entry.request = candidate.snapshot.request;
        entry.submit_time_ns = recovered_time;
        entry.last_update_ns = recovered_time;
        entry.strategy_id = static_cast<StrategyId>(0);
        entry.risk_result = RiskResult::Pass;
        entry.retry_count = 0;
        entry.is_split_child = false;
        entry.parent_order_id = 0;
        entry.shm_order_index = candidate.index;

        if (!book.add_order(entry)) {
            ++stats.add_failed;
            continue;
        }

        ++stats.restored;
        if (order_id > max_recovered_order_id) {
            max_recovered_order_id = order_id;
        }
    }

    // 合并 upstream 发号器与恢复结果，避免新单ID与已恢复订单冲突。
    stats.next_order_seed = 1;
    if (upstream_shm) {
        stats.next_order_seed = upstream_shm->header.next_order_id.load(std::memory_order_acquire);
    }
    const InternalOrderId recovered_next = saturated_next_order_id(max_recovered_order_id);
    if (recovered_next > stats.next_order_seed) {
        stats.next_order_seed = recovered_next;
    }
    if (stats.next_order_seed > 0) {
        book.ensure_next_order_id_at_least(stats.next_order_seed);
    }

    std::string summary = "recovered downstream orders scanned=" + std::to_string(stats.scanned) +
                          " eligible=" + std::to_string(stats.eligible) +
                          " restored=" + std::to_string(stats.restored) +
                          " unreadable=" + std::to_string(stats.unreadable) +
                          " dedup_dropped=" + std::to_string(stats.dedup_dropped) +
                          " add_failed=" + std::to_string(stats.add_failed) +
                          " next_order_seed=" + std::to_string(static_cast<unsigned long long>(stats.next_order_seed));
    ACCT_LOG_INFO("order_recovery", summary);

    if (stats.unreadable > 0) {
        std::string warning =
            "unreadable orders_shm slots skipped during recovery count=" + std::to_string(static_cast<unsigned long long>(stats.unreadable));
        ACCT_LOG_WARN("order_recovery", warning);
    }

    return true;
}

}  // namespace acct_service
