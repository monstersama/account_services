#include "order/order_router.hpp"

#include "common/error.hpp"
#include "common/log.hpp"
#include "order/order_recovery.hpp"
#include "shm/orders_shm.hpp"

namespace acct_service {

order_router::order_router(OrderBook& book, downstream_shm_layout* downstream_shm, orders_shm_layout* orders_shm,
                           upstream_shm_layout* upstream_shm)
    : order_book_(book), downstream_shm_(downstream_shm), orders_shm_(orders_shm), upstream_shm_(upstream_shm) {}

bool order_router::route_order(OrderEntry& entry) {
    if (entry.request.order_type == OrderType::Cancel) {
        return route_cancel(entry.request.orig_internal_order_id, entry.request.internal_order_id,
                            entry.request.md_time_driven);
    }

    ++stats_.orders_received;
    stats_.last_order_time = now_ns();

    if (entry.shm_order_index == kInvalidOrderIndex) {
        ++stats_.orders_rejected;
        order_book_.update_state(entry.request.internal_order_id, OrderState::TraderError);
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderInvariantBroken, "order_router",
                                             "missing order shm index", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (!send_to_downstream(entry.shm_order_index)) {
        ++stats_.orders_rejected;
        ++stats_.queue_full_count;
        order_book_.update_state(entry.request.internal_order_id, OrderState::TraderError);
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::QueuePushFailed, "order_router",
                                             "failed to push order to downstream", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    ++stats_.orders_sent;
    order_book_.update_state(entry.request.internal_order_id, OrderState::TraderSubmitted);
    return true;
}

std::size_t order_router::route_orders(std::vector<OrderEntry*>& entries) {
    std::size_t success_count = 0;
    for (OrderEntry* entry : entries) {
        if (entry && route_order(*entry)) {
            ++success_count;
        }
    }
    return success_count;
}

bool order_router::route_cancel(InternalOrderId orig_id, InternalOrderId cancel_id, MdTime time) {
    ++stats_.orders_received;
    stats_.last_order_time = now_ns();

    const std::vector<InternalOrderId> children = order_book_.get_children(orig_id);
    if (!children.empty()) {
        bool any_sent = false;
        bool any_failed = false;
        bool used_cancel_id = false;

        for (InternalOrderId child_id : children) {
            const OrderEntry* child = order_book_.find_order(child_id);
            if (!child || child->request.order_type != OrderType::New || child->is_terminal()) {
                continue;
            }

            InternalOrderId child_cancel_id = cancel_id;
            if (used_cancel_id) {
                child_cancel_id = allocate_internal_order_id();
            }
            used_cancel_id = true;

            OrderRequest cancel_request;
            cancel_request.init_cancel(child_cancel_id, time, child_id);
            cancel_request.order_state.store(OrderState::TraderPending, std::memory_order_relaxed);

            OrderIndex cancel_index = kInvalidOrderIndex;
            if (!create_internal_order_slot(cancel_request, OrderSlotState::UpstreamDequeued, cancel_index,
                                            order_slot_source_t::AccountInternal)) {
                any_failed = true;
                ++stats_.orders_rejected;
                ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderPoolFull, "order_router",
                                                     "failed to allocate child cancel order slot", 0);
                record_error(status);
                ACCT_LOG_ERROR_STATUS(status);
                continue;
            }

            OrderEntry cancel_entry{};
            cancel_entry.request = cancel_request;
            cancel_entry.submit_time_ns = now_ns();
            cancel_entry.last_update_ns = cancel_entry.submit_time_ns;
            cancel_entry.strategy_id = child->strategy_id;
            cancel_entry.risk_result = RiskResult::Pass;
            cancel_entry.retry_count = 0;
            cancel_entry.is_split_child = true;
            cancel_entry.parent_order_id = orig_id;
            cancel_entry.shm_order_index = cancel_index;

            if (!order_book_.add_order(cancel_entry)) {
                (void)orders_shm_update_stage(orders_shm_, cancel_index, OrderSlotState::QueuePushFailed, now_ns());
                any_failed = true;
                ++stats_.orders_rejected;
                ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderBookFull, "order_router",
                                                     "failed to add child cancel order", 0);
                record_error(status);
                ACCT_LOG_ERROR_STATUS(status);
                continue;
            }

            if (!send_to_downstream(cancel_index)) {
                any_failed = true;
                ++stats_.orders_rejected;
                ++stats_.queue_full_count;
                order_book_.update_state(child_cancel_id, OrderState::TraderError);
                ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::QueuePushFailed, "order_router",
                                                     "failed to send child cancel to downstream", 0);
                record_error(status);
                ACCT_LOG_ERROR_STATUS(status);
                continue;
            }

            ++stats_.orders_sent;
            any_sent = true;
            order_book_.update_state(child_cancel_id, OrderState::TraderSubmitted);
        }

        if (any_failed) {
            order_book_.update_state(orig_id, OrderState::TraderError);
        }

        return any_sent;
    }

    OrderRequest cancel_request;
    cancel_request.init_cancel(cancel_id, time, orig_id);
    cancel_request.order_state.store(OrderState::TraderPending, std::memory_order_relaxed);

    OrderIndex cancel_index = kInvalidOrderIndex;
    if (!create_internal_order_slot(cancel_request, OrderSlotState::UpstreamDequeued, cancel_index,
                                    order_slot_source_t::AccountInternal)) {
        ++stats_.orders_rejected;
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderPoolFull, "order_router",
                                             "failed to allocate cancel order slot", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    OrderEntry cancel_entry{};
    cancel_entry.request = cancel_request;
    cancel_entry.submit_time_ns = now_ns();
    cancel_entry.last_update_ns = cancel_entry.submit_time_ns;
    cancel_entry.risk_result = RiskResult::Pass;
    cancel_entry.retry_count = 0;
    cancel_entry.is_split_child = false;
    cancel_entry.parent_order_id = 0;
    cancel_entry.shm_order_index = cancel_index;

    if (!order_book_.add_order(cancel_entry)) {
        (void)orders_shm_update_stage(orders_shm_, cancel_index, OrderSlotState::QueuePushFailed, now_ns());
        ++stats_.orders_rejected;
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderBookFull, "order_router",
                                             "failed to add cancel order", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (!send_to_downstream(cancel_index)) {
        ++stats_.orders_rejected;
        ++stats_.queue_full_count;
        order_book_.update_state(cancel_id, OrderState::TraderError);
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::QueuePushFailed, "order_router",
                                             "failed to send cancel request", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    ++stats_.orders_sent;
    order_book_.update_state(cancel_id, OrderState::TraderSubmitted);
    return true;
}

// 把执行引擎生成的子单接入既有订单簿/订单池/下游队列，避免维护第二套发单链。
bool order_router::submit_internal_order(OrderEntry& entry) {
    ++stats_.orders_received;
    stats_.last_order_time = now_ns();

    if (entry.request.internal_order_id == 0) {
        entry.request.internal_order_id = allocate_internal_order_id();
    }
    if (entry.submit_time_ns == 0) {
        entry.submit_time_ns = now_ns();
    }
    if (entry.last_update_ns == 0) {
        entry.last_update_ns = entry.submit_time_ns;
    }
    entry.request.order_state.store(OrderState::TraderPending, std::memory_order_relaxed);

    if (entry.shm_order_index == kInvalidOrderIndex &&
        !create_internal_order_slot(entry.request, OrderSlotState::UpstreamDequeued, entry.shm_order_index,
                                    order_slot_source_t::AccountInternal)) {
        ++stats_.orders_rejected;
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderPoolFull, "order_router",
                                             "failed to allocate internal child slot", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (!order_book_.add_order(entry)) {
        (void)orders_shm_update_stage(orders_shm_, entry.shm_order_index, OrderSlotState::QueuePushFailed, now_ns());
        ++stats_.orders_rejected;
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderBookFull, "order_router",
                                             "failed to add internal child", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (!send_to_downstream(entry.shm_order_index)) {
        ++stats_.orders_rejected;
        ++stats_.queue_full_count;
        order_book_.update_state(entry.request.internal_order_id, OrderState::TraderError);
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::QueuePushFailed, "order_router",
                                             "failed to send internal child", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    ++stats_.orders_sent;
    order_book_.update_state(entry.request.internal_order_id, OrderState::TraderSubmitted);
    return true;
}

bool order_router::recover_downstream_active_orders(const upstream_shm_layout* upstream_shm) {
    if (!orders_shm_) {
        return false;
    }
    return recover_downstream_active_orders_from_shm(orders_shm_, upstream_shm, order_book_);
}

const router_stats& order_router::stats() const noexcept { return stats_; }

void order_router::reset_stats() noexcept { stats_ = router_stats{}; }

InternalOrderId order_router::allocate_internal_order_id() noexcept {
    if (upstream_shm_) {
        return upstream_shm_->header.next_order_id.fetch_add(1, std::memory_order_relaxed);
    }
    return order_book_.next_order_id();
}

bool order_router::send_to_downstream(OrderIndex index) {
    if (!downstream_shm_ || !orders_shm_) {
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::ComponentUnavailable, "order_router",
                                             "downstream/orders shm unavailable", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    const bool pushed = downstream_shm_->order_queue.try_push(index);
    if (pushed) {
        downstream_shm_->header.last_update = now_ns();
        (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::DownstreamQueued, now_ns());
    } else {
        (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::QueuePushFailed, now_ns());
    }
    return pushed;
}

bool order_router::create_internal_order_slot(const OrderRequest& request, OrderSlotState stage, OrderIndex& out_index,
                                              order_slot_source_t source) {
    if (!orders_shm_) {
        return false;
    }
    return orders_shm_append(orders_shm_, request, stage, source, now_ns(), out_index);
}

}  // namespace acct_service
