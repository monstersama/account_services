#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

#include <unistd.h>

#include "core/account_service.hpp"
#include "order/order_request.hpp"
#include "shm/shm_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

std::string unique_shm_name(const char* prefix) {
    return std::string("/") + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(now_ns()));
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

}  // namespace

TEST(initialize_and_run_processes_orders) {
    config cfg;
    cfg.account_id = 101;
    cfg.shm.upstream_shm_name = unique_shm_name("acct_upstream");
    cfg.shm.downstream_shm_name = unique_shm_name("acct_downstream");
    cfg.shm.trades_shm_name = unique_shm_name("acct_trades");
    cfg.shm.positions_shm_name = unique_shm_name("acct_positions");
    cfg.shm.create_if_not_exist = true;

    cfg.event_loop.busy_polling = false;
    cfg.event_loop.idle_sleep_us = 50;
    cfg.event_loop.poll_batch_size = 32;
    cfg.event_loop.stats_interval_ms = 0;

    cfg.risk.enable_position_check = false;
    cfg.risk.enable_duplicate_check = false;
    cfg.risk.enable_price_limit_check = false;

    cfg.split.strategy = split_strategy_t::None;

    cfg.db.enable_persistence = false;
    cfg.db.db_path.clear();

    account_service service;
    assert(service.initialize(cfg));
    assert(service.state() == service_state_t::Ready);

    SHMManager upstream_manager;
    SHMManager downstream_manager;
    SHMManager trades_manager;

    upstream_shm_layout* upstream =
        upstream_manager.open_upstream(cfg.shm.upstream_shm_name, shm_mode::Open, cfg.account_id);
    downstream_shm_layout* downstream =
        downstream_manager.open_downstream(cfg.shm.downstream_shm_name, shm_mode::Open, cfg.account_id);
    trades_shm_layout* trades = trades_manager.open_trades(cfg.shm.trades_shm_name, shm_mode::Open, cfg.account_id);

    assert(upstream != nullptr);
    assert(downstream != nullptr);
    assert(trades != nullptr);

    int run_rc = -1;
    std::thread worker([&service, &run_rc]() { run_rc = service.run(); });

    order_request req;
    req.init_new("000001", static_cast<internal_security_id_t>(1), static_cast<internal_order_id_t>(5001),
        trade_side_t::Buy, market_t::SZ, static_cast<volume_t>(100), static_cast<dprice_t>(1000), 93000000);
    req.order_status.store(order_status_t::StrategySubmitted, std::memory_order_relaxed);

    assert(upstream->strategy_order_queue.try_push(req));

    assert(wait_until([downstream]() { return downstream->order_queue.size() > 0; }));

    order_request sent;
    assert(downstream->order_queue.try_pop(sent));
    assert(sent.order_type == order_type_t::New);
    assert(sent.security_id.view() == "000001");

    trade_response rsp{};
    rsp.internal_order_id = static_cast<internal_order_id_t>(5001);
    rsp.internal_security_id = static_cast<internal_security_id_t>(1);
    rsp.trade_side = trade_side_t::Buy;
    rsp.new_status = order_status_t::MarketAccepted;
    rsp.volume_traded = static_cast<volume_t>(50);
    rsp.dprice_traded = static_cast<dprice_t>(1000);
    rsp.dvalue_traded = static_cast<dvalue_t>(50000);
    rsp.dfee = static_cast<dvalue_t>(10);
    rsp.md_time_traded = 93100000;
    rsp.recv_time_ns = now_ns();

    assert(trades->response_queue.try_push(rsp));

    assert(wait_until([&service]() {
        const order_entry* order = service.orders().find_order(static_cast<internal_order_id_t>(5001));
        return order != nullptr && order->request.volume_traded == static_cast<volume_t>(50);
    }));

    const order_entry* order = service.orders().find_order(static_cast<internal_order_id_t>(5001));
    assert(order != nullptr);
    assert(order->request.volume_traded == static_cast<volume_t>(50));
    assert(order->request.order_status.load(std::memory_order_acquire) == order_status_t::MarketAccepted);

    service.stop();
    worker.join();

    assert(run_rc == 0);
    assert(service.state() == service_state_t::Stopped);

    upstream_manager.close();
    downstream_manager.close();
    trades_manager.close();

    (void)SHMManager::unlink(cfg.shm.upstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.downstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.trades_shm_name);
    (void)SHMManager::unlink(cfg.shm.positions_shm_name);
}

TEST(initialize_rejects_invalid_config) {
    config cfg;
    cfg.account_id = 0;

    account_service service;
    assert(!service.initialize(cfg));
}

int main() {
    printf("=== Account Service Test Suite ===\n\n");

    RUN_TEST(initialize_and_run_processes_orders);
    RUN_TEST(initialize_rejects_invalid_config);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
