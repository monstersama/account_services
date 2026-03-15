#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "gateway_config.hpp"
#include "gateway_loop.hpp"
#include "order/order_request.hpp"
#include "shm/orders_shm.hpp"
#include "shm/shm_layout.hpp"
#include "sim_broker_adapter.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        printf("Running %s... ", #name);                                                                               \
        test_##name();                                                                                                 \
        printf("PASSED\n");                                                                                            \
    } while (0)

namespace {

using namespace acct_service;

// 初始化共享内存头，模拟可用队列环境。
void init_header(SHMHeader& header) {
    header.magic = SHMHeader::kMagic;
    header.version = SHMHeader::kVersion;
    header.create_time = now_ns();
    header.last_update = header.create_time;
    header.next_order_id.store(1, std::memory_order_relaxed);
}

// 构造仅包含订单队列的下游共享内存。
std::unique_ptr<downstream_shm_layout> make_downstream_shm() {
    auto shm = std::make_unique<downstream_shm_layout>();
    init_header(shm->header);
    shm->order_queue.init();
    return shm;
}

// 构造仅包含回报队列的成交共享内存。
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

// 在超时时间内等待条件成立。
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

// 收集某个 internal_order_id 对应的回报状态序列。
std::vector<OrderState> collect_statuses_for_order(
    trades_shm_layout* trades, InternalOrderId order_id, std::size_t expected_count) {
    std::vector<OrderState> statuses;
    const bool done = wait_until(
        [&statuses, trades, order_id, expected_count]() {
            TradeResponse response;
            while (trades->response_queue.try_pop(response)) {
                if (response.internal_order_id == order_id) {
                    statuses.push_back(response.new_state);
                }
            }
            return statuses.size() >= expected_count;
        },
        1500);
    assert(done);
    return statuses;
}

// 辅助判断状态是否出现在回报序列中。
bool has_status(const std::vector<OrderState>& statuses, OrderState target) {
    for (OrderState value : statuses) {
        if (value == target) {
            return true;
        }
    }
    return false;
}

// 收集指定订单的完整回报，便于断言 cancelled_volume 等终态字段。
std::vector<TradeResponse> collect_responses_for_order(
    trades_shm_layout* trades, InternalOrderId order_id, std::size_t expected_count) {
    std::vector<TradeResponse> responses;
    const bool done = wait_until(
        [&responses, trades, order_id, expected_count]() {
            TradeResponse response;
            while (trades->response_queue.try_pop(response)) {
                if (response.internal_order_id == order_id) {
                    responses.push_back(response);
                }
            }
            return responses.size() >= expected_count;
        },
        1500);
    assert(done);
    return responses;
}

// 收集一批回报并交给调用方自行过滤，避免多订单场景下误丢其它回报。
std::vector<TradeResponse> collect_response_batch(trades_shm_layout* trades, std::size_t expected_count) {
    std::vector<TradeResponse> responses;
    const bool done = wait_until(
        [&responses, trades, expected_count]() {
            TradeResponse response;
            while (trades->response_queue.try_pop(response)) {
                responses.push_back(response);
            }
            return responses.size() >= expected_count;
        },
        1500);
    assert(done);
    return responses;
}

// 生成测试用 gateway 配置。
gateway::gateway_config make_config() {
    gateway::gateway_config config;
    config.poll_batch_size = 32;
    config.idle_sleep_us = 50;
    config.stats_interval_ms = 0;
    config.max_retry_attempts = 2;
    config.retry_interval_us = 100;
    return config;
}

} // namespace

// 验证新单在 gateway 中可完成“受理->成交->完成”闭环。
TEST(process_new_order_end_to_end) {
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders = make_orders_shm();

    gateway::sim_broker_adapter adapter;
    broker_api::broker_runtime_config runtime_config;
    runtime_config.account_id = 1;
    runtime_config.auto_fill = true;
    assert(adapter.initialize(runtime_config));

    gateway::gateway_loop loop(make_config(), downstream.get(), trades.get(), orders.get(), adapter);
    std::thread worker([&loop]() { (void)loop.run(); });

    OrderRequest request;
    request.init_new("000001",
        InternalSecurityId("XSHE_000001"),
        static_cast<InternalOrderId>(9001),
        TradeSide::Buy,
        Market::SZ,
        static_cast<Volume>(100),
        static_cast<DPrice>(1000),
        93000000);
    request.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);

    OrderIndex request_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(),
        request,
        OrderSlotState::DownstreamQueued,
        order_slot_source_t::AccountInternal,
        now_ns(),
        request_index));
    assert(downstream->order_queue.try_push(request_index));

    const std::vector<OrderState> statuses = collect_statuses_for_order(trades.get(), 9001, 3);

    loop.stop();
    worker.join();
    adapter.shutdown();

    assert(has_status(statuses, OrderState::BrokerAccepted));
    assert(has_status(statuses, OrderState::MarketAccepted));
    assert(has_status(statuses, OrderState::Finished));
    assert(loop.stats().orders_received >= 1);
    assert(loop.stats().responses_pushed >= 3);
}

// 验证撤单在 gateway 中可完成“受理->完成”闭环。
TEST(process_cancel_order_end_to_end) {
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders = make_orders_shm();

    gateway::sim_broker_adapter adapter;
    broker_api::broker_runtime_config runtime_config;
    runtime_config.account_id = 1;
    runtime_config.auto_fill = true;
    assert(adapter.initialize(runtime_config));

    gateway::gateway_loop loop(make_config(), downstream.get(), trades.get(), orders.get(), adapter);
    std::thread worker([&loop]() { (void)loop.run(); });

    OrderRequest cancel_request;
    cancel_request.init_cancel(static_cast<InternalOrderId>(9101), 93100000, static_cast<InternalOrderId>(9001));
    cancel_request.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);

    OrderIndex cancel_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(),
        cancel_request,
        OrderSlotState::DownstreamQueued,
        order_slot_source_t::AccountInternal,
        now_ns(),
        cancel_index));
    assert(downstream->order_queue.try_push(cancel_index));

    const std::vector<OrderState> statuses = collect_statuses_for_order(trades.get(), 9101, 2);

    loop.stop();
    worker.join();
    adapter.shutdown();

    assert(has_status(statuses, OrderState::BrokerAccepted));
    assert(has_status(statuses, OrderState::Finished));
}

// 验证部分成交后撤单完成会带权威 cancelled_volume。
TEST(cancel_finish_reports_authoritative_cancelled_volume) {
    auto downstream = make_downstream_shm();
    auto trades = make_trades_shm();
    auto orders = make_orders_shm();

    gateway::sim_broker_adapter adapter;
    broker_api::broker_runtime_config runtime_config;
    runtime_config.account_id = 1;
    runtime_config.auto_fill = false;
    assert(adapter.initialize(runtime_config));

    gateway::gateway_loop loop(make_config(), downstream.get(), trades.get(), orders.get(), adapter);
    std::thread worker([&loop]() { (void)loop.run(); });

    OrderRequest new_request;
    new_request.init_new("000001", InternalSecurityId("XSHE_000001"), static_cast<InternalOrderId>(9201),
                         TradeSide::Buy, Market::SZ, static_cast<Volume>(100), static_cast<DPrice>(1000), 93000000);
    new_request.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);

    OrderIndex new_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(), new_request, OrderSlotState::DownstreamQueued,
                             order_slot_source_t::AccountInternal, now_ns(), new_index));
    assert(downstream->order_queue.try_push(new_index));

    std::vector<TradeResponse> new_responses = collect_responses_for_order(trades.get(), 9201, 1);
    assert(new_responses.size() == 1);
    assert(new_responses.front().new_state == OrderState::BrokerAccepted);

    OrderRequest cancel_request;
    cancel_request.init_cancel(static_cast<InternalOrderId>(9202), 93100000, static_cast<InternalOrderId>(9201));
    cancel_request.internal_security_id = InternalSecurityId("XSHE_000001");
    cancel_request.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);

    OrderIndex cancel_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(), cancel_request, OrderSlotState::DownstreamQueued,
                             order_slot_source_t::AccountInternal, now_ns(), cancel_index));
    assert(downstream->order_queue.try_push(cancel_index));

    std::vector<TradeResponse> cancel_batch = collect_response_batch(trades.get(), 2);

    loop.stop();
    worker.join();
    adapter.shutdown();

    bool saw_cancel_accepted = false;
    bool saw_original_finished = false;
    for (const TradeResponse& response : cancel_batch) {
        if (response.internal_order_id == 9202 && response.new_state == OrderState::BrokerAccepted) {
            saw_cancel_accepted = true;
        }
        if (response.internal_order_id == 9201 && response.new_state == OrderState::Finished &&
            response.cancelled_volume == 100) {
            saw_original_finished = true;
        }
    }

    assert(saw_cancel_accepted);
    assert(saw_original_finished);
}

int main() {
    printf("=== Gateway Loop Test Suite ===\n\n");

    RUN_TEST(process_new_order_end_to_end);
    RUN_TEST(process_cancel_order_end_to_end);
    RUN_TEST(cancel_finish_reports_authoritative_cancelled_volume);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
