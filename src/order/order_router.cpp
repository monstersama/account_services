#include "order/order_router.hpp"

#include "common/error.hpp"
#include "common/log.hpp"

namespace acct_service {

order_router::order_router(order_book& book, downstream_shm_layout* shm, const split_config& config)
    : order_book_(book), downstream_shm_(shm), splitter_(config) {
    splitter_.set_order_id_generator([this]() { return order_book_.next_order_id(); });
}

bool order_router::route_order(order_entry& entry) {
    if (entry.request.order_type == order_type_t::Cancel) {
        return route_cancel(
            entry.request.orig_internal_order_id, entry.request.internal_order_id, entry.request.md_time_driven);
    }

    ++stats_.orders_received;
    stats_.last_order_time = now_ns();

    if (splitter_.should_split(entry.request)) {
        return handle_split_order(entry);
    }

    if (!send_to_downstream(entry.request)) {
        ++stats_.orders_rejected;
        ++stats_.queue_full_count;
        order_book_.update_status(entry.request.internal_order_id, order_status_t::TraderError);
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::QueuePushFailed, "order_router", "failed to push order to downstream", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    ++stats_.orders_sent;
    order_book_.update_status(entry.request.internal_order_id, order_status_t::TraderSubmitted);
    return true;
}

std::size_t order_router::route_orders(std::vector<order_entry*>& entries) {
    std::size_t success_count = 0;
    for (order_entry* entry : entries) {
        if (entry && route_order(*entry)) {
            ++success_count;
        }
    }
    return success_count;
}

bool order_router::route_cancel(internal_order_id_t orig_id, internal_order_id_t cancel_id, md_time_t time) {
    ++stats_.orders_received;
    stats_.last_order_time = now_ns();

    const std::vector<internal_order_id_t> children = order_book_.get_children(orig_id);
    if (!children.empty()) {
        bool any_sent = false;
        bool any_failed = false;
        bool used_cancel_id = false;

        for (internal_order_id_t child_id : children) {
            const order_entry* child = order_book_.find_order(child_id);
            if (!child || child->request.order_type != order_type_t::New || child->is_terminal()) {
                continue;
            }

            internal_order_id_t child_cancel_id = cancel_id;
            if (used_cancel_id) {
                child_cancel_id = order_book_.next_order_id();
            }
            used_cancel_id = true;

            order_request cancel_request;
            cancel_request.init_cancel(child_cancel_id, time, child_id);
            cancel_request.order_status.store(order_status_t::TraderPending, std::memory_order_relaxed);

            order_entry cancel_entry{};
            cancel_entry.request = cancel_request;
            cancel_entry.submit_time_ns = now_ns();
            cancel_entry.last_update_ns = cancel_entry.submit_time_ns;
            cancel_entry.strategy_id = child->strategy_id;
            cancel_entry.risk_result = risk_result_t::Pass;
            cancel_entry.retry_count = 0;
            cancel_entry.is_split_child = true;
            cancel_entry.parent_order_id = orig_id;

            if (!order_book_.add_order(cancel_entry)) {
                any_failed = true;
                ++stats_.orders_rejected;
                error_status status = ACCT_MAKE_ERROR(error_domain::order, error_code::OrderBookFull, "order_router",
                    "failed to add child cancel order", 0);
                record_error(status);
                ACCT_LOG_ERROR_STATUS(status);
                continue;
            }

            if (!send_to_downstream(cancel_request)) {
                any_failed = true;
                ++stats_.orders_rejected;
                ++stats_.queue_full_count;
                order_book_.update_status(child_cancel_id, order_status_t::TraderError);
                error_status status = ACCT_MAKE_ERROR(error_domain::order, error_code::QueuePushFailed, "order_router",
                    "failed to send child cancel to downstream", 0);
                record_error(status);
                ACCT_LOG_ERROR_STATUS(status);
                continue;
            }

            ++stats_.orders_sent;
            any_sent = true;
            order_book_.update_status(child_cancel_id, order_status_t::TraderSubmitted);
        }

        if (any_failed) {
            order_book_.update_status(orig_id, order_status_t::TraderError);
        }

        return any_sent;
    }

    order_request cancel_request;
    cancel_request.init_cancel(cancel_id, time, orig_id);
    cancel_request.order_status.store(order_status_t::TraderPending, std::memory_order_relaxed);

    order_entry cancel_entry{};
    cancel_entry.request = cancel_request;
    cancel_entry.submit_time_ns = now_ns();
    cancel_entry.last_update_ns = cancel_entry.submit_time_ns;
    cancel_entry.risk_result = risk_result_t::Pass;
    cancel_entry.retry_count = 0;
    cancel_entry.is_split_child = false;
    cancel_entry.parent_order_id = 0;

    if (!order_book_.add_order(cancel_entry)) {
        ++stats_.orders_rejected;
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderBookFull, "order_router", "failed to add cancel order", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (!send_to_downstream(cancel_request)) {
        ++stats_.orders_rejected;
        ++stats_.queue_full_count;
        order_book_.update_status(cancel_id, order_status_t::TraderError);
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::QueuePushFailed, "order_router", "failed to send cancel request", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    ++stats_.orders_sent;
    order_book_.update_status(cancel_id, order_status_t::TraderSubmitted);
    return true;
}

const router_stats& order_router::stats() const noexcept { return stats_; }

void order_router::reset_stats() noexcept { stats_ = router_stats{}; }

bool order_router::send_to_downstream(const order_request& request) {
    if (!downstream_shm_) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::ComponentUnavailable, "order_router", "downstream shm unavailable", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    const bool pushed = downstream_shm_->order_queue.try_push(request);
    if (pushed) {
        downstream_shm_->header.last_update = now_ns();
    }
    return pushed;
}

bool order_router::handle_split_order(order_entry& parent) {
    ++stats_.orders_split;

    split_result result = splitter_.split(parent.request);
    if (!result.success || result.child_orders.empty()) {
        ++stats_.orders_rejected;
        order_book_.update_status(parent.request.internal_order_id, order_status_t::TraderError);
        error_status status =
            ACCT_MAKE_ERROR(error_domain::order, error_code::SplitFailed, "order_router", "split order failed", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    bool any_sent = false;
    bool any_failed = false;

    for (order_request& child_request : result.child_orders) {
        order_entry child_entry{};
        child_entry.request = child_request;
        child_entry.submit_time_ns = now_ns();
        child_entry.last_update_ns = child_entry.submit_time_ns;
        child_entry.strategy_id = parent.strategy_id;
        child_entry.risk_result = parent.risk_result;
        child_entry.retry_count = 0;
        child_entry.is_split_child = true;
        child_entry.parent_order_id = parent.request.internal_order_id;
        child_entry.request.order_status.store(order_status_t::TraderPending, std::memory_order_relaxed);

        if (!order_book_.add_order(child_entry)) {
            any_failed = true;
            ++stats_.orders_rejected;
            error_status status = ACCT_MAKE_ERROR(
                error_domain::order, error_code::OrderBookFull, "order_router", "failed to add child order", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            continue;
        }

        if (!send_to_downstream(child_entry.request)) {
            any_failed = true;
            ++stats_.orders_rejected;
            ++stats_.queue_full_count;
            order_book_.update_status(child_entry.request.internal_order_id, order_status_t::TraderError);
            error_status status = ACCT_MAKE_ERROR(
                error_domain::order, error_code::QueuePushFailed, "order_router", "failed to send child order", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            continue;
        }

        ++stats_.orders_sent;
        any_sent = true;
        order_book_.update_status(child_entry.request.internal_order_id, order_status_t::TraderSubmitted);
    }

    if (any_failed) {
        order_book_.update_status(parent.request.internal_order_id, order_status_t::TraderError);
    }

    return any_sent;
}

}  // namespace acct_service
