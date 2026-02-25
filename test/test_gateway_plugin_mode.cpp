#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "adapter_loader.hpp"
#include "gateway_config.hpp"
#include "gateway_loop.hpp"
#include "order/order_request.hpp"
#include "shm/orders_shm.hpp"
#include "shm/shm_layout.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

#ifndef TEST_ADAPTER_PLUGIN_PATH
#error "TEST_ADAPTER_PLUGIN_PATH is not defined"
#endif

void init_header(shm_header& header) {
    header.magic = shm_header::kMagic;
    header.version = shm_header::kVersion;
    header.create_time = now_ns();
    header.last_update = header.create_time;
    header.next_order_id.store(1, std::memory_order_relaxed);
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
    shm->header.magic = orders_header::kMagic;
    shm->header.version = orders_header::kVersion;
    shm->header.header_size = static_cast<uint32_t>(sizeof(orders_header));
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

bool wait_until(const std::function<bool()>& predicate, int timeout_ms = 1500) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool has_status(const std::vector<order_status_t>& statuses, order_status_t target) {
    for (order_status_t value : statuses) {
        if (value == target) {
            return true;
        }
    }
    return false;
}

gateway::gateway_config make_config() {
    gateway::gateway_config config;
    config.poll_batch_size = 32;
    config.idle_sleep_us = 50;
    config.stats_interval_ms = 0;
    config.max_retry_attempts = 2;
    config.retry_interval_us = 100;
    config.broker_type = "plugin";
    config.adapter_plugin_so = TEST_ADAPTER_PLUGIN_PATH;
    return config;
}

}  // namespace

TEST(process_new_order_with_plugin_adapter) {
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders = make_orders_shm();

    gateway::loaded_adapter loaded;
    std::string error_message;
    assert(gateway::load_adapter_plugin(TEST_ADAPTER_PLUGIN_PATH, loaded, error_message));
    assert(loaded.valid());

    broker_api::IBrokerAdapter* adapter = loaded.get();
    assert(adapter != nullptr);

    broker_api::broker_runtime_config runtime_config;
    runtime_config.account_id = 1;
    runtime_config.auto_fill = true;
    assert(adapter->initialize(runtime_config));

    gateway::gateway_loop loop(make_config(), downstream.get(), trades.get(), orders.get(), *adapter);
    std::thread worker([&loop]() { (void)loop.run(); });

    order_request request;
    request.init_new("000001", static_cast<internal_security_id_t>(1), static_cast<internal_order_id_t>(9301),
        trade_side_t::Buy, market_t::SZ, static_cast<volume_t>(120), static_cast<dprice_t>(1000), 93000000);
    request.order_status.store(order_status_t::TraderSubmitted, std::memory_order_relaxed);

    order_index_t request_index = kInvalidOrderIndex;
    assert(orders_shm_append(
        orders.get(), request, order_slot_stage_t::DownstreamQueued, order_slot_source_t::AccountInternal, now_ns(), request_index));
    assert(downstream->order_queue.try_push(request_index));

    std::vector<order_status_t> statuses;
    const bool got_all = wait_until([&]() {
        trade_response response;
        while (trades->response_queue.try_pop(response)) {
            if (response.internal_order_id == static_cast<internal_order_id_t>(9301)) {
                statuses.push_back(response.new_status);
            }
        }
        return statuses.size() >= 3;
    });

    loop.stop();
    worker.join();

    adapter->shutdown();
    loaded.reset();

    assert(got_all);
    assert(has_status(statuses, order_status_t::BrokerAccepted));
    assert(has_status(statuses, order_status_t::MarketAccepted));
    assert(has_status(statuses, order_status_t::Finished));
}

int main() {
    printf("=== Gateway Plugin Mode Test Suite ===\n\n");

    RUN_TEST(process_new_order_with_plugin_adapter);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
