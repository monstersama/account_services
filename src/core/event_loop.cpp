#include "core/event_loop.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "common/error.hpp"
#include "common/log.hpp"
#include "common/security_identity.hpp"
#include "execution/execution_engine.hpp"
#include "portfolio/account_info.hpp"
#include "shm/orders_shm.hpp"

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace acct_service {

namespace {

std::atomic<EventLoop*> g_active_loop{nullptr};

void signal_handler(int signo) {
    (void)signo;
    EventLoop* loop = g_active_loop.load(std::memory_order_acquire);
    if (loop) {
        loop->stop();
    }
}

bool is_terminal_state(OrderState status) {
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

bool try_calculate_trade_value(Volume volume, DPrice price, DValue& out_value) {
    const __uint128_t value = static_cast<__uint128_t>(volume) * static_cast<__uint128_t>(price);
    if (value > static_cast<__uint128_t>(std::numeric_limits<DValue>::max())) {
        return false;
    }
    out_value = static_cast<DValue>(value);
    return true;
}

bool try_total_with_fee(DValue amount, DValue fee, DValue& out_total) {
    const __uint128_t total = static_cast<__uint128_t>(amount) + static_cast<__uint128_t>(fee);
    if (total > static_cast<__uint128_t>(std::numeric_limits<DValue>::max())) {
        return false;
    }
    out_total = static_cast<DValue>(total);
    return true;
}

}  // namespace

double event_loop_stats::avg_latency_ns() const {
    if (latency_samples == 0) {
        return 0.0;
    }
    return static_cast<double>(total_latency_ns) / static_cast<double>(latency_samples);
}

EventLoop::EventLoop(const EventLoopConfig& config, upstream_shm_layout* upstream_shm,
                     downstream_shm_layout* downstream_shm, trades_shm_layout* trades_shm,
                     orders_shm_layout* orders_shm, OrderBook& OrderBook, order_router& router,
                     PositionManager& positions, RiskManager& risk, const account_info* account_info,
                     ExecutionEngine* execution_engine, OrderEventRecorder* order_event_recorder)
    : config_(config),
      upstream_shm_(upstream_shm),
      downstream_shm_(downstream_shm),
      trades_shm_(trades_shm),
      orders_shm_(orders_shm),
      order_book_(OrderBook),
      router_(router),
      positions_(positions),
      risk_(risk),
      account_info_(account_info),
      execution_engine_(execution_engine),
      order_event_recorder_(order_event_recorder) {
    order_book_.set_change_callback(
        [this](const OrderEntry& entry, order_book_event_t event) { on_order_book_changed(entry, event); });
}

// 为新单补齐手续费估算，避免风控通过但成交结算时手续费不足。
void EventLoop::prepare_order_estimate(OrderRequest& request) const {
    if (request.order_type != OrderType::New || request.dfee_estimate != 0) {
        return;
    }

    DValue order_value = 0;
    if (!try_calculate_trade_value(request.volume_entrust, request.dprice_entrust, order_value) || order_value == 0) {
        return;
    }

    // 优先使用账户费率；单测或缺省场景回退到默认费率模型。
    static const account_info default_account_info{};
    const account_info* fee_model = account_info_ != nullptr ? account_info_ : &default_account_info;
    request.dfee_estimate = fee_model->calculate_fee(request.trade_side, order_value);
}

// 在订单真正下游前冻结买单总额，避免并发新单重复占用同一笔可用资金。
bool EventLoop::reserve_order_resources(OrderEntry& entry) {
    if (entry.request.order_type != OrderType::New || entry.request.trade_side != TradeSide::Buy) {
        return true;
    }

    DValue order_value = 0;
    DValue total = 0;
    if (!try_calculate_trade_value(entry.request.volume_entrust, entry.request.dprice_entrust, order_value) ||
        !try_total_with_fee(order_value, entry.request.dfee_estimate, total)) {
        ACCT_LOG_WARN("EventLoop", "buy order fund reservation overflow");
        return false;
    }
    if (!positions_.freeze_fund(total, entry.request.internal_order_id)) {
        return false;
    }

    entry.fund_frozen = total;
    return true;
}

// 释放订单剩余冻结资金，供发送失败、拒单和撤单完成后的尾款回收使用。
void EventLoop::release_order_resources(OrderEntry& entry) {
    if (entry.request.order_type != OrderType::New || entry.request.trade_side != TradeSide::Buy ||
        entry.fund_frozen == 0) {
        return;
    }
    if (!positions_.unfreeze_fund(entry.fund_frozen, entry.request.internal_order_id)) {
        ACCT_LOG_WARN("EventLoop", "failed to unfreeze remaining buy fund");
        return;
    }

    entry.fund_frozen = 0;
}

// 优先从订单冻结资金中扣减买入成交款；旧订单保留直接扣可用资金的兼容路径。
bool EventLoop::settle_buy_trade_fund(OrderEntry& entry, DValue amount, DValue fee) {
    if (entry.fund_frozen == 0) {
        return positions_.apply_buy_trade_fund(amount, fee, entry.request.internal_order_id);
    }

    DValue total = 0;
    if (!try_total_with_fee(amount, fee, total)) {
        return false;
    }
    if (!positions_.deduct_fund(amount, fee, entry.request.internal_order_id)) {
        return false;
    }

    if (entry.fund_frozen >= total) {
        entry.fund_frozen -= total;
    } else {
        entry.fund_frozen = 0;
    }
    return true;
}

EventLoop::~EventLoop() {
    stop();
    order_book_.set_change_callback({});

    EventLoop* expected = this;
    g_active_loop.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

void EventLoop::run() {
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

    EventLoop* active = this;
    g_active_loop.compare_exchange_strong(active, nullptr, std::memory_order_release, std::memory_order_relaxed);
}

void EventLoop::stop() noexcept { running_.store(false, std::memory_order_release); }

bool EventLoop::is_running() const noexcept { return running_.load(std::memory_order_acquire); }

const event_loop_stats& EventLoop::stats() const noexcept { return stats_; }

void EventLoop::reset_stats() noexcept { stats_ = event_loop_stats{}; }

void EventLoop::setup_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void EventLoop::loop_iteration() {
    const TimestampNs start = now_monotonic_ns();
    ++stats_.total_iterations;

    const std::size_t orders = process_upstream_orders();
    const std::size_t responses = process_downstream_responses();
    if (execution_engine_) {
        execution_engine_->tick(now_ns());
    }
    if (config_.archive_terminal_orders) {
        process_pending_archives(now_ns());
    }

    if (orders == 0 && responses == 0) {
        ++stats_.idle_iterations;
        if (!config_.busy_polling && config_.idle_sleep_us > 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(config_.idle_sleep_us));
        }
    }

    const TimestampNs now = now_monotonic_ns();
    if (config_.stats_interval_ms > 0) {
        const TimestampNs interval_ns = static_cast<TimestampNs>(config_.stats_interval_ms) * 1000000ULL;
        if (now >= last_stats_time_ && now - last_stats_time_ >= interval_ns) {
            print_periodic_stats();
            last_stats_time_ = now;
        }
    }

    update_latency_stats(start, now);
}

std::size_t EventLoop::process_upstream_orders() {
    if (!upstream_shm_ || !orders_shm_) {
        return 0;
    }

    const std::size_t batch_limit = (config_.poll_batch_size == 0) ? 1 : config_.poll_batch_size;
    std::size_t processed = 0;

    OrderIndex order_index = kInvalidOrderIndex;
    while (processed < batch_limit && upstream_shm_->upstream_order_queue.try_pop(order_index)) {
        order_slot_snapshot snapshot;
        if (!orders_shm_read_snapshot(orders_shm_, order_index, snapshot)) {
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "EventLoop",
                                                 "failed to read order slot from upstream index", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            continue;
        }

        (void)orders_shm_update_stage(orders_shm_, order_index, OrderSlotState::UpstreamDequeued, now_ns());
        handle_order_request(order_index, snapshot.request);
        ++processed;
    }

    if (processed > 0) {
        stats_.orders_processed += processed;
        stats_.last_order_time = now_ns();
    }

    return processed;
}

std::size_t EventLoop::process_downstream_responses() {
    if (!trades_shm_) {
        return 0;
    }

    const std::size_t batch_limit = (config_.poll_batch_size == 0) ? 1 : config_.poll_batch_size;
    std::size_t processed = 0;

    TradeResponse response;
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

void EventLoop::handle_order_request(OrderIndex index, OrderRequest& request) {
    if (request.order_type == OrderType::New && request.internal_security_id.empty() && !request.security_id.empty()) {
        if (!build_internal_security_id(request.market, request.security_id.view(), request.internal_security_id)) {
            request.order_state.store(OrderState::TraderError, std::memory_order_release);
            (void)orders_shm_sync_order(orders_shm_, index, request, now_ns());
            (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::QueuePushFailed, now_ns());
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::InvalidParam, "EventLoop",
                                                 "invalid market/security_id for internal key", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return;
        }
    }

    if (request.internal_order_id == 0) {
        request.internal_order_id = order_book_.next_order_id();
        (void)orders_shm_sync_order(orders_shm_, index, request, now_ns());
    }

    prepare_order_estimate(request);

    OrderEntry entry{};
    entry.request = request;
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<StrategyId>(0);
    entry.risk_result = RiskResult::Pass;
    entry.retry_count = 0;
    entry.is_split_child = false;
    entry.parent_order_id = 0;
    entry.shm_order_index = index;

    if (!order_book_.add_order(entry)) {
        (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::QueuePushFailed, now_ns());
        ErrorStatus status =
            ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderBookFull, "EventLoop", "OrderBook add_order failed", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return;
    }
    // 风控检查
    order_book_.update_state(request.internal_order_id, OrderState::RiskControllerPending);

    if (request.order_type == OrderType::New) {
        const risk_check_result risk_result = risk_.check_order(request);

        if (OrderEntry* active = order_book_.find_order(request.internal_order_id)) {
            active->risk_result = risk_result.code;
        }

        if (!risk_result.passed()) {
            order_book_.update_state(request.internal_order_id, OrderState::RiskControllerRejected);
            (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::RiskRejected, now_ns());
            return;
        }

        order_book_.update_state(request.internal_order_id, OrderState::RiskControllerAccepted);
    } else {
        order_book_.update_state(request.internal_order_id, OrderState::RiskControllerAccepted);
    }

    // 风控通过后立刻冻结买单资金，阻断并发订单重复占用可用余额。
    OrderEntry* active = order_book_.find_order(request.internal_order_id);
    if (!active) {
        return;
    }

    // 时间片执行算法交给长期执行引擎管理，避免沿用一次性拆单/冻结路径。
    if (execution_engine_ && execution_engine_->should_manage(active->request)) {
        active->request.active_strategy_claimed = execution_engine_->has_active_strategy() ? 1U : 0U;
        order_book_.update_state(request.internal_order_id, OrderState::TraderPending);
        const ExecutionEngine::SessionStartResult start_result =
            execution_engine_->start_session(index, active->request, active->strategy_id, active->submit_time_ns);
        if (start_result != ExecutionEngine::SessionStartResult::Started) {
            const bool rejected = start_result == ExecutionEngine::SessionStartResult::Unsupported ||
                                  start_result == ExecutionEngine::SessionStartResult::MarketDataUnavailable;
            order_book_.update_state(request.internal_order_id,
                                     rejected ? OrderState::TraderRejected : OrderState::TraderError);
            (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::QueuePushFailed, now_ns());
            ErrorStatus status = ACCT_MAKE_ERROR(
                ErrorDomain::order, rejected ? ErrorCode::InvalidParam : ErrorCode::SplitFailed, "EventLoop",
                (start_result == ExecutionEngine::SessionStartResult::Unsupported)
                    ? "unsupported passive execution algo"
                : (start_result == ExecutionEngine::SessionStartResult::MarketDataUnavailable)
                    ? "managed execution requires ready market data"
                    : "failed to start execution session",
                0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
        }
        return;
    }

    if (!reserve_order_resources(*active)) {
        order_book_.update_state(request.internal_order_id, OrderState::RiskControllerRejected);
        (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::RiskRejected, now_ns());
        ACCT_LOG_WARN("EventLoop", "buy order fund reservation failed after risk pass");
        return;
    }

    // 进入订单路由
    if (!router_.route_order(*active)) {
        release_order_resources(*active);
        order_book_.update_state(request.internal_order_id, OrderState::TraderError);
        (void)orders_shm_update_stage(orders_shm_, index, OrderSlotState::QueuePushFailed, now_ns());
        ErrorStatus status =
            ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::RouteFailed, "EventLoop", "route_order failed", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
    }
}

void EventLoop::on_order_book_changed(const OrderEntry& entry, order_book_event_t event) {
    if (!orders_shm_ || entry.shm_order_index == kInvalidOrderIndex) {
        return;
    }

    const OrderState status = entry.request.order_state.load(std::memory_order_acquire);
    const TimestampNs update_ns = now_ns();

    bool synced = false;
    if (event == order_book_event_t::Archived || is_terminal_state(status)) {
        synced = orders_shm_write_order(orders_shm_, entry.shm_order_index, entry.request, OrderSlotState::Terminal,
                                        order_slot_source_t::AccountInternal, update_ns);
    } else if (status == OrderState::RiskControllerRejected) {
        synced = orders_shm_write_order(orders_shm_, entry.shm_order_index, entry.request, OrderSlotState::RiskRejected,
                                        order_slot_source_t::AccountInternal, update_ns);
    } else {
        synced = orders_shm_sync_order(orders_shm_, entry.shm_order_index, entry.request, update_ns);
    }

    if (!synced) {
        ErrorStatus status_err = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "EventLoop",
                                                 "failed to sync order book snapshot to orders shm", 0);
        record_error(status_err);
        ACCT_LOG_ERROR_STATUS(status_err);
    }

    if (order_event_recorder_) {
        order_event_recorder_->record_order_event(entry, event);
    }
}

void EventLoop::handle_trade_response(const TradeResponse& response) {
    if (execution_engine_) {
        execution_engine_->on_trade_response(response);
    }

    if (response.internal_order_id == 0) {
        return;
    }

    order_book_.update_state(response.internal_order_id, response.new_state);

    OrderEntry* order = order_book_.find_order(response.internal_order_id);

    if (response.volume_traded > 0) {
        order_book_.update_trade(response.internal_order_id, response.volume_traded, response.dprice_traded,
                                 response.dvalue_traded, response.dfee);

        order = order_book_.find_order(response.internal_order_id);
        if (order && order->request.order_type == OrderType::New) {
            if (response.trade_side == TradeSide::Buy) {
                if (!settle_buy_trade_fund(*order, response.dvalue_traded, response.dfee)) {
                    ErrorStatus status =
                        ACCT_MAKE_ERROR(ErrorDomain::portfolio, ErrorCode::PositionUpdateFailed, "EventLoop",
                                        "failed to settle buy fund from trade response", 0);
                    record_error(status);
                    ACCT_LOG_ERROR_STATUS(status);
                }
            } else if (response.trade_side == TradeSide::Sell) {
                if (!positions_.apply_sell_trade_fund(response.dvalue_traded, response.dfee,
                                                      response.internal_order_id)) {
                    ErrorStatus status =
                        ACCT_MAKE_ERROR(ErrorDomain::portfolio, ErrorCode::PositionUpdateFailed, "EventLoop",
                                        "failed to settle sell fund from trade response", 0);
                    record_error(status);
                    ACCT_LOG_ERROR_STATUS(status);
                }
            }

            const InternalSecurityId security_id = response.internal_security_id.empty()
                                                       ? order->request.internal_security_id
                                                       : response.internal_security_id;

            if (!security_id.empty()) {
                if (!positions_.get_position(security_id) && !order->request.security_id.empty()) {
                    const std::string_view position_name = !order->request.internal_security_id.empty()
                                                               ? order->request.internal_security_id.view()
                                                               : security_id.view();
                    const InternalSecurityId added = positions_.add_security(order->request.security_id.view(),
                                                                             position_name, order->request.market);
                    if (added.empty()) {
                        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::portfolio, ErrorCode::PositionUpdateFailed,
                                                             "EventLoop", "failed to create missing position row", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    } else if (added != security_id) {
                        ErrorStatus status =
                            ACCT_MAKE_ERROR(ErrorDomain::portfolio, ErrorCode::OrderInvariantBroken, "EventLoop",
                                            "security id mismatch while creating position row", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    }
                }

                if (response.trade_side == TradeSide::Buy) {
                    if (!positions_.add_position(security_id, response.volume_traded, response.dprice_traded,
                                                 response.internal_order_id)) {
                        ErrorStatus status =
                            ACCT_MAKE_ERROR(ErrorDomain::portfolio, ErrorCode::PositionUpdateFailed, "EventLoop",
                                            "failed to add position from trade response", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    }
                } else if (response.trade_side == TradeSide::Sell) {
                    if (!positions_.deduct_position(security_id, response.volume_traded, response.dvalue_traded,
                                                    response.internal_order_id)) {
                        ErrorStatus status =
                            ACCT_MAKE_ERROR(ErrorDomain::portfolio, ErrorCode::PositionUpdateFailed, "EventLoop",
                                            "failed to deduct position from trade response", 0);
                        record_error(status);
                        ACCT_LOG_ERROR_STATUS(status);
                    }
                }
            }
        }
    }

    if (order && is_terminal_state(order->request.order_state.load(std::memory_order_acquire))) {
        release_order_resources(*order);
    }
    if (order && order->request.order_type == OrderType::Cancel &&
        is_terminal_state(order->request.order_state.load(std::memory_order_acquire)) &&
        order->request.orig_internal_order_id != 0) {
        if (OrderEntry* original = order_book_.find_order(order->request.orig_internal_order_id)) {
            release_order_resources(*original);
        }
    }

    if (!is_terminal_state(response.new_state)) {
        pending_archive_deadlines_ns_.erase(response.internal_order_id);
        return;
    }

    if (!config_.archive_terminal_orders) {
        pending_archive_deadlines_ns_.erase(response.internal_order_id);
        return;
    }

    if (config_.terminal_archive_delay_ms == 0) {
        (void)order_book_.archive_order(response.internal_order_id);
        pending_archive_deadlines_ns_.erase(response.internal_order_id);
        return;
    }

    const TimestampNs deadline_ns = now_ns() + static_cast<TimestampNs>(config_.terminal_archive_delay_ms) * 1000000ULL;
    pending_archive_deadlines_ns_[response.internal_order_id] = deadline_ns;
}

void EventLoop::process_pending_archives(TimestampNs now_ns_value) {
    if (pending_archive_deadlines_ns_.empty()) {
        return;
    }

    std::vector<InternalOrderId> due_order_ids;
    due_order_ids.reserve(pending_archive_deadlines_ns_.size());
    for (const auto& item : pending_archive_deadlines_ns_) {
        if (now_ns_value >= item.second) {
            due_order_ids.push_back(item.first);
        }
    }

    for (InternalOrderId order_id : due_order_ids) {
        const OrderEntry* entry = order_book_.find_order(order_id);
        if (!entry) {
            pending_archive_deadlines_ns_.erase(order_id);
            continue;
        }

        const OrderState status = entry->request.order_state.load(std::memory_order_acquire);
        if (!is_terminal_state(status)) {
            pending_archive_deadlines_ns_.erase(order_id);
            continue;
        }

        (void)order_book_.archive_order(order_id);
        pending_archive_deadlines_ns_.erase(order_id);
    }
}

void EventLoop::update_latency_stats(TimestampNs start, TimestampNs end) {
    if (end < start) {
        return;
    }

    const uint64_t latency = end - start;
    stats_.min_latency_ns = (latency < stats_.min_latency_ns) ? latency : stats_.min_latency_ns;
    stats_.max_latency_ns = (latency > stats_.max_latency_ns) ? latency : stats_.max_latency_ns;
    stats_.total_latency_ns += latency;
    ++stats_.latency_samples;
}

void EventLoop::print_periodic_stats() {
    std::fprintf(stderr,
                 "[EventLoop] iter=%llu orders=%llu responses=%llu idle=%llu avg_ns=%.0f min_ns=%llu max_ns=%llu\n",
                 static_cast<unsigned long long>(stats_.total_iterations),
                 static_cast<unsigned long long>(stats_.orders_processed),
                 static_cast<unsigned long long>(stats_.responses_processed),
                 static_cast<unsigned long long>(stats_.idle_iterations), stats_.avg_latency_ns(),
                 static_cast<unsigned long long>(stats_.min_latency_ns == UINT64_MAX ? 0 : stats_.min_latency_ns),
                 static_cast<unsigned long long>(stats_.max_latency_ns));
}

void EventLoop::set_cpu_affinity(int core) {
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
