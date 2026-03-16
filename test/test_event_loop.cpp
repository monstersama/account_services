#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>

#include "common/constants.hpp"
#include "core/event_loop.hpp"
#include "order/order_router.hpp"
#include "portfolio/account_info.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_manager.hpp"
#include "shm/orders_shm.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        printf("Running %s... ", #name);                                                                               \
        test_##name();                                                                                                 \
        printf("PASSED\n");                                                                                            \
    } while (0)

namespace {

using namespace acct_service;

void init_header(SHMHeader& header) {
    header.magic = SHMHeader::kMagic;
    header.version = SHMHeader::kVersion;
    header.create_time = now_ns();
    header.last_update = header.create_time;
    header.next_order_id.store(1, std::memory_order_relaxed);
}

std::unique_ptr<upstream_shm_layout> make_upstream_shm() {
    auto shm = std::make_unique<upstream_shm_layout>();
    init_header(shm->header);
    shm->upstream_order_queue.init();
    return shm;
}

std::unique_ptr<downstream_shm_layout> make_downstream_shm() {
    auto shm = std::make_unique<downstream_shm_layout>();
    init_header(shm->header);
    shm->order_queue.init();
    return shm;
}

std::unique_ptr<trades_shm_layout> make_trades_shm() {
    auto shm = std::make_unique<trades_shm_layout>();
    init_header(shm->header);
    shm->response_queue.init();
    return shm;
}

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

std::unique_ptr<positions_shm_layout> make_positions_shm() {
    auto shm = std::make_unique<positions_shm_layout>();
    shm->header.magic = PositionsHeader::kMagic;
    shm->header.version = PositionsHeader::kVersion;
    shm->header.header_size = static_cast<uint32_t>(sizeof(PositionsHeader));
    shm->header.total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
    shm->header.capacity = static_cast<uint32_t>(kMaxPositions);
    shm->header.init_state = 0;
    shm->header.create_time = 0;
    shm->header.last_update = 0;
    shm->header.id.store(1, std::memory_order_relaxed);
    shm->position_count.store(0, std::memory_order_relaxed);
    return shm;
}

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

OrderRequest make_order(InternalOrderId order_id, Volume volume) {
    OrderRequest req;
    req.init_new(
        "000001", InternalSecurityId("XSHE_000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000, 93000000);
    req.order_state.store(OrderState::StrategySubmitted, std::memory_order_relaxed);
    return req;
}

} // namespace

TEST(process_order_and_trade_response) {
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));
    assert(positions.add_security("000001", "PingAn", Market::SZ) == std::string_view("XSHE_000001"));

    RiskConfig risk_cfg;
    risk_cfg.enable_position_check = false;
    risk_cfg.enable_price_limit_check = false;
    risk_cfg.enable_duplicate_check = false;
    risk_cfg.max_order_volume = 0;
    risk_cfg.max_order_value = 0;
    risk_cfg.max_orders_per_second = 0;
    RiskManager risk(positions, risk_cfg);

    auto book = std::make_unique<OrderBook>();
    split_config split_cfg;
    split_cfg.strategy = SplitStrategy::None;
    order_router router(*book, downstream.get(), orders_shm.get());

    EventLoopConfig loop_cfg;
    loop_cfg.busy_polling = false;
    loop_cfg.idle_sleep_us = 50;
    loop_cfg.poll_batch_size = 32;
    loop_cfg.stats_interval_ms = 0;
    EventLoop loop(loop_cfg, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, nullptr, nullptr);

    std::thread worker([&loop]() { loop.run(); });

    const InternalOrderId order_id = 500;
    OrderRequest req = make_order(order_id, 100);
    OrderIndex order_index = kInvalidOrderIndex;
    assert(orders_shm_append(
        orders_shm.get(), req, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy, now_ns(), order_index));
    assert(upstream->upstream_order_queue.try_push(order_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }));

    OrderIndex downstream_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(downstream_index));
    order_slot_snapshot sent_snapshot;
    assert(orders_shm_read_snapshot(orders_shm.get(), downstream_index, sent_snapshot));
    assert(sent_snapshot.request.internal_order_id == order_id);
    assert(sent_snapshot.request.order_type == OrderType::New);

    TradeResponse rsp{};
    rsp.internal_order_id = order_id;
    rsp.internal_security_id = InternalSecurityId("XSHE_000001");
    rsp.trade_side = TradeSide::Buy;
    rsp.new_state = OrderState::MarketAccepted;
    rsp.volume_traded = 50;
    rsp.dprice_traded = 1000;
    rsp.dvalue_traded = 50000;
    rsp.dfee = 10;
    rsp.md_time_traded = 93100000;
    rsp.recv_time_ns = now_ns();

    assert(trades->response_queue.try_push(rsp));

    assert(wait_until([&book, order_id]() {
        const OrderEntry* order = book->find_order(order_id);
        return order && order->request.volume_traded == 50;
    }));

    const OrderEntry* order = book->find_order(order_id);
    assert(order != nullptr);
    assert(order->request.volume_traded == 50);
    assert(order->request.order_state.load(std::memory_order_acquire) == OrderState::MarketAccepted);

    const position* pos = positions.get_position(InternalSecurityId("XSHE_000001"));
    assert(pos != nullptr);
    assert(pos->volume_buy_traded >= 50);

    account_info fee_model;
    const DValue reserved_total = 100 * 1000 + fee_model.calculate_fee(TradeSide::Buy, 100 * 1000);
    const fund_info fund = positions.get_fund_info();
    assert(fund.available == 100000000 - reserved_total);
    assert(fund.frozen == reserved_total - (50000 + 10));
    assert(fund.total_asset == 99999990);
    assert(fund.market_value == 50000);

    assert(wait_until([&loop]() { return loop.stats().responses_processed >= 1; }));

    loop.stop();
    worker.join();

    assert(loop.stats().orders_processed >= 1);
    assert(loop.stats().responses_processed >= 1);
}

TEST(delay_archive_allows_late_terminal_trade) {
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));
    assert(positions.add_security("000001", "PingAn", Market::SZ) == std::string_view("XSHE_000001"));

    RiskConfig risk_cfg;
    risk_cfg.enable_position_check = false;
    risk_cfg.enable_price_limit_check = false;
    risk_cfg.enable_duplicate_check = false;
    risk_cfg.max_order_volume = 0;
    risk_cfg.max_order_value = 0;
    risk_cfg.max_orders_per_second = 0;
    RiskManager risk(positions, risk_cfg);

    auto book = std::make_unique<OrderBook>();
    split_config split_cfg;
    split_cfg.strategy = SplitStrategy::None;
    order_router router(*book, downstream.get(), orders_shm.get());

    EventLoopConfig loop_cfg;
    loop_cfg.busy_polling = false;
    loop_cfg.idle_sleep_us = 50;
    loop_cfg.poll_batch_size = 32;
    loop_cfg.stats_interval_ms = 0;
    loop_cfg.archive_terminal_orders = true;
    loop_cfg.terminal_archive_delay_ms = 80;
    EventLoop loop(loop_cfg, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, nullptr, nullptr);

    std::thread worker([&loop]() { loop.run(); });

    const InternalOrderId order_id = 700;
    OrderRequest req = make_order(order_id, 100);
    OrderIndex order_index = kInvalidOrderIndex;
    assert(orders_shm_append(
        orders_shm.get(), req, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy, now_ns(), order_index));
    assert(upstream->upstream_order_queue.try_push(order_index));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }));
    OrderIndex downstream_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(downstream_index));

    TradeResponse first_terminal{};
    first_terminal.internal_order_id = order_id;
    first_terminal.internal_security_id = InternalSecurityId("XSHE_000001");
    first_terminal.trade_side = TradeSide::Buy;
    first_terminal.new_state = OrderState::Finished;
    first_terminal.recv_time_ns = now_ns();
    assert(trades->response_queue.try_push(first_terminal));

    assert(wait_until([&book, order_id]() {
        const OrderEntry* order = book->find_order(order_id);
        return order && order->request.order_state.load(std::memory_order_acquire) == OrderState::Finished;
    }));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    TradeResponse late_trade{};
    late_trade.internal_order_id = order_id;
    late_trade.internal_security_id = InternalSecurityId("XSHE_000001");
    late_trade.trade_side = TradeSide::Buy;
    late_trade.new_state = OrderState::Finished;
    late_trade.volume_traded = 40;
    late_trade.dprice_traded = 1000;
    late_trade.dvalue_traded = 40000;
    late_trade.dfee = 8;
    late_trade.md_time_traded = 93100000;
    late_trade.recv_time_ns = now_ns();
    assert(trades->response_queue.try_push(late_trade));

    assert(wait_until([&book, order_id]() {
        const OrderEntry* order = book->find_order(order_id);
        return order && order->request.volume_traded == 40;
    }));

    assert(wait_until([&book, order_id]() { return book->find_order(order_id) == nullptr; }, 1500));

    loop.stop();
    worker.join();

    assert(loop.stats().responses_processed >= 2);
}

TEST(reject_second_buy_after_fund_reservation) {
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders_shm = make_orders_shm();
    auto positions_shm = make_positions_shm();

    PositionManager positions(positions_shm.get());
    assert(positions.initialize(1));

    fund_info constrained_fund;
    constrained_fund.total_asset = 100000;
    constrained_fund.available = 100000;
    constrained_fund.frozen = 0;
    constrained_fund.market_value = 0;
    assert(positions.overwrite_fund_info(constrained_fund));

    RiskConfig risk_cfg;
    risk_cfg.enable_position_check = false;
    risk_cfg.enable_price_limit_check = false;
    risk_cfg.enable_duplicate_check = false;
    risk_cfg.max_order_volume = 0;
    risk_cfg.max_order_value = 0;
    risk_cfg.max_orders_per_second = 0;
    RiskManager risk(positions, risk_cfg);

    auto book = std::make_unique<OrderBook>();
    split_config split_cfg;
    split_cfg.strategy = SplitStrategy::None;
    order_router router(*book, downstream.get(), orders_shm.get());

    EventLoopConfig loop_cfg;
    loop_cfg.busy_polling = false;
    loop_cfg.idle_sleep_us = 50;
    loop_cfg.poll_batch_size = 32;
    loop_cfg.stats_interval_ms = 0;
    EventLoop loop(loop_cfg, upstream.get(), downstream.get(), trades.get(), orders_shm.get(), *book, router,
                   positions, risk, nullptr, nullptr, nullptr);

    std::thread worker([&loop]() { loop.run(); });

    OrderRequest first = make_order(800, 50);
    OrderRequest second = make_order(801, 50);
    OrderIndex first_index = kInvalidOrderIndex;
    OrderIndex second_index = kInvalidOrderIndex;
    assert(orders_shm_append(
        orders_shm.get(), first, OrderSlotState::UpstreamQueued, order_slot_source_t::Strategy, now_ns(), first_index));
    assert(orders_shm_append(orders_shm.get(),
        second,
        OrderSlotState::UpstreamQueued,
        order_slot_source_t::Strategy,
        now_ns(),
        second_index));
    assert(upstream->upstream_order_queue.try_push(first_index));
    assert(upstream->upstream_order_queue.try_push(second_index));

    assert(wait_until([&book]() {
        const OrderEntry* first_order = book->find_order(800);
        const OrderEntry* second_order = book->find_order(801);
        return first_order != nullptr && second_order != nullptr &&
               second_order->request.order_state.load(std::memory_order_acquire) == OrderState::RiskControllerRejected;
    }));

    const OrderEntry* first_order = book->find_order(800);
    const OrderEntry* second_order = book->find_order(801);
    assert(first_order != nullptr);
    assert(second_order != nullptr);
    assert(first_order->request.order_state.load(std::memory_order_acquire) == OrderState::TraderSubmitted);
    assert(second_order->request.order_state.load(std::memory_order_acquire) == OrderState::RiskControllerRejected);

    account_info fee_model;
    const DValue order_value = 50 * 1000;
    const DValue estimated_fee = fee_model.calculate_fee(TradeSide::Buy, order_value);
    const fund_info fund = positions.get_fund_info();
    assert(fund.available == 100000 - (order_value + estimated_fee));
    assert(fund.frozen == order_value + estimated_fee);

    loop.stop();
    worker.join();
}

int main() {
    printf("=== Event Loop Test Suite ===\n\n");

    RUN_TEST(process_order_and_trade_response);
    RUN_TEST(delay_archive_allows_late_terminal_trade);
    RUN_TEST(reject_second_buy_after_fund_reservation);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
