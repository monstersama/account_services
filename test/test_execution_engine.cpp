#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "core/event_loop.hpp"
#include "execution/execution_engine.hpp"
#include "market_data/market_data_service.hpp"
#include "order/order_router.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "shm/orders_shm.hpp"
#include "snapshot_shm.hpp"
#include "strategy/active_strategy.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

// 为 snapshot_reader 文件后端测试提供作用域内环境变量控制。
class ScopedEnvOverride {
public:
    // 设置环境变量并在析构时恢复，避免不同测试互相污染。
    ScopedEnvOverride(const char* key, const char* value) : key_(key) {
        const char* existing = std::getenv(key);
        if (existing != nullptr) {
            had_old_value_ = true;
            old_value_ = existing;
        }
        (void)::setenv(key, value, 1);
    }

    ~ScopedEnvOverride() {
        if (had_old_value_) {
            (void)::setenv(key_.c_str(), old_value_.c_str(), 1);
        } else {
            (void)::unsetenv(key_.c_str());
        }
    }

private:
    std::string key_;
    std::string old_value_;
    bool had_old_value_ = false;
};

// 初始化通用 SHM 头部，供纯内存测试夹具复用。
void init_header(SHMHeader& header) {
    header.magic = SHMHeader::kMagic;
    header.version = SHMHeader::kVersion;
    header.create_time = now_ns();
    header.last_update = header.create_time;
    header.next_order_id.store(1, std::memory_order_relaxed);
}

// 构造上游 SHM 夹具。
std::unique_ptr<upstream_shm_layout> make_upstream_shm() {
    auto shm = std::make_unique<upstream_shm_layout>();
    init_header(shm->header);
    shm->upstream_order_queue.init();
    return shm;
}

// 构造下游 SHM 夹具。
std::unique_ptr<downstream_shm_layout> make_downstream_shm() {
    auto shm = std::make_unique<downstream_shm_layout>();
    init_header(shm->header);
    shm->order_queue.init();
    return shm;
}

// 构造成交回报 SHM 夹具。
std::unique_ptr<trades_shm_layout> make_trades_shm() {
    auto shm = std::make_unique<trades_shm_layout>();
    init_header(shm->header);
    shm->response_queue.init();
    return shm;
}

// 构造订单池 SHM 夹具。
std::unique_ptr<orders_shm_layout> make_orders_shm() {
    auto shm = std::make_unique<orders_shm_layout>();
    shm->header.magic = OrdersHeader::kMagic;
    shm->header.version = OrdersHeader::kVersion;
    shm->header.header_size = static_cast<uint32_t>(sizeof(OrdersHeader));
    shm->header.total_size = static_cast<uint32_t>(sizeof(orders_shm_layout));
    shm->header.capacity = static_cast<uint32_t>(kDailyOrderPoolCapacity);
    shm->header.init_state = 1;
    shm->header.create_time = now_ns();
    shm->header.last_update = shm->header.create_time;
    shm->header.next_index.store(0, std::memory_order_relaxed);
    shm->header.full_reject_count.store(0, std::memory_order_relaxed);
    std::memcpy(shm->header.trading_day, "19700101", 9);
    return shm;
}

// 构造持仓 SHM 夹具。
std::unique_ptr<positions_shm_layout> make_positions_shm() {
    auto shm = std::make_unique<positions_shm_layout>();
    shm->header.magic = PositionsHeader::kMagic;
    shm->header.version = PositionsHeader::kVersion;
    shm->header.header_size = static_cast<uint32_t>(sizeof(PositionsHeader));
    shm->header.total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
    shm->header.capacity = static_cast<uint32_t>(kMaxPositions);
    shm->header.init_state = 0;
    shm->header.id.store(1, std::memory_order_relaxed);
    shm->position_count.store(0, std::memory_order_relaxed);
    return shm;
}

// 轮询等待条件成立，避免执行引擎测试依赖固定 sleep。
bool wait_until(const std::function<bool()>& predicate, int timeout_ms = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// 生成带执行算法声明的父单请求，供执行引擎接管。
OrderRequest make_managed_order(InternalOrderId order_id, Volume volume, PassiveExecutionAlgo algo) {
    OrderRequest request;
    request.init_new("000001", InternalSecurityId("XSHE_000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000,
                     93000000);
    request.passive_execution_algo = algo;
    request.order_state.store(OrderState::StrategySubmitted, std::memory_order_relaxed);
    return request;
}

// 生成唯一 snapshot 文件路径，避免并发测试复用同一文件。
std::string unique_snapshot_path(const char* prefix) {
    return std::string("/tmp/") + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(now_ns()));
}

// 构造单标的 snapshot writer，供主动策略测试发布 fresh prediction。
using snapshot_writer_ptr = std::unique_ptr<void, void (*)(void*)>;

// 创建单标的 snapshot 文件并初始化 symbol 索引。
snapshot_writer_ptr make_snapshot_writer(const std::string& snapshot_path) {
    snapshot_writer_ptr writer(snapshot_shm_writer_new(), snapshot_shm_writer_delete);
    assert(writer != nullptr);

    snapshot_shm::SnapshotSymbolDef symbol{};
    std::strncpy(symbol.symbol, "XSHE_000001", snapshot_shm::kSnapshotSymbolBytes - 1);
    assert(snapshot_shm_writer_init(writer.get(), snapshot_path.c_str(), 20260225, &symbol, 1) == 1);
    return writer;
}

// 生成最小盘口快照，让主动策略测试能读取到 bid/ask 与 prediction 同时有效。
snapshot_shm::LobSnapshot make_snapshot() {
    snapshot_shm::LobSnapshot snapshot{};
    snapshot.time_of_day = 93'100'000;
    snapshot.total_bid_levels = 1;
    snapshot.total_ask_levels = 1;
    snapshot.bids[0].price = 998;
    snapshot.bids[0].volume = 100;
    snapshot.asks[0].price = 999;
    snapshot.asks[0].volume = 120;
    return snapshot;
}

// 为 managed execution 测试封装行情文件生命周期，保证子单始终能读到稳定盘口。
class ManagedMarketDataFixture {
public:
    // 创建单标的行情文件，并按需要发布 prediction/盘口快照。
    ManagedMarketDataFixture(const char* prefix, float signal, uint32_t flags,
                             snapshot_shm::SnapshotPredictionState state)
        : env_override_("SHM_USE_FILE", "1"),
          snapshot_path_(unique_snapshot_path(prefix)),
          writer_(make_snapshot_writer(snapshot_path_)),
          market_data_config_(make_market_data_config(snapshot_path_)),
          market_data_service_(market_data_config_) {
        const snapshot_shm::LobSnapshot snapshot = make_snapshot();
        assert(snapshot_shm_writer_publish_with_state(writer_.get(), 0, &snapshot, signal, flags,
                                                      static_cast<uint8_t>(state)) == 1);
        assert(market_data_service_.initialize());
    }

    ~ManagedMarketDataFixture() {
        market_data_service_.close();
        assert(snapshot_shm_writer_unlink(snapshot_path_.c_str()) == 1);
    }

    // 返回供 ExecutionEngine 直接复用的行情服务实例。
    MarketDataService* service() { return &market_data_service_; }

private:
    // 构造最小市场数据配置，避免每个测试重复拼装。
    static MarketDataConfig make_market_data_config(const std::string& snapshot_path) {
        MarketDataConfig config{};
        config.enabled = true;
        config.snapshot_shm_name = snapshot_path;
        return config;
    }

    ScopedEnvOverride env_override_;
    std::string snapshot_path_;
    snapshot_writer_ptr writer_;
    MarketDataConfig market_data_config_{};
    MarketDataService market_data_service_;
};

// 用于验证主动策略返回的 volume/price 会被真正落到子单上的测试策略。
class FixedDecisionStrategy final : public ActiveStrategy {
public:
    // 固定返回半片数量和指定价格，便于测试断言决策是否被执行引擎尊重。
    FixedDecisionStrategy(Volume volume, DPrice price) : volume_(volume), price_(price) {}

    const char* name() const noexcept override { return "fixed_decision"; }

    ActiveDecision evaluate(const ActiveStrategyContext& context) override {
        (void)context;
        return ActiveDecision{true, volume_, price_};
    }

private:
    Volume volume_ = 0;
    DPrice price_ = 0;
};

}  // namespace

TEST(twap_falls_back_without_active_strategy) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine_no_active", 0.0F, 0,
                                                 snapshot_shm::SnapshotPredictionState::kNone);
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 40;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 4;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());
    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(), nullptr);

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(900, 100, PassiveExecutionAlgo::TWAP);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(child_index));
    assert(child_index != parent_index);

    order_slot_snapshot child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), child_index, child_snapshot));
    assert(child_snapshot.request.passive_execution_algo == PassiveExecutionAlgo::None);
    assert(child_snapshot.request.active_strategy_claimed == 0);
    assert(child_snapshot.request.volume_entrust > 0);
    assert(child_snapshot.request.volume_entrust < request.volume_entrust);
    assert(child_snapshot.request.dprice_entrust == 999);

    loop.stop();
    worker.join();
}

TEST(twap_uses_active_strategy_with_fresh_prediction) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine", 1.5F,
                                                 snapshot_shm::kSnapshotSlotFlagHasSignal |
                                                     snapshot_shm::kSnapshotSlotFlagSignalFresh,
                                                 snapshot_shm::SnapshotPredictionState::kFresh);

    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 40;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 4;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());

    ActiveStrategyConfig active_strategy_config{};
    active_strategy_config.enabled = true;
    active_strategy_config.name = "prediction_signal";
    active_strategy_config.signal_threshold = 0.5;
    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(),
                                     StrategyRegistry::create(active_strategy_config));

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(901, 100, PassiveExecutionAlgo::TWAP);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(child_index));

    order_slot_snapshot child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), child_index, child_snapshot));
    assert(child_snapshot.request.active_strategy_claimed == 1);
    assert(child_snapshot.request.passive_execution_algo == PassiveExecutionAlgo::None);
    assert(child_snapshot.request.dprice_entrust == 999);

    loop.stop();
    worker.join();
}

TEST(active_strategy_respects_volume_and_price_decision) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine_fixed_decision", 1.0F,
                                                 snapshot_shm::kSnapshotSlotFlagHasSignal |
                                                     snapshot_shm::kSnapshotSlotFlagSignalFresh,
                                                 snapshot_shm::SnapshotPredictionState::kFresh);

    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 40;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 4;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());

    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(),
                                     std::make_unique<FixedDecisionStrategy>(17, 998));

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(902, 100, PassiveExecutionAlgo::TWAP);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(child_index));

    order_slot_snapshot child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), child_index, child_snapshot));
    assert(child_snapshot.request.active_strategy_claimed == 1);
    assert(child_snapshot.request.volume_entrust == 17);
    assert(child_snapshot.request.dprice_entrust == 999);

    loop.stop();
    worker.join();
}

TEST(cancelled_parent_stops_future_twap_slices) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine_cancel", 0.0F, 0,
                                                 snapshot_shm::SnapshotPredictionState::kNone);
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 40;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 4;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());
    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(), nullptr);

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(903, 100, PassiveExecutionAlgo::TWAP);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex first_child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(first_child_index));

    order_slot_snapshot first_child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), first_child_index, first_child_snapshot));
    const InternalOrderId first_child_id = first_child_snapshot.request.internal_order_id;

    assert(router.route_cancel(request.internal_order_id, 9900, 93100000));

    TradeResponse finished_response{};
    finished_response.internal_order_id = first_child_id;
    finished_response.internal_security_id = InternalSecurityId("XSHE_000001");
    finished_response.trade_side = TradeSide::Buy;
    finished_response.new_state = OrderState::Finished;
    finished_response.recv_time_ns = now_ns();
    assert(trades->response_queue.try_push(finished_response));

    assert(wait_until([&book, first_child_id]() {
        const OrderEntry* child = book->find_order(first_child_id);
        return child && child->request.order_state.load(std::memory_order_acquire) == OrderState::Finished;
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::size_t new_child_count = 0;
    OrderIndex queued_index = kInvalidOrderIndex;
    while (downstream->order_queue.try_pop(queued_index)) {
        order_slot_snapshot snapshot{};
        assert(orders_shm_read_snapshot(orders_shm.get(), queued_index, snapshot));
        if (snapshot.request.order_type == OrderType::New) {
            ++new_child_count;
        }
    }
    assert(new_child_count == 0);

    loop.stop();
    worker.join();
}

TEST(fixed_size_managed_parent_releases_next_clip) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine_fixed_size", 0.0F, 0,
                                                 snapshot_shm::SnapshotPredictionState::kNone);
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 40;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 4;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());
    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(), nullptr);

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(904, 100, PassiveExecutionAlgo::FixedSize);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex first_child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(first_child_index));

    order_slot_snapshot first_child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), first_child_index, first_child_snapshot));
    assert(first_child_snapshot.request.volume_entrust == 40);
    assert(first_child_snapshot.request.dprice_entrust == 999);

    const OrderEntry* parent_before_finish = book->find_order(904);
    assert(parent_before_finish != nullptr);
    assert(parent_before_finish->request.execution_algo == PassiveExecutionAlgo::FixedSize);
    assert(parent_before_finish->request.execution_state == ExecutionState::Running);
    assert(parent_before_finish->request.target_volume == 100);
    assert(parent_before_finish->request.working_volume == 40);
    assert(parent_before_finish->request.schedulable_volume == 60);

    TradeResponse first_finished{};
    first_finished.internal_order_id = first_child_snapshot.request.internal_order_id;
    first_finished.internal_security_id = InternalSecurityId("XSHE_000001");
    first_finished.trade_side = TradeSide::Buy;
    first_finished.new_state = OrderState::Finished;
    first_finished.volume_traded = 40;
    first_finished.dprice_traded = 1000;
    first_finished.dvalue_traded = 40000;
    first_finished.recv_time_ns = now_ns();
    assert(trades->response_queue.try_push(first_finished));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex second_child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(second_child_index));

    order_slot_snapshot second_child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), second_child_index, second_child_snapshot));
    assert(second_child_snapshot.request.volume_entrust == 40);
    assert(second_child_snapshot.request.dprice_entrust == 999);

    const OrderEntry* parent_after_second_submit = book->find_order(904);
    assert(parent_after_second_submit != nullptr);
    assert(parent_after_second_submit->request.volume_traded == 40);
    assert(parent_after_second_submit->request.volume_remain == 60);
    assert(parent_after_second_submit->request.working_volume == 40);
    assert(parent_after_second_submit->request.schedulable_volume == 20);

    loop.stop();
    worker.join();
}

TEST(iceberg_enters_managed_execution_session) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine_iceberg", 0.0F, 0,
                                                 snapshot_shm::SnapshotPredictionState::kNone);
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 30;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 8;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());
    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(), nullptr);

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(905, 90, PassiveExecutionAlgo::Iceberg);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }, 1500));

    OrderIndex child_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(child_index));

    order_slot_snapshot child_snapshot{};
    assert(orders_shm_read_snapshot(orders_shm.get(), child_index, child_snapshot));
    assert(child_snapshot.request.volume_entrust == 30);
    assert(child_snapshot.request.dprice_entrust == 999);

    const OrderEntry* parent = book->find_order(905);
    assert(parent != nullptr);
    assert(parent->request.execution_algo == PassiveExecutionAlgo::Iceberg);
    assert(parent->request.execution_state == ExecutionState::Running);
    assert(parent->request.target_volume == 90);

    loop.stop();
    worker.join();
}

TEST(vwap_is_rejected_without_falling_back_to_legacy_splitter) {
    ManagedMarketDataFixture market_data_fixture("acct_exec_engine_vwap", 0.0F, 0,
                                                 snapshot_shm::SnapshotPredictionState::kNone);
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    RiskConfig risk_config;
    risk_config.enable_position_check = false;
    risk_config.enable_price_limit_check = false;
    risk_config.enable_duplicate_check = false;
    RiskManager risk(positions, risk_config);

    auto book = std::make_unique<OrderBook>();
    split_config split_config{};
    split_config.strategy = SplitStrategy::None;
    split_config.max_child_volume = 40;
    split_config.min_child_volume = 1;
    split_config.max_child_count = 4;
    split_config.interval_ms = 1;
    order_router router(*book, downstream.get(), orders_shm.get());
    ExecutionEngine execution_engine(split_config, *book, router, market_data_fixture.service(), nullptr);

    EventLoopConfig loop_config{};
    loop_config.busy_polling = false;
    loop_config.idle_sleep_us = 50;
    loop_config.poll_batch_size = 32;
    loop_config.stats_interval_ms = 0;
    EventLoop loop(loop_config, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, &execution_engine);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest request = make_managed_order(906, 100, PassiveExecutionAlgo::VWAP);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders_shm.get(), request, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy,
                             now_ns(), parent_index));
    assert(upstream->upstream_order_queue.try_push(parent_index));

    assert(wait_until([&book]() {
        const OrderEntry* parent = book->find_order(906);
        return parent != nullptr &&
               parent->request.order_state.load(std::memory_order_acquire) == OrderState::TraderRejected;
    }));
    assert(downstream->order_queue.size() == 0);

    loop.stop();
    worker.join();
}

int main() {
    printf("=== Execution Engine Test Suite ===\n\n");

    RUN_TEST(twap_falls_back_without_active_strategy);
    RUN_TEST(twap_uses_active_strategy_with_fresh_prediction);
    RUN_TEST(active_strategy_respects_volume_and_price_decision);
    RUN_TEST(cancelled_parent_stops_future_twap_slices);
    RUN_TEST(fixed_size_managed_parent_releases_next_clip);
    RUN_TEST(iceberg_enters_managed_execution_session);
    RUN_TEST(vwap_is_rejected_without_falling_back_to_legacy_splitter);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
