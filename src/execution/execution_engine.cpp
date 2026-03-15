#include "execution/execution_engine.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "common/log.hpp"
#include "order/passive_execution.hpp"

namespace acct_service {

namespace {

// 做饱和加法，避免累计成交额和手续费时无界溢出。
uint64_t saturating_add(uint64_t lhs, uint64_t rhs) noexcept {
    const uint64_t limit = std::numeric_limits<uint64_t>::max();
    if (lhs > limit - rhs) {
        return limit;
    }
    return lhs + rhs;
}

// 统一解析订单最终落到的被动执行策略。
SplitStrategy resolve_execution_strategy(const OrderRequest& request, const split_config& split_config) noexcept {
    return resolve_passive_split_strategy(request.passive_execution_algo, split_config.strategy);
}

// 统一判断是否应该进入执行会话框架。
bool is_managed_strategy(SplitStrategy strategy) noexcept {
    switch (strategy) {
        case SplitStrategy::FixedSize:
        case SplitStrategy::TWAP:
        case SplitStrategy::VWAP:
        case SplitStrategy::Iceberg:
            return true;
        case SplitStrategy::None:
        default:
            return false;
    }
}

// 统一判断策略是否已有具体会话实现。
bool is_supported_session_strategy(SplitStrategy strategy) noexcept {
    switch (strategy) {
        case SplitStrategy::FixedSize:
        case SplitStrategy::TWAP:
        case SplitStrategy::Iceberg:
            return true;
        case SplitStrategy::VWAP:
        case SplitStrategy::None:
        default:
            return false;
    }
}

// 识别子单失败终态，失败时父单会话需要立即收敛为 Failed。
bool is_failure_terminal_state(OrderState status) noexcept {
    switch (status) {
        case OrderState::TraderRejected:
        case OrderState::TraderError:
        case OrderState::BrokerRejected:
        case OrderState::MarketRejected:
        case OrderState::Unknown:
            return true;
        default:
            return false;
    }
}

// 选取父单镜像对外展示时使用的“最佳进度”状态。
int status_progress_rank(OrderState status) noexcept {
    switch (status) {
        case OrderState::MarketAccepted:
            return 7;
        case OrderState::BrokerAccepted:
            return 6;
        case OrderState::TraderSubmitted:
            return 5;
        case OrderState::TraderPending:
            return 4;
        case OrderState::RiskControllerAccepted:
            return 3;
        case OrderState::RiskControllerPending:
            return 2;
        case OrderState::StrategySubmitted:
            return 1;
        default:
            return 0;
    }
}

// 统一解析顺序型执行算法的 clip/display 口径。
Volume resolve_clip_volume(const split_config& split_config) noexcept {
    if (split_config.max_child_volume != 0) {
        return split_config.max_child_volume;
    }
    return std::max<Volume>(split_config.min_child_volume, 1);
}

// 判断一个盘口档位是否可以作为定价输入。
bool is_valid_level(const snapshot_shm::SnapshotLevel& level) noexcept { return level.price != 0 && level.volume != 0; }

// 复用旧 TWAP 的切片口径，预先生成每个时间片应消耗的预算。
bool build_twap_slice_plan(const OrderRequest& parent_request, const split_config& split_config,
                           std::vector<Volume>& out_slice_volumes) {
    out_slice_volumes.clear();
    if (parent_request.volume_entrust == 0 || split_config.max_child_count == 0) {
        return false;
    }

    const Volume target_volume = resolve_clip_volume(split_config);
    if (target_volume == 0) {
        return false;
    }

    std::size_t child_count =
        static_cast<std::size_t>((parent_request.volume_entrust + target_volume - 1) / target_volume);
    child_count = std::max<std::size_t>(child_count, 1);
    child_count = std::min<std::size_t>(child_count, split_config.max_child_count);
    if (child_count == 0) {
        return false;
    }

    out_slice_volumes.reserve(child_count);
    const Volume base_volume = parent_request.volume_entrust / child_count;
    Volume remainder = parent_request.volume_entrust % child_count;
    for (std::size_t i = 0; i < child_count; ++i) {
        Volume slice_volume = base_volume;
        if (remainder > 0) {
            ++slice_volume;
            --remainder;
        }
        if (slice_volume > 0) {
            out_slice_volumes.push_back(slice_volume);
        }
    }
    return !out_slice_volumes.empty();
}

// 在内部子单上保留父单基础字段，只重置执行过程相关的可变状态。
OrderRequest make_child_request(const OrderRequest& parent_request, InternalOrderId child_order_id, Volume volume,
                                DPrice price, bool active_strategy_claimed) {
    OrderRequest child_request = parent_request;
    child_request.internal_order_id = child_order_id;
    child_request.passive_execution_algo = PassiveExecutionAlgo::None;
    child_request.active_strategy_claimed = active_strategy_claimed ? 1U : 0U;
    child_request.volume_entrust = volume;
    child_request.dprice_entrust = price;
    child_request.volume_remain = volume;
    child_request.volume_traded = 0;
    child_request.dvalue_traded = 0;
    child_request.dprice_traded = 0;
    child_request.dfee_estimate = 0;
    child_request.dfee_executed = 0;
    child_request.md_time_traded_first = 0;
    child_request.md_time_traded_latest = 0;
    child_request.md_time_broker_response = 0;
    child_request.md_time_market_response = 0;
    child_request.broker_order_id.as_uint = 0;
    child_request.orig_internal_order_id = 0;
    child_request.execution_algo = PassiveExecutionAlgo::None;
    child_request.execution_state = ExecutionState::None;
    child_request.target_volume = 0;
    child_request.working_volume = 0;
    child_request.schedulable_volume = 0;
    return child_request;
}

}  // namespace

struct child_execution_ledger {
    InternalOrderId child_order_id = 0;
    Volume entrust_volume = 0;
    Volume confirmed_traded_volume = 0;
    DValue confirmed_traded_value = 0;
    DValue confirmed_fee = 0;
    Volume final_cancelled_volume = 0;
    bool finalized = false;
    bool cancel_requested = false;
    OrderState order_state = OrderState::NotSet;

    // 计算当前仍占用 working_volume 的未结算部分。
    Volume unresolved_volume() const noexcept {
        if (finalized) {
            return 0;
        }
        const Volume known = confirmed_traded_volume + final_cancelled_volume;
        if (known >= entrust_volume) {
            return 0;
        }
        return entrust_volume - known;
    }
};

class ExecutionSession {
public:
    // 保存父单上下文和统一账本依赖，供各算法派生类复用。
    ExecutionSession(OrderIndex parent_index, const OrderRequest& parent_request, StrategyId strategy_id,
                     PassiveExecutionAlgo execution_algo, OrderBook& order_book, order_router& order_router,
                     MarketDataService* market_data_service, ActiveStrategy* active_strategy)
        : parent_index_(parent_index),
          parent_request_(parent_request),
          strategy_id_(strategy_id),
          execution_algo_(execution_algo),
          order_book_(order_book),
          order_router_(order_router),
          market_data_service_(market_data_service),
          active_strategy_(active_strategy) {}

    virtual ~ExecutionSession() = default;

    // 返回是否满足本会话的最小创建条件。
    virtual bool is_valid() const noexcept { return true; }

    // 推进一次算法状态机，并同步父单镜像。
    virtual void tick(TimestampNs now_ns_value) = 0;

    // 用统一账本消费子单回报，供预算释放和父单镜像同步使用。
    virtual void on_trade_response(const TradeResponse& response) {
        const auto child_it = child_ledgers_.find(response.internal_order_id);
        if (child_it == child_ledgers_.end()) {
            return;
        }

        child_execution_ledger& ledger = child_it->second;
        if (ledger.finalized) {
            if (response.volume_traded > 0 || response.cancelled_volume > 0) {
                ACCT_LOG_WARN("ExecutionEngine", "ignored conflicting late trade response after child finalized");
            }
            return;
        }

        if (response.volume_traded > 0) {
            ledger.confirmed_traded_volume =
                std::min<Volume>(ledger.entrust_volume, ledger.confirmed_traded_volume + response.volume_traded);
            ledger.confirmed_traded_value = saturating_add(ledger.confirmed_traded_value, response.dvalue_traded);
            ledger.confirmed_fee = saturating_add(ledger.confirmed_fee, response.dfee);
        }

        ledger.order_state = response.new_state;
        if (response.new_state == OrderState::Finished) {
            finalize_finished_child(ledger, response.cancelled_volume);
            on_child_finalized(ledger.child_order_id);
            return;
        }

        if (is_failure_terminal_state(response.new_state)) {
            ledger.final_cancelled_volume = ledger.unresolved_volume();
            ledger.finalized = true;
            failed_ = true;
            on_child_finalized(ledger.child_order_id);
        }
    }

    // 暴露父单 ID 给引擎做会话索引和回收。
    InternalOrderId parent_order_id() const noexcept { return parent_request_.internal_order_id; }

    // 标识会话是否已经收敛，可以从引擎中回收。
    bool is_terminal() const noexcept { return terminal_; }

protected:
    // 读取父单最新盘口快照，供主动/被动共用定价逻辑复用。
    bool read_market_data_view(MarketDataView& out_view) const {
        if (!market_data_service_ || !market_data_service_->is_ready()) {
            return false;
        }
        return market_data_service_->read(parent_request_.internal_security_id.view(), out_view);
    }

    // 基于当前盘口生成子单价格，父单限价只作为边界，不再直接作为默认发单价。
    bool resolve_market_price(const MarketDataView& market_data_view, DPrice& out_price) const noexcept {
        const snapshot_shm::LobSnapshot& snapshot = market_data_view.snapshot;
        const bool has_best_bid = snapshot.total_bid_levels > 0 && is_valid_level(snapshot.bids[0]);
        const bool has_best_ask = snapshot.total_ask_levels > 0 && is_valid_level(snapshot.asks[0]);
        if (!has_best_bid && !has_best_ask) {
            return false;
        }

        DPrice candidate_price = 0;
        if (parent_request_.trade_side == TradeSide::Buy) {
            candidate_price = has_best_ask ? snapshot.asks[0].price : snapshot.bids[0].price;
            candidate_price = std::min(candidate_price, parent_request_.dprice_entrust);
        } else if (parent_request_.trade_side == TradeSide::Sell) {
            candidate_price = has_best_bid ? snapshot.bids[0].price : snapshot.asks[0].price;
            candidate_price = std::max(candidate_price, parent_request_.dprice_entrust);
        } else {
            return false;
        }

        if (candidate_price == 0) {
            return false;
        }
        out_price = candidate_price;
        return true;
    }

    // 刷新父单存在性和取消请求感知，保持会话与订单簿一致。
    bool refresh_parent_state() {
        const OrderEntry* parent = order_book_.find_order(parent_request_.internal_order_id);
        if (!parent) {
            failed_ = true;
            terminal_ = true;
            return false;
        }

        cancel_requested_ = false;
        const std::vector<InternalOrderId> related_ids = order_book_.get_children(parent_request_.internal_order_id);
        for (InternalOrderId related_id : related_ids) {
            const OrderEntry* entry = order_book_.find_order(related_id);
            if (!entry || entry->request.order_type != OrderType::Cancel) {
                continue;
            }

            cancel_requested_ = true;
            const auto child_it = child_ledgers_.find(entry->request.orig_internal_order_id);
            if (child_it != child_ledgers_.end()) {
                child_it->second.cancel_requested = true;
            }
        }
        return true;
    }

    // 按统一账本构造父单镜像，写回 monitor 和 orders_shm。
    void sync_parent_view() {
        ManagedParentView view{};
        view.execution_algo = execution_algo_;
        view.execution_state = derive_execution_state();
        view.target_volume = parent_request_.volume_entrust;
        view.working_volume = working_volume();
        view.schedulable_volume = schedulable_volume();
        view.confirmed_traded_volume = confirmed_traded_volume();
        view.confirmed_traded_value = confirmed_traded_value();
        view.confirmed_fee = confirmed_fee();
        (void)order_book_.sync_managed_parent_view(parent_request_.internal_order_id, view, derive_parent_state());
    }

    // 当前账本已确认成交量累计。
    Volume confirmed_traded_volume() const noexcept {
        Volume total = 0;
        for (const auto& [child_id, ledger] : child_ledgers_) {
            (void)child_id;
            total = saturating_add(total, ledger.confirmed_traded_volume);
        }
        return total;
    }

    // 当前账本已确认成交额累计。
    DValue confirmed_traded_value() const noexcept {
        DValue total = 0;
        for (const auto& [child_id, ledger] : child_ledgers_) {
            (void)child_id;
            total = saturating_add(total, ledger.confirmed_traded_value);
        }
        return total;
    }

    // 当前账本已确认手续费累计。
    DValue confirmed_fee() const noexcept {
        DValue total = 0;
        for (const auto& [child_id, ledger] : child_ledgers_) {
            (void)child_id;
            total = saturating_add(total, ledger.confirmed_fee);
        }
        return total;
    }

    // 当前所有未 finalize 子单占用的 working_volume。
    Volume working_volume() const noexcept {
        Volume total = 0;
        for (const auto& [child_id, ledger] : child_ledgers_) {
            (void)child_id;
            total = saturating_add(total, ledger.unresolved_volume());
        }
        return total;
    }

    // 当前父单剩余目标量，仅由已确认成交量派生。
    Volume remaining_target() const noexcept {
        const Volume traded = confirmed_traded_volume();
        if (traded >= parent_request_.volume_entrust) {
            return 0;
        }
        return parent_request_.volume_entrust - traded;
    }

    // 当前父单还能继续释放的预算量。
    Volume schedulable_volume() const noexcept {
        const Volume remaining = remaining_target();
        const Volume working = working_volume();
        if (working >= remaining) {
            return 0;
        }
        return remaining - working;
    }

    // 判断当前是否还有未终态的活动子单。
    bool has_working_children() const noexcept { return working_volume() > 0; }

    // 以统一路径提交一笔内部子单，并把它登记进 child ledger。
    bool submit_child(Volume volume, DPrice price, bool active_claimed) {
        if (volume == 0 || price == 0) {
            return false;
        }

        OrderEntry child_entry{};
        child_entry.request =
            make_child_request(parent_request_, order_book_.next_order_id(), volume, price, active_claimed);
        child_entry.submit_time_ns = now_ns();
        child_entry.last_update_ns = child_entry.submit_time_ns;
        child_entry.strategy_id = strategy_id_;
        child_entry.risk_result = RiskResult::Pass;
        child_entry.retry_count = 0;
        child_entry.is_split_child = true;
        child_entry.parent_order_id = parent_request_.internal_order_id;
        child_entry.shm_order_index = kInvalidOrderIndex;

        if (!order_router_.submit_internal_order(child_entry)) {
            failed_ = true;
            terminal_ = true;
            sync_parent_view();
            return false;
        }

        child_execution_ledger ledger{};
        ledger.child_order_id = child_entry.request.internal_order_id;
        ledger.entrust_volume = volume;
        ledger.order_state = OrderState::TraderSubmitted;
        child_ledgers_.emplace(ledger.child_order_id, ledger);
        return true;
    }

    // 在当前预算窗口内先询问主动策略，命中后直接生成一笔内部子单。
    bool try_active_submit(const MarketDataView& market_data_view, DPrice market_price, Volume budget_volume) {
        if (!active_strategy_ || budget_volume == 0) {
            return false;
        }
        if (!market_data_view.prediction.has_fresh_prediction()) {
            return false;
        }
        if (market_data_view.prediction.publish_seq_no == last_active_publish_seq_no_) {
            return false;
        }

        const ActiveDecision decision =
            active_strategy_->evaluate(ActiveStrategyContext{parent_request_, market_data_view, budget_volume});
        last_active_publish_seq_no_ = market_data_view.prediction.publish_seq_no;
        if (!decision.should_submit || decision.volume == 0 || decision.volume > budget_volume) {
            return false;
        }

        return submit_child(decision.volume, market_price, true);
    }

    // 当子单终态落地后，由派生类决定如何推进后续算法步骤。
    virtual void on_child_finalized(InternalOrderId child_order_id) { (void)child_order_id; }

    // 统一把 finished 事件收口成 child ledger 的 final_cancelled_volume。
    void finalize_finished_child(child_execution_ledger& ledger, Volume cancelled_volume) {
        const Volume unresolved_before = ledger.unresolved_volume();
        if (cancelled_volume > 0) {
            const Volume max_cancelled = (ledger.confirmed_traded_volume >= ledger.entrust_volume)
                                             ? 0
                                             : (ledger.entrust_volume - ledger.confirmed_traded_volume);
            ledger.final_cancelled_volume = std::min(cancelled_volume, max_cancelled);
            if (ledger.confirmed_traded_volume + cancelled_volume > ledger.entrust_volume) {
                ACCT_LOG_WARN("ExecutionEngine", "clamped conflicting cancelled_volume on child finish");
            }
        } else if (unresolved_before == 0) {
            ledger.final_cancelled_volume = 0;
        } else {
            // 兼容仍未补齐 cancelled_volume 的旧链路，但明确打日志暴露协议缺口。
            ACCT_LOG_WARN("ExecutionEngine", "inferred remaining child volume on finish without cancelled_volume");
            ledger.final_cancelled_volume = unresolved_before;
        }

        ledger.finalized = true;
        if (cancel_requested_ && !has_working_children()) {
            last_active_publish_seq_no_ = 0;
        }
    }

    // 根据当前账本和会话状态，推导父单在旧状态字段中应展示的进度。
    OrderState derive_parent_state() const noexcept {
        if (failed_) {
            return OrderState::TraderError;
        }
        if (terminal_) {
            return OrderState::Finished;
        }

        OrderState best_progress_status = OrderState::TraderPending;
        int best_rank = status_progress_rank(best_progress_status);
        for (const auto& [child_id, ledger] : child_ledgers_) {
            (void)child_id;
            const int rank = status_progress_rank(ledger.order_state);
            if (rank > best_rank) {
                best_rank = rank;
                best_progress_status = ledger.order_state;
            }
        }
        return best_progress_status;
    }

    // 统一输出新的执行态字段，供 monitor 区分 Running/Cancelling/Finished/Failed。
    ExecutionState derive_execution_state() const noexcept {
        if (failed_) {
            return ExecutionState::Failed;
        }
        if (terminal_) {
            return ExecutionState::Finished;
        }
        if (cancel_requested_) {
            return ExecutionState::Cancelling;
        }
        return ExecutionState::Running;
    }

    // 在父单进入终态时停止会话并刷新父单镜像。
    void finalize_parent_if_idle() {
        if (failed_) {
            terminal_ = true;
            sync_parent_view();
            return;
        }
        if (cancel_requested_ && !has_working_children()) {
            terminal_ = true;
            sync_parent_view();
            return;
        }
        if (remaining_target() == 0 && !has_working_children()) {
            terminal_ = true;
            sync_parent_view();
        }
    }

    OrderIndex parent_index_ = kInvalidOrderIndex;
    OrderRequest parent_request_{};
    StrategyId strategy_id_ = 0;
    PassiveExecutionAlgo execution_algo_ = PassiveExecutionAlgo::None;
    OrderBook& order_book_;
    order_router& order_router_;
    MarketDataService* market_data_service_ = nullptr;
    ActiveStrategy* active_strategy_ = nullptr;
    std::unordered_map<InternalOrderId, child_execution_ledger> child_ledgers_;
    uint64_t last_active_publish_seq_no_ = 0;
    bool cancel_requested_ = false;
    bool failed_ = false;
    bool terminal_ = false;
};

class SequentialClipSession : public ExecutionSession {
public:
    // 用统一 clip 逻辑驱动 FixedSize / Iceberg 这两类顺序型算法。
    SequentialClipSession(OrderIndex parent_index, const OrderRequest& parent_request, StrategyId strategy_id,
                          PassiveExecutionAlgo execution_algo, const split_config& split_config, OrderBook& order_book,
                          order_router& order_router, MarketDataService* market_data_service,
                          ActiveStrategy* active_strategy)
        : ExecutionSession(parent_index, parent_request, strategy_id, execution_algo, order_book, order_router,
                           market_data_service, active_strategy),
          clip_volume_(resolve_clip_volume(split_config)) {}

    // 顺序型算法的创建条件是 clip/display 口径非零。
    bool is_valid() const noexcept override { return clip_volume_ != 0; }

    // 没有在途子单时释放下一笔预算；先主动，后被动。
    void tick(TimestampNs now_ns_value) override {
        (void)now_ns_value;
        if (!refresh_parent_state()) {
            sync_parent_view();
            return;
        }

        if (failed_) {
            terminal_ = true;
            sync_parent_view();
            return;
        }

        if (has_working_children()) {
            sync_parent_view();
            return;
        }

        if (cancel_requested_) {
            terminal_ = true;
            sync_parent_view();
            return;
        }

        const Volume budget_volume = std::min<Volume>(clip_volume_, schedulable_volume());
        if (budget_volume == 0) {
            finalize_parent_if_idle();
            if (!terminal_) {
                sync_parent_view();
            }
            return;
        }

        MarketDataView market_data_view{};
        DPrice market_price = 0;
        if (!read_market_data_view(market_data_view) || !resolve_market_price(market_data_view, market_price)) {
            sync_parent_view();
            return;
        }

        if (!try_active_submit(market_data_view, market_price, budget_volume)) {
            (void)submit_child(budget_volume, market_price, false);
        }
        sync_parent_view();
    }

private:
    Volume clip_volume_ = 0;
};

class FixedSizeSession final : public SequentialClipSession {
public:
    // 使用固定 clip 逐笔释放预算，直到目标量完成或父单取消。
    FixedSizeSession(OrderIndex parent_index, const OrderRequest& parent_request, StrategyId strategy_id,
                     const split_config& split_config, OrderBook& order_book, order_router& order_router,
                     MarketDataService* market_data_service, ActiveStrategy* active_strategy)
        : SequentialClipSession(parent_index, parent_request, strategy_id, PassiveExecutionAlgo::FixedSize,
                                split_config, order_book, order_router, market_data_service, active_strategy) {}
};

class IcebergSession final : public SequentialClipSession {
public:
    // 首版 Iceberg 与 FixedSize 共享推进器，只保留独立会话类型和状态命名。
    IcebergSession(OrderIndex parent_index, const OrderRequest& parent_request, StrategyId strategy_id,
                   const split_config& split_config, OrderBook& order_book, order_router& order_router,
                   MarketDataService* market_data_service, ActiveStrategy* active_strategy)
        : SequentialClipSession(parent_index, parent_request, strategy_id, PassiveExecutionAlgo::Iceberg, split_config,
                                order_book, order_router, market_data_service, active_strategy) {}
};

class TwapSession final : public ExecutionSession {
public:
    // 记录 TWAP 的时间片计划和当前片节奏，按固定时钟持续推进。
    TwapSession(OrderIndex parent_index, const OrderRequest& parent_request, StrategyId strategy_id,
                const split_config& split_config, TimestampNs start_time_ns, OrderBook& order_book,
                order_router& order_router, MarketDataService* market_data_service, ActiveStrategy* active_strategy)
        : ExecutionSession(parent_index, parent_request, strategy_id, PassiveExecutionAlgo::TWAP, order_book,
                           order_router, market_data_service, active_strategy),
          start_time_ns_(start_time_ns),
          interval_ns_(static_cast<TimestampNs>(split_config.interval_ms) * 1'000'000ULL),
          next_deadline_ns_(start_time_ns) {
        valid_ = interval_ns_ != 0 && build_twap_slice_plan(parent_request, split_config, slice_volumes_);
    }

    // TWAP 只有在 interval 与切片计划都有效时才能创建。
    bool is_valid() const noexcept override { return valid_; }

    // 每轮先给当前时间片一个主动尝试机会，到片末再回落被动片额。
    void tick(TimestampNs now_ns_value) override {
        if (!refresh_parent_state()) {
            sync_parent_view();
            return;
        }

        if (failed_) {
            terminal_ = true;
            sync_parent_view();
            return;
        }

        advance_slice_if_ready();
        if (slice_index_ >= slice_volumes_.size()) {
            if (!has_working_children()) {
                terminal_ = true;
                sync_parent_view();
            } else {
                sync_parent_view();
            }
            return;
        }

        if (cancel_requested_) {
            finalize_parent_if_idle();
            if (!terminal_) {
                sync_parent_view();
            }
            return;
        }

        const Volume budget_volume = current_budget_volume();
        if (budget_volume > 0) {
            MarketDataView market_data_view{};
            DPrice market_price = 0;
            if (!read_market_data_view(market_data_view) || !resolve_market_price(market_data_view, market_price)) {
                sync_parent_view();
                return;
            }

            if (!slice_consumed_) {
                slice_consumed_ = try_active_submit(market_data_view, market_price, budget_volume);
            }
            if (!slice_consumed_ && now_ns_value >= next_deadline_ns_) {
                slice_consumed_ = submit_child(budget_volume, market_price, false);
            }
        }

        advance_slice_if_ready();
        if (slice_index_ >= slice_volumes_.size()) {
            if (!has_working_children()) {
                terminal_ = true;
            }
        }
        if (!terminal_) {
            sync_parent_view();
        }
    }

protected:
    // 当前片子单全部终态后，推进到下一时间片并恢复主动评估机会。
    void on_child_finalized(InternalOrderId child_order_id) override {
        (void)child_order_id;
        advance_slice_if_ready();
    }

private:
    // 返回当前片在 remaining/working 约束下还能消耗的预算。
    Volume current_budget_volume() const noexcept {
        if (slice_index_ >= slice_volumes_.size()) {
            return 0;
        }
        return std::min<Volume>(slice_volumes_[slice_index_], schedulable_volume());
    }

    // 当前片已有子单且全部终态后，切到下一片并按固定时钟继续推进。
    void advance_slice_if_ready() {
        if (!slice_consumed_ || has_working_children()) {
            return;
        }
        if (slice_index_ < slice_volumes_.size()) {
            ++slice_index_;
        }
        slice_consumed_ = false;
        last_active_publish_seq_no_ = 0;
        next_deadline_ns_ = start_time_ns_ + static_cast<TimestampNs>(slice_index_) * interval_ns_;
    }

    bool valid_ = false;
    TimestampNs start_time_ns_ = 0;
    TimestampNs interval_ns_ = 0;
    TimestampNs next_deadline_ns_ = 0;
    std::size_t slice_index_ = 0;
    bool slice_consumed_ = false;
    std::vector<Volume> slice_volumes_;
};

// 用统一执行会话工厂创建具体的被动算法实现。
std::unique_ptr<ExecutionSession> create_execution_session(const split_config& split_config, OrderIndex parent_index,
                                                           const OrderRequest& parent_request, StrategyId strategy_id,
                                                           TimestampNs start_time_ns, OrderBook& order_book,
                                                           order_router& order_router,
                                                           MarketDataService* market_data_service,
                                                           ActiveStrategy* active_strategy) {
    const SplitStrategy strategy = resolve_execution_strategy(parent_request, split_config);
    switch (strategy) {
        case SplitStrategy::FixedSize:
            return std::make_unique<FixedSizeSession>(parent_index, parent_request, strategy_id, split_config,
                                                      order_book, order_router, market_data_service, active_strategy);
        case SplitStrategy::Iceberg:
            return std::make_unique<IcebergSession>(parent_index, parent_request, strategy_id, split_config, order_book,
                                                    order_router, market_data_service, active_strategy);
        case SplitStrategy::TWAP:
            return std::make_unique<TwapSession>(parent_index, parent_request, strategy_id, split_config, start_time_ns,
                                                 order_book, order_router, market_data_service, active_strategy);
        case SplitStrategy::VWAP:
        case SplitStrategy::None:
        default:
            return nullptr;
    }
}

ExecutionEngine::ExecutionEngine(const split_config& split_config, OrderBook& order_book, order_router& order_router,
                                 MarketDataService* market_data_service,
                                 std::unique_ptr<ActiveStrategy> active_strategy)
    : split_config_(split_config),
      order_book_(order_book),
      order_router_(order_router),
      market_data_service_(market_data_service),
      active_strategy_(std::move(active_strategy)) {}

ExecutionEngine::~ExecutionEngine() = default;

// 所有显式执行算法都统一进入执行引擎，不再回退旧的一次性 splitter 运行时路径。
bool ExecutionEngine::should_manage(const OrderRequest& request) const noexcept {
    if (request.order_type != OrderType::New) {
        return false;
    }
    return is_managed_strategy(resolve_execution_strategy(request, split_config_));
}

bool ExecutionEngine::has_active_strategy() const noexcept { return active_strategy_ != nullptr; }

// 为父单创建统一执行会话，VWAP 明确拒绝，其他无效配置返回错误。
ExecutionEngine::SessionStartResult ExecutionEngine::start_session(OrderIndex parent_index,
                                                                   const OrderRequest& parent_request,
                                                                   StrategyId strategy_id, TimestampNs start_time_ns) {
    if (!should_manage(parent_request)) {
        return SessionStartResult::InvalidConfig;
    }
    if (sessions_.find(parent_request.internal_order_id) != sessions_.end()) {
        return SessionStartResult::Duplicate;
    }

    const SplitStrategy strategy = resolve_execution_strategy(parent_request, split_config_);
    if (!is_supported_session_strategy(strategy)) {
        return SessionStartResult::Unsupported;
    }
    if (!market_data_service_ || !market_data_service_->is_ready()) {
        return SessionStartResult::MarketDataUnavailable;
    }

    std::unique_ptr<ExecutionSession> session =
        create_execution_session(split_config_, parent_index, parent_request, strategy_id, start_time_ns, order_book_,
                                 order_router_, market_data_service_, active_strategy_.get());
    if (!session || !session->is_valid()) {
        return SessionStartResult::InvalidConfig;
    }

    if (!order_book_.mark_managed_parent(parent_request.internal_order_id)) {
        return SessionStartResult::InvalidConfig;
    }

    sessions_.emplace(parent_request.internal_order_id, std::move(session));
    sessions_.at(parent_request.internal_order_id)->tick(start_time_ns);
    return SessionStartResult::Started;
}

// 推进所有活跃会话，并在终态后及时回收会话对象。
void ExecutionEngine::tick(TimestampNs now_ns_value) {
    std::vector<InternalOrderId> finished_sessions;
    finished_sessions.reserve(sessions_.size());

    for (auto& [parent_order_id, session] : sessions_) {
        if (!session) {
            finished_sessions.push_back(parent_order_id);
            continue;
        }

        session->tick(now_ns_value);
        if (session->is_terminal()) {
            finished_sessions.push_back(parent_order_id);
        }
    }

    for (InternalOrderId parent_order_id : finished_sessions) {
        sessions_.erase(parent_order_id);
    }
}

// 把子单回报路由回所属会话，用统一账本释放预算并刷新父单镜像。
void ExecutionEngine::on_trade_response(const TradeResponse& response) noexcept {
    InternalOrderId parent_order_id = 0;
    if (!order_book_.try_get_parent(response.internal_order_id, parent_order_id)) {
        return;
    }

    const auto session_it = sessions_.find(parent_order_id);
    if (session_it == sessions_.end() || !session_it->second) {
        return;
    }
    session_it->second->on_trade_response(response);
}

}  // namespace acct_service
