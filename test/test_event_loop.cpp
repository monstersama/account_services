#include <cassert>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

#include "common/constants.hpp"
#include "core/event_loop.hpp"
#include "order/order_router.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

void init_header(shm_header& header) {
    header.magic = shm_header::kMagic;
    header.version = shm_header::kVersion;
    header.create_time = now_ns();
    header.last_update = header.create_time;
    header.next_order_id.store(1, std::memory_order_relaxed);
}

std::unique_ptr<upstream_shm_layout> make_upstream_shm() {
    auto shm = std::make_unique<upstream_shm_layout>();
    init_header(shm->header);
    shm->strategy_order_queue.init();
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

std::unique_ptr<positions_shm_layout> make_positions_shm() {
    auto shm = std::make_unique<positions_shm_layout>();
    shm->header.magic = positions_header::kMagic;
    shm->header.version = positions_header::kVersion;
    shm->header.header_size = static_cast<uint32_t>(sizeof(positions_header));
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

order_request make_order(internal_order_id_t order_id, volume_t volume) {
    order_request req;
    req.init_new(
        "000001", static_cast<internal_security_id_t>(1), order_id, trade_side_t::Buy, market_t::SZ, volume, 1000,
        93000000);
    req.order_status.store(order_status_t::StrategySubmitted, std::memory_order_relaxed);
    return req;
}

}  // namespace

TEST(process_order_and_trade_response) {
    auto upstream = make_upstream_shm();
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto positions_shm = make_positions_shm();

    position_manager positions(positions_shm.get());
    assert(positions.initialize(1));
    assert(positions.add_security("000001", "PingAn", market_t::SZ) == 1);

    risk_config risk_cfg;
    risk_cfg.enable_position_check = false;
    risk_cfg.enable_price_limit_check = false;
    risk_cfg.enable_duplicate_check = false;
    risk_cfg.max_order_volume = 0;
    risk_cfg.max_order_value = 0;
    risk_cfg.max_orders_per_second = 0;
    risk_manager risk(positions, risk_cfg);

    auto book = std::make_unique<order_book>();
    split_config split_cfg;
    split_cfg.strategy = split_strategy_t::None;
    order_router router(*book, downstream.get(), split_cfg);

    event_loop_config loop_cfg;
    loop_cfg.busy_polling = false;
    loop_cfg.idle_sleep_us = 50;
    loop_cfg.poll_batch_size = 32;
    loop_cfg.stats_interval_ms = 0;
    event_loop loop(loop_cfg, upstream.get(), downstream.get(), *book, router, positions, risk);
    loop.set_trades_shm(trades.get());

    std::thread worker([&loop]() { loop.run(); });

    const internal_order_id_t order_id = 500;
    order_request req = make_order(order_id, 100);
    assert(upstream->strategy_order_queue.try_push(req));

    assert(wait_until([&downstream]() { return downstream->order_queue.size() > 0; }));

    order_request sent;
    assert(downstream->order_queue.try_pop(sent));
    assert(sent.internal_order_id == order_id);
    assert(sent.order_type == order_type_t::New);

    trade_response rsp{};
    rsp.internal_order_id = order_id;
    rsp.internal_security_id = static_cast<internal_security_id_t>(1);
    rsp.trade_side = trade_side_t::Buy;
    rsp.new_status = order_status_t::MarketAccepted;
    rsp.volume_traded = 50;
    rsp.dprice_traded = 1000;
    rsp.dvalue_traded = 50000;
    rsp.dfee = 10;
    rsp.md_time_traded = 93100000;
    rsp.recv_time_ns = now_ns();

    assert(trades->response_queue.try_push(rsp));

    assert(wait_until([&book, order_id]() {
        const order_entry* order = book->find_order(order_id);
        return order && order->request.volume_traded == 50;
    }));

    const order_entry* order = book->find_order(order_id);
    assert(order != nullptr);
    assert(order->request.volume_traded == 50);
    assert(order->request.order_status.load(std::memory_order_acquire) == order_status_t::MarketAccepted);

    const position* pos = positions.get_position(1);
    assert(pos != nullptr);
    assert(pos->volume_buy_traded >= 50);

    assert(wait_until([&loop]() { return loop.stats().responses_processed >= 1; }));

    loop.stop();
    worker.join();

    assert(loop.stats().orders_processed >= 1);
    assert(loop.stats().responses_processed >= 1);
}

int main() {
    printf("=== Event Loop Test Suite ===\n\n");

    RUN_TEST(process_order_and_trade_response);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
