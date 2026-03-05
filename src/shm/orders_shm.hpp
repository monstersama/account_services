#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

#include "common/log.hpp"
#include "common/types.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

struct order_slot_snapshot {
    OrderRequest request{};
    OrderSlotState stage{OrderSlotState::Empty};
    order_slot_source_t source{order_slot_source_t::Unknown};
    TimestampNs last_update_ns{0};
};

inline bool is_valid_trading_day(std::string_view trading_day) noexcept {
    if (trading_day.size() != 8) {
        return false;
    }
    return std::all_of(trading_day.begin(), trading_day.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

inline std::string make_orders_shm_name(std::string_view base_name, std::string_view trading_day) {
    std::string name(base_name);
    name += "_";
    name.append(trading_day.data(), trading_day.size());
    return name;
}

inline bool extract_trading_day_from_name(std::string_view shm_name, char out_trading_day[9]) noexcept {
    if (!out_trading_day) {
        return false;
    }

    std::memset(out_trading_day, 0, 9);
    const std::size_t pos = shm_name.find_last_of('_');
    if (pos == std::string_view::npos) {
        return false;
    }
    const std::string_view suffix = shm_name.substr(pos + 1);
    if (!is_valid_trading_day(suffix)) {
        return false;
    }

    std::memcpy(out_trading_day, suffix.data(), suffix.size());
    return true;
}

inline bool is_terminal_order_state(OrderState status) noexcept {
    switch (status) {
        case OrderState::RiskControllerRejected:
        case OrderState::TraderRejected:
        case OrderState::TraderError:
        case OrderState::BrokerRejected:
        case OrderState::MarketRejected:
        case OrderState::Finished:
        case OrderState::Unknown:
            return true;
        default:
            return false;
    }
}

inline bool orders_shm_index_exists(const orders_shm_layout* shm, OrderIndex index) noexcept {
    if (!shm || index == kInvalidOrderIndex) {
        return false;
    }
    const OrderIndex upper = shm->header.next_index.load(std::memory_order_acquire);
    return index < upper && index < shm->header.capacity;
}

inline bool orders_shm_try_allocate(orders_shm_layout* shm, OrderIndex& out_index) noexcept {
    if (!shm) {
        return false;
    }

    OrderIndex current = shm->header.next_index.load(std::memory_order_acquire);
    while (true) {
        if (current >= shm->header.capacity) {
            shm->header.full_reject_count.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const OrderIndex next = current + 1;
        if (shm->header.next_index.compare_exchange_weak(
                current, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            out_index = current;
            const OrderIndex warn80 = static_cast<OrderIndex>((static_cast<uint64_t>(shm->header.capacity) * 80ULL) / 100ULL);
            const OrderIndex warn95 = static_cast<OrderIndex>((static_cast<uint64_t>(shm->header.capacity) * 95ULL) / 100ULL);
            if (next == warn80) {
                ACCT_LOG_WARN("orders_shm", "orders pool usage reached 80%");
            } else if (next == warn95) {
                ACCT_LOG_WARN("orders_shm", "orders pool usage reached 95%");
            }
            return true;
        }
    }
}

template <typename Mutator>
inline bool orders_shm_mutate_slot(orders_shm_layout* shm, OrderIndex index, Mutator&& mutator) noexcept {
    static_assert(std::is_invocable_v<Mutator, OrderSlot&>, "mutator must be callable with OrderSlot&");

    if (!orders_shm_index_exists(shm, index)) {
        return false;
    }

    OrderSlot& slot = shm->slots[index];
    uint64_t seq = slot.seq.load(std::memory_order_relaxed);
    if ((seq & 1ULL) != 0U) {
        ++seq;
    }

    slot.seq.store(seq + 1, std::memory_order_relaxed);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    mutator(slot);

    slot.seq.store(seq + 2, std::memory_order_release);  // even: publish
    shm->header.last_update = now_ns();
    return true;
}

inline bool orders_shm_write_order(orders_shm_layout* shm, OrderIndex index, const OrderRequest& request,
    OrderSlotState stage, order_slot_source_t source, TimestampNs update_ns) noexcept {
    return orders_shm_mutate_slot(shm, index, [&](OrderSlot& slot) {
        slot.request = request;
        slot.stage = stage;
        slot.source = source;
        slot.last_update_ns = update_ns;
    });
}

inline bool orders_shm_sync_order(orders_shm_layout* shm, OrderIndex index, const OrderRequest& request,
    TimestampNs update_ns) noexcept {
    return orders_shm_mutate_slot(shm, index, [&](OrderSlot& slot) {
        slot.request = request;
        slot.last_update_ns = update_ns;
    });
}

inline bool orders_shm_update_stage(
    orders_shm_layout* shm, OrderIndex index, OrderSlotState stage, TimestampNs update_ns) noexcept {
    return orders_shm_mutate_slot(shm, index, [&](OrderSlot& slot) {
        slot.stage = stage;
        slot.last_update_ns = update_ns;
    });
}

inline bool orders_shm_append(orders_shm_layout* shm, const OrderRequest& request, OrderSlotState stage,
    order_slot_source_t source, TimestampNs update_ns, OrderIndex& out_index) noexcept {
    if (!orders_shm_try_allocate(shm, out_index)) {
        return false;
    }
    return orders_shm_write_order(shm, out_index, request, stage, source, update_ns);
}

inline bool orders_shm_read_snapshot(const orders_shm_layout* shm, OrderIndex index, order_slot_snapshot& out) noexcept {
    if (!orders_shm_index_exists(shm, index)) {
        return false;
    }

    const OrderSlot& slot = shm->slots[index];
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        const uint64_t seq0 = slot.seq.load(std::memory_order_acquire);
        if ((seq0 & 1ULL) != 0U) {
            continue;
        }

        order_slot_snapshot snapshot;
        snapshot.request = slot.request;
        snapshot.stage = slot.stage;
        snapshot.source = slot.source;
        snapshot.last_update_ns = slot.last_update_ns;

        std::atomic_thread_fence(std::memory_order_acquire);
        const uint64_t seq1 = slot.seq.load(std::memory_order_acquire);
        if (seq0 == seq1 && (seq1 & 1ULL) == 0U) {
            out = snapshot;
            return true;
        }
    }

    return false;
}

}  // namespace acct_service
