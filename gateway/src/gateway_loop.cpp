#include "gateway_loop.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <thread>

#include "common/error.hpp"
#include "common/log.hpp"
#include "order_mapper.hpp"
#include "response_mapper.hpp"
#include "shm/orders_shm.hpp"

namespace acct_service::gateway {

namespace {

// 回报队列写入失败时的本地重试次数。
constexpr uint32_t kResponsePushAttempts = 3;

}  // namespace

gateway_loop::gateway_loop(const gateway_config& config, downstream_shm_layout* downstream_shm, trades_shm_layout* trades_shm,
    orders_shm_layout* orders_shm, broker_api::IBrokerAdapter& adapter)
    : config_(config),
      downstream_shm_(downstream_shm),
      trades_shm_(trades_shm),
      orders_shm_(orders_shm),
      adapter_(adapter) {}

int gateway_loop::run() {
    // 启动前先校验共享内存依赖。
    if (!downstream_shm_ || !trades_shm_ || !orders_shm_) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::core, error_code::ComponentUnavailable, "gateway_loop", "shared memory not available", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return 1;
    }

    running_.store(true, std::memory_order_release);
    last_stats_print_ns_ = now_ns();

    // 单线程循环：重试 -> 新单 -> 回报。
    while (running_.load(std::memory_order_acquire)) {
        ++stats_.loop_iterations;

        bool did_work = false;
        did_work = process_retry_queue() || did_work;
        did_work = process_orders(config_.poll_batch_size) || did_work;
        did_work = process_events(config_.poll_batch_size) || did_work;

        if (!did_work) {
            ++stats_.idle_iterations;
            if (config_.idle_sleep_us > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(config_.idle_sleep_us));
            }
        }

        if (config_.stats_interval_ms > 0) {
            const timestamp_ns_t now = now_ns();
            const uint64_t interval_ns = static_cast<uint64_t>(config_.stats_interval_ms) * 1000000ULL;
            if (now >= last_stats_print_ns_ + interval_ns) {
                print_periodic_stats();
                last_stats_print_ns_ = now;
            }
        }
    }

    return 0;
}

void gateway_loop::stop() noexcept { running_.store(false, std::memory_order_release); }

const gateway_stats& gateway_loop::stats() const noexcept { return stats_; }

bool gateway_loop::process_retry_queue() {
    if (retry_queue_.empty()) {
        return false;
    }

    bool did_work = false;
    const timestamp_ns_t now = now_ns();
    const std::size_t count = retry_queue_.size();

    // 只处理本轮已存在的重试项，避免长时间占用循环。
    for (std::size_t i = 0; i < count; ++i) {
        retry_item item = retry_queue_.front();
        retry_queue_.pop_front();

        if (item.next_retry_at_ns > now) {
            retry_queue_.push_back(item);
            continue;
        }

        did_work = true;
        submit_request(item.request, item.attempts);
    }

    stats_.retry_queue_size = retry_queue_.size();
    return did_work;
}

bool gateway_loop::process_orders(std::size_t batch_limit) {
    if (batch_limit == 0) {
        return false;
    }

    bool did_work = false;
    std::size_t processed = 0;
    order_index_t index = kInvalidOrderIndex;

    // 批量消费下游订单，减少每轮函数调用开销。
    while (processed < batch_limit && downstream_shm_->order_queue.try_pop(index)) {
        ++processed;
        did_work = true;
        ++stats_.orders_received;
        stats_.last_order_time_ns = now_ns();

        order_slot_snapshot snapshot;
        if (!orders_shm_read_snapshot(orders_shm_, index, snapshot)) {
            ++stats_.orders_failed;
            error_status status = ACCT_MAKE_ERROR(error_domain::order, error_code::OrderNotFound, "gateway_loop",
                "failed to read downstream order slot", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            continue;
        }

        (void)orders_shm_update_stage(orders_shm_, index, order_slot_stage_t::DownstreamDequeued, now_ns());
        const order_request& request = snapshot.request;

        broker_api::broker_order_request mapped;
        if (!map_order_request_to_broker(request, mapped)) {
            ++stats_.orders_failed;
            emit_trader_error(request.internal_order_id, request.internal_security_id, request.trade_side);
            continue;
        }

        submit_request(mapped, 0);
    }

    return did_work;
}

bool gateway_loop::process_events(std::size_t batch_limit) {
    if (batch_limit == 0) {
        return false;
    }

    constexpr std::size_t kMaxEventBatch = 256;
    std::array<broker_api::broker_event, kMaxEventBatch> events{};
    const std::size_t max_events = std::min<std::size_t>(batch_limit, events.size());
    const std::size_t count = adapter_.poll_events(events.data(), max_events);
    if (count == 0) {
        return false;
    }

    stats_.events_received += count;

    // 将适配器事件逐条映射为 trade_response。
    for (std::size_t i = 0; i < count; ++i) {
        trade_response response;
        if (!map_broker_event_to_trade_response(events[i], response)) {
            ++stats_.responses_dropped;
            continue;
        }

        if (!push_response(response)) {
            ++stats_.responses_dropped;
            stop();
            error_status status = ACCT_MAKE_ERROR(
                error_domain::order, error_code::QueuePushFailed, "gateway_loop", "failed to push trade response", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            break;
        }

        ++stats_.responses_pushed;
    }

    return true;
}

void gateway_loop::submit_request(const broker_api::broker_order_request& request, uint32_t attempts) {
    const broker_api::send_result result = adapter_.submit(request);
    if (result.accepted) {
        ++stats_.orders_submitted;
        return;
    }

    // 可重试错误：按策略延后重试。
    if (result.retryable && attempts < config_.max_retry_attempts) {
        retry_item item;
        item.request = request;
        item.attempts = attempts + 1;
        item.next_retry_at_ns = now_ns() + static_cast<uint64_t>(config_.retry_interval_us) * 1000ULL;
        retry_queue_.push_back(item);
        ++stats_.retries_scheduled;
        stats_.retry_queue_size = retry_queue_.size();
        return;
    }

    // 不可重试或重试耗尽：回写 TraderError。
    ++stats_.orders_failed;
    if (attempts > 0) {
        ++stats_.retries_exhausted;
    }
    internal_security_id_t internal_security_id;
    internal_security_id.assign(request.internal_security_id);
    emit_trader_error(request.internal_order_id, internal_security_id, to_order_side(request.trade_side));
}

bool gateway_loop::push_response(const trade_response& response) {
    // 回报写队列满时短暂重试，尽量避免丢失状态。
    for (uint32_t attempt = 0; attempt < kResponsePushAttempts; ++attempt) {
        if (trades_shm_->response_queue.try_push(response)) {
            return true;
        }
        if (config_.retry_interval_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(config_.retry_interval_us));
        }
    }
    return false;
}

void gateway_loop::emit_trader_error(
    internal_order_id_t internal_order_id, internal_security_id_t internal_security_id, trade_side_t side_value) {
    if (internal_order_id == 0) {
        return;
    }

    trade_response response{};
    response.internal_order_id = internal_order_id;
    response.internal_security_id = internal_security_id;
    response.trade_side = side_value;
    response.new_status = order_status_t::TraderError;
    response.recv_time_ns = now_ns();

    // 尝试把失败状态写回上游，保证链路可观测。
    if (push_response(response)) {
        ++stats_.responses_pushed;
    } else {
        ++stats_.responses_dropped;
    }
}

void gateway_loop::print_periodic_stats() {
    std::fprintf(stderr,
        "[gateway] loops=%llu idle=%llu received=%llu submitted=%llu failed=%llu retry_q=%llu events=%llu responses=%llu dropped=%llu\n",
        static_cast<unsigned long long>(stats_.loop_iterations),
        static_cast<unsigned long long>(stats_.idle_iterations),
        static_cast<unsigned long long>(stats_.orders_received),
        static_cast<unsigned long long>(stats_.orders_submitted),
        static_cast<unsigned long long>(stats_.orders_failed),
        static_cast<unsigned long long>(stats_.retry_queue_size),
        static_cast<unsigned long long>(stats_.events_received),
        static_cast<unsigned long long>(stats_.responses_pushed),
        static_cast<unsigned long long>(stats_.responses_dropped));
}

}  // namespace acct_service::gateway
