#include "core/event_loop.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#include "common/error.hpp"
#include "common/log.hpp"

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace acct_service {

namespace {

std::atomic<event_loop*> g_active_loop{nullptr};

void signal_handler(int signo) {
    (void)signo;
    event_loop* loop = g_active_loop.load(std::memory_order_acquire);
    if (loop) {
        loop->stop();
    }
}

bool is_terminal_status(order_status_t status) {
    switch (status) {
        case order_status_t::RiskControllerRejected:
        case order_status_t::TraderRejected:
        case order_status_t::TraderError:
        case order_status_t::BrokerRejected:
        case order_status_t::MarketRejected:
        case order_status_t::Finished:
        case order_status_t::Unknown:
            return true;
        default:
            return false;
    }
}

}  // namespace

double event_loop_stats::avg_latency_ns() const {
    if (latency_samples == 0) {
        return 0.0;
    }
    return static_cast<double>(total_latency_ns) / static_cast<double>(latency_samples);
}

event_loop::event_loop(const event_loop_config& config, upstream_shm_layout* upstream_shm,
    downstream_shm_layout* downstream_shm, order_book& order_book, order_router& router, position_manager& positions,
    risk_manager& risk)
    : config_(config),
      upstream_shm_(upstream_shm),
      downstream_shm_(downstream_shm),
      order_book_(order_book),
      router_(router),
      positions_(positions),
      risk_(risk) {}

event_loop::~event_loop() {
    stop();

    event_loop* expected = this;
    g_active_loop.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

void event_loop::run() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    if (config_.pin_cpu && config_.cpu_core >= 0) {
        set_cpu_affinity(config_.cpu_core);
    }

    setup_signal_handlers();
    g_active_loop.store(this, std::memory_order_release);

    stats_.start_time = now_ns();
    last_stats_time_ = now_monotonic_ns();

    while (running_.load(std::memory_order_acquire)) {
        loop_iteration();
        if (should_stop_service()) {
            running_.store(false, std::memory_order_release);
        }
    }

    running_.store(false, std::memory_order_release);

    event_loop* active = this;
    g_active_loop.compare_exchange_strong(active, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

void event_loop::stop() noexcept { running_.store(false, std::memory_order_release); }

bool event_loop::is_running() const noexcept { return running_.load(std::memory_order_acquire); }

const event_loop_stats& event_loop::stats() const noexcept { return stats_; }

void event_loop::reset_stats() noexcept { stats_ = event_loop_stats{}; }

void event_loop::setup_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void event_loop::set_trades_shm(trades_shm_layout* trades_shm) noexcept { trades_shm_ = trades_shm; }

void event_loop::loop_iteration() {
    const timestamp_ns_t start = now_monotonic_ns();
    ++stats_.total_iterations;

    const std::size_t orders = process_upstream_orders();
    const std::size_t responses = process_downstream_responses();

    if (orders == 0 && responses == 0) {
        ++stats_.idle_iterations;
        if (!config_.busy_polling && config_.idle_sleep_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(config_.idle_sleep_us));
        }
    }

    const timestamp_ns_t now = now_monotonic_ns();
    if (config_.stats_interval_ms > 0) {
        const timestamp_ns_t interval_ns = static_cast<timestamp_ns_t>(config_.stats_interval_ms) * 1000000ULL;
        if (now >= last_stats_time_ && now - last_stats_time_ >= interval_ns) {
            print_periodic_stats();
            last_stats_time_ = now;
        }
    }

    update_latency_stats(start, now);
}

std::size_t event_loop::process_upstream_orders() {
    if (!upstream_shm_) {
        return 0;
    }

    const std::size_t batch_limit = (config_.poll_batch_size == 0) ? 1 : config_.poll_batch_size;
    std::size_t processed = 0;

    order_request request;
    while (processed < batch_limit && upstream_shm_->strategy_order_queue.try_pop(request)) {
        handle_order_request(request);
        ++processed;
    }

    if (processed > 0) {
        stats_.orders_processed += processed;
        stats_.last_order_time = now_ns();
    }

    return processed;
}

std::size_t event_loop::process_downstream_responses() {
    if (!trades_shm_) {
        return 0;
    }

    const std::size_t batch_limit = (config_.poll_batch_size == 0) ? 1 : config_.poll_batch_size;
    std::size_t processed = 0;

    trade_response response;
    while (processed < batch_limit && trades_shm_->response_queue.try_pop(response)) {
        handle_trade_response(response);
        ++processed;
    }

    if (processed > 0) {
        stats_.responses_processed += processed;
        stats_.last_response_time = now_ns();
    }

    return processed;
}

void event_loop::handle_order_request(order_request& request) {
    if (request.internal_order_id == 0) {
        request.internal_order_id = order_book_.next_order_id();
    }

    order_entry entry{};
    entry.request = request;
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<strategy_id_t>(0);
    entry.risk_result = risk_result_t::Pass;
    entry.retry_count = 0;
    entry.is_split_child = false;
    entry.parent_order_id = 0;

    if (!order_book_.add_order(entry)) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderBookFull, "event_loop", "order_book add_order failed", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return;
    }

    order_book_.update_status(request.internal_order_id, order_status_t::RiskControllerPending);

    if (request.order_type == order_type_t::New) {
        const risk_check_result risk_result = risk_.check_order(request);

        if (order_entry* active = order_book_.find_order(request.internal_order_id)) {
            active->risk_result = risk_result.code;
        }

        if (!risk_result.passed()) {
            order_book_.update_status(request.internal_order_id, order_status_t::RiskControllerRejected);
            return;
        }

        order_book_.update_status(request.internal_order_id, order_status_t::RiskControllerAccepted);
    } else {
        order_book_.update_status(request.internal_order_id, order_status_t::RiskControllerAccepted);
    }

    order_entry* active = order_book_.find_order(request.internal_order_id);
    if (!active || !router_.route_order(*active)) {
        order_book_.update_status(request.internal_order_id, order_status_t::TraderError);
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::RouteFailed, "event_loop", "route_order failed", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
    }
}

void event_loop::handle_trade_response(const trade_response& response) {
    if (response.internal_order_id == 0) {
        return;
    }

    order_book_.update_status(response.internal_order_id, response.new_status);

    if (response.volume_traded > 0) {
        order_book_.update_trade(response.internal_order_id, response.volume_traded, response.dprice_traded,
            response.dvalue_traded, response.dfee);

        const order_entry* order = order_book_.find_order(response.internal_order_id);
        if (order && order->request.order_type == order_type_t::New) {
            const internal_security_id_t security_id =
                (response.internal_security_id != 0) ? response.internal_security_id : order->request.internal_security_id;

            if (security_id != 0) {
                if (!positions_.get_position(security_id) && !order->request.security_id.empty()) {
                    const internal_security_id_t added =
                        positions_.add_security(order->request.security_id.view(), order->request.security_id.view(), order->request.market);
                    if (added == 0) {
                        error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::PositionUpdateFailed,
                            "event_loop", "failed to create missing position row", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    } else if (added != security_id) {
                        error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::OrderInvariantBroken,
                            "event_loop", "security id mismatch while creating position row", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    }
                }

                if (response.trade_side == trade_side_t::Buy) {
                    if (!positions_.add_position(
                            security_id, response.volume_traded, response.dprice_traded, response.internal_order_id)) {
                        error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::PositionUpdateFailed,
                            "event_loop", "failed to add position from trade response", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    }
                } else if (response.trade_side == trade_side_t::Sell) {
                    if (!positions_.deduct_position(
                            security_id, response.volume_traded, response.dvalue_traded, response.internal_order_id)) {
                        error_status status = ACCT_MAKE_ERROR(error_domain::portfolio, error_code::PositionUpdateFailed,
                            "event_loop", "failed to deduct position from trade response", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    }
                }
            }
        }
    }

    if (is_terminal_status(response.new_status)) {
        (void)order_book_.archive_order(response.internal_order_id);
    }
}

void event_loop::update_latency_stats(timestamp_ns_t start, timestamp_ns_t end) {
    if (end < start) {
        return;
    }

    const uint64_t latency = end - start;
    stats_.min_latency_ns = (latency < stats_.min_latency_ns) ? latency : stats_.min_latency_ns;
    stats_.max_latency_ns = (latency > stats_.max_latency_ns) ? latency : stats_.max_latency_ns;
    stats_.total_latency_ns += latency;
    ++stats_.latency_samples;
}

void event_loop::print_periodic_stats() {
    std::fprintf(stderr,
        "[event_loop] iter=%llu orders=%llu responses=%llu idle=%llu avg_ns=%.0f min_ns=%llu max_ns=%llu\n",
        static_cast<unsigned long long>(stats_.total_iterations), static_cast<unsigned long long>(stats_.orders_processed),
        static_cast<unsigned long long>(stats_.responses_processed),
        static_cast<unsigned long long>(stats_.idle_iterations), stats_.avg_latency_ns(),
        static_cast<unsigned long long>(stats_.min_latency_ns == UINT64_MAX ? 0 : stats_.min_latency_ns),
        static_cast<unsigned long long>(stats_.max_latency_ns));
}

void event_loop::set_cpu_affinity(int core) {
#if defined(__linux__)
    if (core < 0) {
        return;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    (void)sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#else
    (void)core;
#endif
}

}  // namespace acct_service
