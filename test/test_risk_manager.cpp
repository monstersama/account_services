#include <cassert>
#include <cstdio>
#include <memory>

#include "common/constants.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        printf("Running %s... ", #name);                                                                               \
        test_##name();                                                                                                 \
        printf("PASSED\n");                                                                                            \
    } while (0)

namespace {

using namespace acct_service;

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

OrderRequest make_buy_order(InternalOrderId order_id, Volume volume) {
    OrderRequest req;
    req.init_new(
        "000001", InternalSecurityId("XSHE_000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000, 93000000);
    req.order_state.store(OrderState::StrategySubmitted, std::memory_order_relaxed);
    return req;
}

} // namespace

TEST(fund_and_duplicate_rules) {
    auto shm = make_positions_shm();
    PositionManager positions(shm.get());
    assert(positions.initialize(1));
    assert(positions.add_security("000001", "PingAn", Market::SZ) == std::string_view("XSHE_000001"));

    RiskConfig cfg;
    cfg.max_order_value = 0;
    cfg.max_order_volume = 0;
    cfg.max_orders_per_second = 0;
    cfg.enable_fund_check = true;
    cfg.enable_position_check = false;
    cfg.enable_price_limit_check = false;
    cfg.enable_duplicate_check = true;
    cfg.duplicate_window_ns = 1000000000ULL;

    RiskManager manager(positions, cfg);

    OrderRequest large = make_buy_order(1, 200000);
    const risk_check_result large_result = manager.check_order(large);
    assert(!large_result.passed());
    assert(large_result.code == RiskResult::RejectInsufficientFund);

    OrderRequest small = make_buy_order(2, 100);
    const risk_check_result first = manager.check_order(small);
    assert(first.passed());

    const risk_check_result second = manager.check_order(small);
    assert(!second.passed());
    assert(second.code == RiskResult::RejectDuplicateOrder);

    OrderRequest same_params_new_id = small;
    same_params_new_id.internal_order_id = 3;
    const risk_check_result third = manager.check_order(same_params_new_id);
    assert(third.passed());

    const RiskState& stats = manager.stats();
    assert(stats.total_checks == 4);
    assert(stats.passed == 2);
    assert(stats.rejected >= 2);
    assert(stats.rejected_duplicate >= 1);
}

TEST(fund_rule_reserves_fee_buffer) {
    auto shm = make_positions_shm();
    PositionManager positions(shm.get());
    assert(positions.initialize(1));

    RiskConfig cfg;
    cfg.max_order_value = 0;
    cfg.max_order_volume = 0;
    cfg.max_orders_per_second = 0;
    cfg.enable_fund_check = true;
    cfg.enable_position_check = false;
    cfg.enable_price_limit_check = false;
    cfg.enable_duplicate_check = false;

    RiskManager manager(positions, cfg);

    OrderRequest exact_value_no_fee = make_buy_order(10, 100000);
    const risk_check_result no_fee_result = manager.check_order(exact_value_no_fee);
    assert(!no_fee_result.passed());
    assert(no_fee_result.code == RiskResult::RejectInsufficientFund);

    OrderRequest explicit_fee_fit = make_buy_order(11, 99999);
    explicit_fee_fit.dfee_estimate = 1000;
    const risk_check_result explicit_fee_fit_result = manager.check_order(explicit_fee_fit);
    assert(explicit_fee_fit_result.passed());

    OrderRequest explicit_fee_over = make_buy_order(12, 99999);
    explicit_fee_over.dfee_estimate = 1001;
    const risk_check_result explicit_fee_over_result = manager.check_order(explicit_fee_over);
    assert(!explicit_fee_over_result.passed());
    assert(explicit_fee_over_result.code == RiskResult::RejectInsufficientFund);
}

int main() {
    printf("=== Risk Manager Test Suite ===\n\n");

    RUN_TEST(fund_and_duplicate_rules);
    RUN_TEST(fund_rule_reserves_fee_buffer);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
