#include "order/order_event_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/fixed_string.hpp"
#include "common/log.hpp"
#include "order/passive_execution.hpp"

namespace acct_service {

namespace {

// 区分生产业务事件文件和调试细节文件。
enum class order_event_stream_t : uint8_t {
    Business = 0,
    Debug = 1,
};

const char* to_string(order_event_stream_t stream) noexcept {
    switch (stream) {
        case order_event_stream_t::Business:
            return "business";
        case order_event_stream_t::Debug:
            return "debug";
    }
    return "business";
}

// 为订单事件定义稳定的落盘种类，避免运行时拼装自由文本。
enum class OrderEventKind : uint8_t {
    OrderAdded = 0,
    OrderStatusUpdated = 1,
    OrderTradeUpdated = 2,
    OrderArchived = 3,
    ParentRefreshed = 4,
    SessionStarted = 5,
    SessionRejected = 6,
    ChildSubmitAttempt = 7,
    ChildSubmitResult = 8,
    ChildFinalized = 9,
};

// 固定大小的队列条目，保证热路径无需动态分配即可投递事件。
struct OrderEventRecord {
    TimestampNs ts_ns = 0;
    order_event_stream_t stream = order_event_stream_t::Business;
    OrderEventKind kind = OrderEventKind::OrderAdded;
    InternalOrderId order_id = 0;
    InternalOrderId parent_order_id = 0;
    StrategyId strategy_id = 0;
    OrderIndex shm_order_index = kInvalidOrderIndex;
    Volume volume_entrust = 0;
    Volume volume_traded = 0;
    Volume volume_remain = 0;
    Volume target_volume = 0;
    Volume working_volume = 0;
    Volume schedulable_volume = 0;
    DPrice dprice_entrust = 0;
    DPrice dprice_traded = 0;
    DValue dvalue_traded = 0;
    DValue dfee_executed = 0;
    OrderType order_type = OrderType::NotSet;
    OrderState order_state = OrderState::NotSet;
    TradeSide trade_side = TradeSide::NotSet;
    Market market = Market::NotSet;
    PassiveExecutionAlgo passive_execution_algo = PassiveExecutionAlgo::Default;
    PassiveExecutionAlgo execution_algo = PassiveExecutionAlgo::None;
    ExecutionState execution_state = ExecutionState::None;
    bool is_split_child = false;
    bool active_strategy_claimed = false;
    bool success = false;
    bool cancel_requested = false;
    SecurityId security_id{};
    InternalSecurityId internal_security_id{};
    FixedString<96> detail{};
};

// 统一把订单枚举渲染成稳定的文本值，便于文件检索和下游解析。
const char* to_string(OrderEventKind kind) noexcept {
    switch (kind) {
        case OrderEventKind::OrderAdded:
            return "order_added";
        case OrderEventKind::OrderStatusUpdated:
            return "order_status_updated";
        case OrderEventKind::OrderTradeUpdated:
            return "order_trade_updated";
        case OrderEventKind::OrderArchived:
            return "order_archived";
        case OrderEventKind::ParentRefreshed:
            return "parent_refreshed";
        case OrderEventKind::SessionStarted:
            return "session_started";
        case OrderEventKind::SessionRejected:
            return "session_rejected";
        case OrderEventKind::ChildSubmitAttempt:
            return "child_submit_attempt";
        case OrderEventKind::ChildSubmitResult:
            return "child_submit_result";
        case OrderEventKind::ChildFinalized:
            return "child_finalized";
    }
    return "order_added";
}

// 统一渲染订单类型，避免业务日志混入数值枚举。
const char* to_string(OrderType type) noexcept {
    switch (type) {
        case OrderType::New:
            return "new";
        case OrderType::Cancel:
            return "cancel";
        case OrderType::NotSet:
            return "not_set";
        case OrderType::Unknown:
            return "unknown";
    }
    return "unknown";
}

// 统一渲染买卖方向。
const char* to_string(TradeSide side) noexcept {
    switch (side) {
        case TradeSide::Buy:
            return "buy";
        case TradeSide::Sell:
            return "sell";
        case TradeSide::NotSet:
        default:
            return "not_set";
    }
}

// 统一渲染市场枚举。
const char* to_string(Market market) noexcept {
    switch (market) {
        case Market::SZ:
            return "sz";
        case Market::SH:
            return "sh";
        case Market::BJ:
            return "bj";
        case Market::HK:
            return "hk";
        case Market::NotSet:
            return "not_set";
        case Market::Unknown:
        default:
            return "unknown";
    }
}

// 统一渲染订单状态。
const char* to_string(OrderState state) noexcept {
    switch (state) {
        case OrderState::NotSet:
            return "not_set";
        case OrderState::StrategySubmitted:
            return "strategy_submitted";
        case OrderState::RiskControllerPending:
            return "risk_pending";
        case OrderState::RiskControllerRejected:
            return "risk_rejected";
        case OrderState::RiskControllerAccepted:
            return "risk_accepted";
        case OrderState::TraderPending:
            return "trader_pending";
        case OrderState::TraderRejected:
            return "trader_rejected";
        case OrderState::TraderSubmitted:
            return "trader_submitted";
        case OrderState::TraderError:
            return "trader_error";
        case OrderState::BrokerRejected:
            return "broker_rejected";
        case OrderState::BrokerAccepted:
            return "broker_accepted";
        case OrderState::MarketRejected:
            return "market_rejected";
        case OrderState::MarketAccepted:
            return "market_accepted";
        case OrderState::Finished:
            return "finished";
        case OrderState::Unknown:
        default:
            return "unknown";
    }
}

// 统一渲染执行状态。
const char* to_string(ExecutionState state) noexcept {
    switch (state) {
        case ExecutionState::None:
            return "none";
        case ExecutionState::Running:
            return "running";
        case ExecutionState::Cancelling:
            return "cancelling";
        case ExecutionState::Finished:
            return "finished";
        case ExecutionState::Failed:
            return "failed";
    }
    return "none";
}

// 对字符串字段做最小转义，保证日志仍然保持单行。
std::string escape_field(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

// 统一填充订单快照上的稳定业务字段，供生产日志和调试日志共享。
void fill_from_order_entry(OrderEventRecord& record, const OrderEntry& entry) {
    const OrderRequest& request = entry.request;
    record.order_id = request.internal_order_id;
    record.parent_order_id = entry.parent_order_id;
    record.strategy_id = entry.strategy_id;
    record.shm_order_index = entry.shm_order_index;
    record.volume_entrust = request.volume_entrust;
    record.volume_traded = request.volume_traded;
    record.volume_remain = request.volume_remain;
    record.target_volume = request.target_volume;
    record.working_volume = request.working_volume;
    record.schedulable_volume = request.schedulable_volume;
    record.dprice_entrust = request.dprice_entrust;
    record.dprice_traded = request.dprice_traded;
    record.dvalue_traded = request.dvalue_traded;
    record.dfee_executed = request.dfee_executed;
    record.order_type = request.order_type;
    record.order_state = request.order_state.load(std::memory_order_acquire);
    record.trade_side = request.trade_side;
    record.market = request.market;
    record.passive_execution_algo = request.passive_execution_algo;
    record.execution_algo = request.execution_algo;
    record.execution_state = request.execution_state;
    record.is_split_child = entry.is_split_child;
    record.active_strategy_claimed = request.active_strategy_claimed != 0;
    record.security_id = request.security_id;
    record.internal_security_id = request.internal_security_id;
}

bool should_render_success(OrderEventKind kind) noexcept { return kind == OrderEventKind::ChildSubmitResult; }

bool should_render_cancel_requested(OrderEventKind kind) noexcept { return kind == OrderEventKind::ChildFinalized; }

bool should_render_active_strategy_claimed(OrderEventKind kind) noexcept {
    return kind == OrderEventKind::ChildSubmitAttempt || kind == OrderEventKind::ChildSubmitResult;
}

bool should_render_shm_order_index(const OrderEventRecord& record) noexcept {
    return record.stream == order_event_stream_t::Business || record.shm_order_index != kInvalidOrderIndex;
}

bool should_render_order_state(const OrderEventRecord& record) noexcept {
    return record.stream == order_event_stream_t::Business || record.order_state != OrderState::NotSet;
}

bool should_render_detail(const OrderEventRecord& record) noexcept { return !record.detail.empty(); }

bool is_child_trace(OrderEventKind kind) noexcept {
    return kind == OrderEventKind::ChildSubmitAttempt || kind == OrderEventKind::ChildSubmitResult ||
           kind == OrderEventKind::ChildFinalized;
}

InternalOrderId debug_parent_order_id(const OrderEventRecord& record) noexcept {
    return (record.parent_order_id != 0) ? record.parent_order_id : record.order_id;
}

const char* debug_detail_key(OrderEventKind kind) noexcept {
    switch (kind) {
        case OrderEventKind::SessionRejected:
            return "reason";
        case OrderEventKind::ChildSubmitResult:
            return "result";
        case OrderEventKind::ChildFinalized:
            return "summary";
        case OrderEventKind::OrderAdded:
        case OrderEventKind::OrderStatusUpdated:
        case OrderEventKind::OrderTradeUpdated:
        case OrderEventKind::OrderArchived:
        case OrderEventKind::ParentRefreshed:
        case OrderEventKind::SessionStarted:
        case OrderEventKind::ChildSubmitAttempt:
        default:
            return "detail";
    }
}

// 根据订单簿事件映射稳定的业务事件名。
OrderEventKind map_order_event_kind(order_book_event_t event) noexcept {
    switch (event) {
        case order_book_event_t::Added:
            return OrderEventKind::OrderAdded;
        case order_book_event_t::StatusUpdated:
            return OrderEventKind::OrderStatusUpdated;
        case order_book_event_t::TradeUpdated:
            return OrderEventKind::OrderTradeUpdated;
        case order_book_event_t::Archived:
            return OrderEventKind::OrderArchived;
        case order_book_event_t::ParentRefreshed:
        default:
            return OrderEventKind::ParentRefreshed;
    }
}

}  // namespace

struct OrderEventRecorder::Impl {
    BusinessLogConfig config_{};
    AccountId account_id_ = 0;
    std::string trading_day_{};
    std::vector<OrderEventRecord> ring_{};
    uint32_t ring_size_ = 0;
    std::atomic<uint32_t> write_index_{0};
    std::atomic<uint32_t> read_index_{0};
    std::atomic<uint64_t> dropped_count_{0};
    std::mutex wait_mutex_{};
    std::condition_variable wait_cv_{};
    std::ofstream business_out_{};
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    std::ofstream debug_out_{};
#endif
    std::jthread writer_thread_{};

    // 判断 ring 中是否还有未刷新的事件。
    bool has_pending_events() const noexcept {
        return read_index_.load(std::memory_order_acquire) != write_index_.load(std::memory_order_acquire);
    }

    // 以无锁 SPSC 方式入队；队列满时直接丢弃，不阻塞交易线程。
    bool try_enqueue(const OrderEventRecord& record) noexcept {
        const uint32_t write_index = write_index_.load(std::memory_order_relaxed);
        const uint32_t next_index = (write_index + 1) % ring_size_;
        const uint32_t read_index = read_index_.load(std::memory_order_acquire);
        if (next_index == read_index) {
            dropped_count_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        ring_[write_index] = record;
        write_index_.store(next_index, std::memory_order_release);
        if (write_index == read_index) {
            wait_cv_.notify_one();
        }
        return true;
    }

    // 打开业务事件文件；调试构建额外打开详细 trace 文件。
    bool open_outputs() {
        std::error_code ec;
        std::filesystem::create_directories(config_.output_dir, ec);
        if (ec) {
            return false;
        }

        const std::string suffix = std::to_string(account_id_) + "_" + trading_day_;
        business_out_.open(config_.output_dir + "/order_events_" + suffix + ".log", std::ios::out | std::ios::app);
        if (!business_out_.is_open()) {
            return false;
        }

#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
        debug_out_.open(config_.output_dir + "/order_debug_" + suffix + ".log", std::ios::out | std::ios::app);
        if (!debug_out_.is_open()) {
            return false;
        }
#endif

        return true;
    }

    void write_business_prefix(std::ostream& out, const OrderEventRecord& record) {
        out << "ts_ns=" << record.ts_ns << " account_id=" << account_id_ << " trading_day=\""
            << escape_field(trading_day_) << "\" stream=\"" << to_string(record.stream) << "\" event=\""
            << to_string(record.kind) << "\"" << " order_id=" << record.order_id
            << " parent_order_id=" << record.parent_order_id << " strategy_id=" << record.strategy_id;
        if (should_render_shm_order_index(record)) {
            out << " shm_order_index=" << record.shm_order_index;
        }
    }

    // 业务事件日志只保留订单快照事实字段，避免混入调试默认值误导排查。
    void write_business_record(std::ostream& out, const OrderEventRecord& record) {
        write_business_prefix(out, record);
        out << " is_split_child=" << (record.is_split_child ? "true" : "false") << " order_type=\""
            << to_string(record.order_type) << "\" trade_side=\"" << to_string(record.trade_side) << "\" market=\""
            << to_string(record.market) << "\" order_state=\"" << to_string(record.order_state)
            << "\" passive_execution_algo=\"" << passive_execution_algo_name(record.passive_execution_algo)
            << "\" execution_algo=\"" << passive_execution_algo_name(record.execution_algo) << "\" execution_state=\""
            << to_string(record.execution_state) << "\" volume_entrust=" << record.volume_entrust
            << " volume_traded=" << record.volume_traded << " volume_remain=" << record.volume_remain
            << " target_volume=" << record.target_volume << " working_volume=" << record.working_volume
            << " schedulable_volume=" << record.schedulable_volume << " dprice_entrust=" << record.dprice_entrust
            << " dprice_traded=" << record.dprice_traded << " dvalue_traded=" << record.dvalue_traded
            << " dfee_executed=" << record.dfee_executed << " security_id=\"" << escape_field(record.security_id.view())
            << "\" internal_security_id=\"" << escape_field(record.internal_security_id.view()) << "\"";
        if (should_render_detail(record)) {
            out << " detail=\"" << escape_field(record.detail.view()) << "\"";
        }
        out << '\n';
    }

    void write_debug_prefix(std::ostream& out, const OrderEventRecord& record) {
        out << "ts_ns=" << record.ts_ns << " account_id=" << account_id_ << " trading_day=\""
            << escape_field(trading_day_) << "\" stream=\"" << to_string(record.stream) << "\" trace=\""
            << to_string(record.kind) << "\"" << " parent_order_id=" << debug_parent_order_id(record)
            << " strategy_id=" << record.strategy_id;
        if (is_child_trace(record.kind)) {
            out << " child_order_id=" << record.order_id;
        }
        if (should_render_shm_order_index(record)) {
            out << " shm_order_index=" << record.shm_order_index;
        }
    }

    // 调试 trace 聚焦执行会话推进过程，只打印当前事件真正有语义的字段。
    void write_debug_record(std::ostream& out, const OrderEventRecord& record) {
        write_debug_prefix(out, record);
        out << " order_type=\"" << to_string(record.order_type) << "\" trade_side=\"" << to_string(record.trade_side)
            << "\" market=\"" << to_string(record.market) << "\"";
        if (should_render_order_state(record)) {
            out << " order_state=\"" << to_string(record.order_state) << "\"";
        }
        out << " passive_execution_algo=\"" << passive_execution_algo_name(record.passive_execution_algo)
            << "\" execution_algo=\"" << passive_execution_algo_name(record.execution_algo) << "\" execution_state=\""
            << to_string(record.execution_state) << "\" is_split_child=" << (record.is_split_child ? "true" : "false");
        if (should_render_active_strategy_claimed(record.kind)) {
            out << " active_strategy_claimed=" << (record.active_strategy_claimed ? "true" : "false");
        }
        if (should_render_success(record.kind)) {
            out << " success=" << (record.success ? "true" : "false");
        }
        if (should_render_cancel_requested(record.kind)) {
            out << " cancel_requested=" << (record.cancel_requested ? "true" : "false");
        }
        out << " volume_entrust=" << record.volume_entrust << " volume_traded=" << record.volume_traded
            << " volume_remain=" << record.volume_remain << " target_volume=" << record.target_volume
            << " working_volume=" << record.working_volume << " schedulable_volume=" << record.schedulable_volume
            << " dprice_entrust=" << record.dprice_entrust << " dprice_traded=" << record.dprice_traded
            << " dvalue_traded=" << record.dvalue_traded << " dfee_executed=" << record.dfee_executed
            << " security_id=\"" << escape_field(record.security_id.view()) << "\" internal_security_id=\""
            << escape_field(record.internal_security_id.view()) << "\"";
        if (should_render_detail(record)) {
            out << ' ' << debug_detail_key(record.kind) << "=\"" << escape_field(record.detail.view()) << "\"";
        }
        out << '\n';
    }

    // 后台线程持续消费 ring，避免热路径直接做文件 IO。
    void writer_loop(std::stop_token stop_token) {
        const auto flush_interval = std::chrono::milliseconds(config_.flush_interval_ms);

        while (!stop_token.stop_requested()) {
            bool drained = false;
            uint32_t read_index = read_index_.load(std::memory_order_relaxed);
            uint32_t write_index = write_index_.load(std::memory_order_acquire);
            while (read_index != write_index) {
                const OrderEventRecord& record = ring_[read_index];
                if (record.stream == order_event_stream_t::Business) {
                    write_business_record(business_out_, record);
                }
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
                else {
                    write_debug_record(debug_out_, record);
                }
#endif
                drained = true;
                read_index = (read_index + 1) % ring_size_;
                read_index_.store(read_index, std::memory_order_release);
                write_index = write_index_.load(std::memory_order_acquire);
            }

            if (drained) {
                business_out_.flush();
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
                debug_out_.flush();
#endif
            }

            std::unique_lock<std::mutex> lock(wait_mutex_);
            wait_cv_.wait_for(lock, flush_interval,
                              [&]() { return stop_token.stop_requested() || has_pending_events(); });
        }

        uint32_t read_index = read_index_.load(std::memory_order_relaxed);
        uint32_t write_index = write_index_.load(std::memory_order_acquire);
        while (read_index != write_index) {
            const OrderEventRecord& record = ring_[read_index];
            if (record.stream == order_event_stream_t::Business) {
                write_business_record(business_out_, record);
            }
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
            else {
                write_debug_record(debug_out_, record);
            }
#endif
            read_index = (read_index + 1) % ring_size_;
            read_index_.store(read_index, std::memory_order_release);
            write_index = write_index_.load(std::memory_order_acquire);
        }

        business_out_.flush();
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
        debug_out_.flush();
#endif
    }
};

bool should_emit_debug_order_trace() noexcept {
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    return true;
#else
    return false;
#endif
}

OrderEventRecorder::OrderEventRecorder() = default;

OrderEventRecorder::~OrderEventRecorder() noexcept { shutdown(); }

bool OrderEventRecorder::init(const BusinessLogConfig& config, AccountId account_id, std::string_view trading_day) {
    shutdown();
    if (!config.enabled) {
        return true;
    }
    if (config.queue_capacity < 2 || config.queue_capacity >= static_cast<std::size_t>(UINT32_MAX) ||
        config.output_dir.empty() || config.flush_interval_ms == 0) {
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->config_ = config;
    impl->account_id_ = account_id;
    impl->trading_day_ = std::string(trading_day);
    impl->ring_size_ = static_cast<uint32_t>(config.queue_capacity + 1);
    impl->ring_.resize(impl->ring_size_);

    if (!impl->open_outputs()) {
        return false;
    }

    impl->writer_thread_ =
        std::jthread([state = impl.get()](std::stop_token stop_token) { state->writer_loop(stop_token); });
    impl_ = std::move(impl);
    return true;
}

void OrderEventRecorder::shutdown() noexcept {
    std::unique_ptr<Impl> impl = std::move(impl_);
    if (!impl) {
        return;
    }

    if (impl->writer_thread_.joinable()) {
        impl->writer_thread_.request_stop();
        impl->wait_cv_.notify_all();
        impl->writer_thread_.join();
    }
}

bool OrderEventRecorder::enabled() const noexcept { return impl_ != nullptr; }

uint64_t OrderEventRecorder::dropped_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    return impl_->dropped_count_.load(std::memory_order_relaxed);
}

void OrderEventRecorder::record_order_event(const OrderEntry& entry, order_book_event_t event) noexcept {
    if (!impl_) {
        return;
    }

    OrderEventRecord record{};
    fill_from_order_entry(record, entry);
    record.ts_ns = (entry.last_update_ns != 0) ? entry.last_update_ns : entry.submit_time_ns;
    record.stream = order_event_stream_t::Business;
    record.kind = map_order_event_kind(event);
    (void)impl_->try_enqueue(record);
}

void OrderEventRecorder::record_session_started(const OrderRequest& parent_request, StrategyId strategy_id,
                                                PassiveExecutionAlgo execution_algo) noexcept {
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    if (!impl_) {
        return;
    }

    OrderEventRecord record{};
    record.ts_ns = now_ns();
    record.stream = order_event_stream_t::Debug;
    record.kind = OrderEventKind::SessionStarted;
    record.order_id = parent_request.internal_order_id;
    record.strategy_id = strategy_id;
    record.order_type = parent_request.order_type;
    record.trade_side = parent_request.trade_side;
    record.market = parent_request.market;
    record.passive_execution_algo = parent_request.passive_execution_algo;
    record.execution_algo = execution_algo;
    record.execution_state = ExecutionState::Running;
    record.volume_entrust = parent_request.volume_entrust;
    record.target_volume = parent_request.target_volume;
    record.working_volume = parent_request.working_volume;
    record.schedulable_volume = parent_request.schedulable_volume;
    record.security_id = parent_request.security_id;
    record.internal_security_id = parent_request.internal_security_id;
    (void)impl_->try_enqueue(record);
#else
    (void)parent_request;
    (void)strategy_id;
    (void)execution_algo;
#endif
}

void OrderEventRecorder::record_session_start_rejected(const OrderRequest& parent_request, StrategyId strategy_id,
                                                       PassiveExecutionAlgo execution_algo,
                                                       std::string_view reason) noexcept {
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    if (!impl_) {
        return;
    }

    OrderEventRecord record{};
    record.ts_ns = now_ns();
    record.stream = order_event_stream_t::Debug;
    record.kind = OrderEventKind::SessionRejected;
    record.order_id = parent_request.internal_order_id;
    record.strategy_id = strategy_id;
    record.order_type = parent_request.order_type;
    record.trade_side = parent_request.trade_side;
    record.market = parent_request.market;
    record.passive_execution_algo = parent_request.passive_execution_algo;
    record.execution_algo = execution_algo;
    record.security_id = parent_request.security_id;
    record.internal_security_id = parent_request.internal_security_id;
    record.detail.assign(reason);
    (void)impl_->try_enqueue(record);
#else
    (void)parent_request;
    (void)strategy_id;
    (void)execution_algo;
    (void)reason;
#endif
}

void OrderEventRecorder::record_child_submit_attempt(const OrderRequest& parent_request, StrategyId strategy_id,
                                                     PassiveExecutionAlgo execution_algo,
                                                     InternalOrderId child_order_id, Volume requested_volume,
                                                     DPrice price, ExecutionState execution_state, Volume target_volume,
                                                     Volume working_volume, Volume schedulable_volume,
                                                     bool active_strategy_claimed) noexcept {
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    if (!impl_) {
        return;
    }

    OrderEventRecord record{};
    record.ts_ns = now_ns();
    record.stream = order_event_stream_t::Debug;
    record.kind = OrderEventKind::ChildSubmitAttempt;
    record.order_id = child_order_id;
    record.parent_order_id = parent_request.internal_order_id;
    record.strategy_id = strategy_id;
    record.order_type = OrderType::New;
    record.trade_side = parent_request.trade_side;
    record.market = parent_request.market;
    record.passive_execution_algo = parent_request.passive_execution_algo;
    record.execution_algo = execution_algo;
    record.execution_state = execution_state;
    record.volume_entrust = requested_volume;
    record.target_volume = target_volume;
    record.working_volume = working_volume;
    record.schedulable_volume = schedulable_volume;
    record.dprice_entrust = price;
    record.is_split_child = true;
    record.active_strategy_claimed = active_strategy_claimed;
    record.security_id = parent_request.security_id;
    record.internal_security_id = parent_request.internal_security_id;
    (void)impl_->try_enqueue(record);
#else
    (void)parent_request;
    (void)strategy_id;
    (void)execution_algo;
    (void)child_order_id;
    (void)requested_volume;
    (void)price;
    (void)execution_state;
    (void)target_volume;
    (void)working_volume;
    (void)schedulable_volume;
    (void)active_strategy_claimed;
#endif
}

void OrderEventRecorder::record_child_submit_result(const OrderRequest& parent_request, StrategyId strategy_id,
                                                    PassiveExecutionAlgo execution_algo, InternalOrderId child_order_id,
                                                    OrderIndex shm_order_index, bool success,
                                                    std::string_view reason) noexcept {
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    if (!impl_) {
        return;
    }

    OrderEventRecord record{};
    record.ts_ns = now_ns();
    record.stream = order_event_stream_t::Debug;
    record.kind = OrderEventKind::ChildSubmitResult;
    record.order_id = child_order_id;
    record.parent_order_id = parent_request.internal_order_id;
    record.strategy_id = strategy_id;
    record.shm_order_index = shm_order_index;
    record.order_type = OrderType::New;
    record.trade_side = parent_request.trade_side;
    record.market = parent_request.market;
    record.is_split_child = true;
    record.execution_algo = execution_algo;
    record.success = success;
    record.security_id = parent_request.security_id;
    record.internal_security_id = parent_request.internal_security_id;
    record.detail.assign(reason);
    (void)impl_->try_enqueue(record);
#else
    (void)parent_request;
    (void)strategy_id;
    (void)execution_algo;
    (void)child_order_id;
    (void)shm_order_index;
    (void)success;
    (void)reason;
#endif
}

void OrderEventRecorder::record_child_finalized(const OrderRequest& parent_request, StrategyId strategy_id,
                                                PassiveExecutionAlgo execution_algo, InternalOrderId child_order_id,
                                                OrderIndex shm_order_index, OrderState order_state,
                                                Volume entrust_volume, Volume traded_volume, Volume cancelled_volume,
                                                bool cancel_requested, ExecutionState execution_state,
                                                Volume target_volume, Volume working_volume,
                                                Volume schedulable_volume) noexcept {
#if defined(ACCT_ENABLE_DEBUG_ORDER_TRACE)
    if (!impl_) {
        return;
    }

    OrderEventRecord record{};
    record.ts_ns = now_ns();
    record.stream = order_event_stream_t::Debug;
    record.kind = OrderEventKind::ChildFinalized;
    record.order_id = child_order_id;
    record.parent_order_id = parent_request.internal_order_id;
    record.strategy_id = strategy_id;
    record.shm_order_index = shm_order_index;
    record.order_type = OrderType::New;
    record.order_state = order_state;
    record.trade_side = parent_request.trade_side;
    record.market = parent_request.market;
    record.passive_execution_algo = parent_request.passive_execution_algo;
    record.execution_algo = execution_algo;
    record.execution_state = execution_state;
    record.volume_entrust = entrust_volume;
    record.volume_traded = traded_volume;
    record.volume_remain =
        (entrust_volume >= traded_volume + cancelled_volume) ? (entrust_volume - traded_volume - cancelled_volume) : 0;
    record.target_volume = target_volume;
    record.working_volume = working_volume;
    record.schedulable_volume = schedulable_volume;
    record.is_split_child = true;
    record.cancel_requested = cancel_requested;
    record.security_id = parent_request.security_id;
    record.internal_security_id = parent_request.internal_security_id;
    (void)impl_->try_enqueue(record);
#else
    (void)parent_request;
    (void)strategy_id;
    (void)execution_algo;
    (void)child_order_id;
    (void)shm_order_index;
    (void)order_state;
    (void)entrust_volume;
    (void)traded_volume;
    (void)cancelled_volume;
    (void)cancel_requested;
    (void)execution_state;
    (void)target_volume;
    (void)working_volume;
    (void)schedulable_volume;
#endif
}

}  // namespace acct_service
