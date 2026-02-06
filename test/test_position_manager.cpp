#include <cassert>
#include <cstdio>
#include <memory>
#include <string_view>

#include "common/constants.hpp"
#include "portfolio/position_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

constexpr acct_service::dvalue_t kExpectedInitialFund = 100000000;

void setup_header(acct_service::positions_shm_layout& shm, uint32_t init_state) {
    shm.header.magic = acct_service::positions_header::kMagic;
    shm.header.version = acct_service::positions_header::kVersion;
    shm.header.header_size = static_cast<uint32_t>(sizeof(acct_service::positions_header));
    shm.header.total_size = static_cast<uint32_t>(sizeof(acct_service::positions_shm_layout));
    shm.header.capacity = static_cast<uint32_t>(acct_service::kMaxPositions);
    shm.header.init_state = init_state;
    shm.header.create_time = 0;
    shm.header.last_update = 0;
    shm.header.id.store(1, std::memory_order_relaxed);
    shm.position_count.store(0, std::memory_order_relaxed);
}

std::unique_ptr<acct_service::positions_shm_layout> make_shm(uint32_t init_state) {
    auto shm = std::make_unique<acct_service::positions_shm_layout>();
    setup_header(*shm, init_state);
    return shm;
}

void assert_fund_row_fields(const position& fund_row, const fund_info& fund) {
    assert(fund_total_asset_field(fund_row) == fund.total_asset);
    assert(fund_available_field(fund_row) == fund.available);
    assert(fund_frozen_field(fund_row) == fund.frozen);
    assert(fund_market_value_field(fund_row) == fund.market_value);
}

}  // namespace

TEST(initialize_sets_fund_row) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());

    const bool ok = manager.initialize(1);
    assert(ok);
    assert(manager.position_count() == 0);

    const position& fund_row = shm->positions[kFundPositionIndex];
    assert(fund_row.id == std::string_view(kFundPositionId));
    assert(fund_row.name == std::string_view(kFundPositionId));

    const fund_info fund = manager.get_fund_info();
    assert(fund.total_asset == kExpectedInitialFund);
    assert(fund.available == kExpectedInitialFund);
    assert(fund.frozen == 0);
    assert(fund.market_value == 0);
    assert_fund_row_fields(fund_row, fund);
}

TEST(add_security_uses_row_index_and_excludes_fund_row) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    const internal_security_id_t first = manager.add_security("000001", "PingAn", market_t::SZ);
    const internal_security_id_t second = manager.add_security("600000", "PuFa", market_t::SH);
    assert(first == 1);
    assert(second == 2);
    assert(manager.position_count() == 2);

    assert(shm->positions[1].id == std::string_view("000001"));
    assert(shm->positions[2].id == std::string_view("600000"));
    assert(manager.find_security_id("000001").value_or(0) == 1);
    assert(manager.find_security_id("600000").value_or(0) == 2);

    const auto all_positions = manager.get_all_positions();
    assert(all_positions.size() == 2);
    assert(all_positions[0]->id == std::string_view("000001"));
    assert(all_positions[1]->id == std::string_view("600000"));
}

TEST(fund_ops_write_into_fund_row) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    assert(manager.freeze_fund(100, 1));
    assert(manager.unfreeze_fund(40, 1));
    assert(manager.deduct_fund(50, 10, 1));
    assert(manager.add_fund(20, 1));
    assert(!manager.deduct_fund(1, 0, 2));

    const fund_info fund = manager.get_fund_info();
    assert(fund.total_asset == 100000010);
    assert(fund.available == 99999960);
    assert(fund.frozen == 0);
    assert(fund.market_value == 50);

    const position& fund_row = shm->positions[kFundPositionIndex];
    assert_fund_row_fields(fund_row, fund);
}

TEST(add_position_requires_registered_security) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    assert(!manager.add_position(1, 100, 123, 1));

    const internal_security_id_t sec_id = manager.add_security("000001", "PingAn", market_t::SZ);
    assert(sec_id == 1);
    assert(manager.add_position(sec_id, 100, 123, 2));

    const position* pos = manager.get_position(sec_id);
    assert(pos != nullptr);
    assert(pos->volume_buy == 100);
    assert(pos->dvalue_buy == 12300);
    assert(pos->volume_available_t1 == 100);
}

TEST(initialize_rebuilds_code_map_from_existing_rows) {
    using namespace acct_service;

    auto shm = make_shm(1);
    shm->position_count.store(2, std::memory_order_relaxed);
    shm->positions[1].id.assign("000001");
    shm->positions[1].name.assign("PingAn");
    shm->positions[2].id.assign("600000");
    shm->positions[2].name.assign("PuFa");

    position_manager manager(shm.get());
    assert(manager.initialize(1));

    assert(shm->positions[kFundPositionIndex].id == std::string_view(kFundPositionId));
    assert(manager.position_count() == 2);
    assert(manager.find_security_id("000001").value_or(0) == 1);
    assert(manager.find_security_id("600000").value_or(0) == 2);
}

int main() {
    printf("=== Position Manager Test Suite ===\n\n");

    RUN_TEST(initialize_sets_fund_row);
    RUN_TEST(add_security_uses_row_index_and_excludes_fund_row);
    RUN_TEST(fund_ops_write_into_fund_row);
    RUN_TEST(add_position_requires_registered_security);
    RUN_TEST(initialize_rebuilds_code_map_from_existing_rows);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
