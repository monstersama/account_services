#include <cassert>
#include <cstdio>
#include <memory>

#include "common/constants.hpp"
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

order_request make_buy_order(internal_order_id_t order_id, volume_t volume) {
    order_request req;
    req.init_new("000001", internal_security_id_t("SZ.000001"), order_id, trade_side_t::Buy, market_t::SZ, volume,
        1000, 93000000);
    req.order_status.store(order_status_t::StrategySubmitted, std::memory_order_relaxed);
    return req;
}

}  // namespace

TEST(fund_and_duplicate_rules) {
    auto shm = make_positions_shm();
    position_manager positions(shm.get());
    assert(positions.initialize(1));
    assert(positions.add_security("000001", "PingAn", market_t::SZ) == std::string_view("SZ.000001"));

    risk_config cfg;
    cfg.max_order_value = 0;
    cfg.max_order_volume = 0;
    cfg.max_orders_per_second = 0;
    cfg.enable_fund_check = true;
    cfg.enable_position_check = false;
    cfg.enable_price_limit_check = false;
    cfg.enable_duplicate_check = true;
    cfg.duplicate_window_ns = 1000000000ULL;

    risk_manager manager(positions, cfg);

    order_request large = make_buy_order(1, 200000);
    const risk_check_result large_result = manager.check_order(large);
    assert(!large_result.passed());
    assert(large_result.code == risk_result_t::RejectInsufficientFund);

    order_request small = make_buy_order(2, 100);
    const risk_check_result first = manager.check_order(small);
    assert(first.passed());

    const risk_check_result second = manager.check_order(small);
    assert(!second.passed());
    assert(second.code == risk_result_t::RejectDuplicateOrder);

    order_request same_params_new_id = small;
    same_params_new_id.internal_order_id = 3;
    const risk_check_result third = manager.check_order(same_params_new_id);
    assert(third.passed());

    const risk_stats& stats = manager.stats();
    assert(stats.total_checks == 4);
    assert(stats.passed == 2);
    assert(stats.rejected >= 2);
    assert(stats.rejected_duplicate >= 1);
}

int main() {
    printf("=== Risk Manager Test Suite ===\n\n");

    RUN_TEST(fund_and_duplicate_rules);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
