#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

#include "order/order_book.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        printf("Running %s... ", #name);                                                                               \
        test_##name();                                                                                                 \
        printf("PASSED\n");                                                                                            \
    } while (0)

namespace {

using namespace acct_service;

OrderEntry make_new_entry(
    InternalOrderId order_id, Volume volume, bool is_split_child = false, InternalOrderId parent_order_id = 0) {
    OrderEntry entry{};
    entry.request.init_new(
        "000001", InternalSecurityId("XSHE_000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000, 93000000);
    entry.request.order_state.store(OrderState::StrategySubmitted, std::memory_order_relaxed);
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<StrategyId>(1);
    entry.risk_result = RiskResult::Pass;
    entry.retry_count = 0;
    entry.is_split_child = is_split_child;
    entry.parent_order_id = parent_order_id;
    return entry;
}

bool contains(const std::vector<InternalOrderId>& ids, InternalOrderId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

TEST(split_mapping_and_aggregation) {
    auto book = std::make_unique<OrderBook>();

    const InternalOrderId parent_id = book->next_order_id();
    const InternalOrderId child1_id = book->next_order_id();
    const InternalOrderId child2_id = book->next_order_id();

    assert(book->add_order(make_new_entry(parent_id, 1000)));
    assert(book->add_order(make_new_entry(child1_id, 400, true, parent_id)));
    assert(book->add_order(make_new_entry(child2_id, 600, true, parent_id)));

    const auto children = book->get_children(parent_id);
    assert(children.size() == 2);
    assert(contains(children, child1_id));
    assert(contains(children, child2_id));

    InternalOrderId reverse_parent = 0;
    assert(book->try_get_parent(child1_id, reverse_parent));
    assert(reverse_parent == parent_id);

    assert(book->update_state(child1_id, OrderState::TraderSubmitted));
    assert(book->update_state(child2_id, OrderState::TraderSubmitted));
    assert(book->update_trade(child1_id, 400, 1000, 400000, 10));
    assert(book->update_trade(child2_id, 600, 1000, 600000, 20));

    const OrderEntry* parent = book->find_order(parent_id);
    assert(parent != nullptr);
    assert(parent->request.volume_traded == 1000);
    assert(parent->request.volume_remain == 0);
    assert(parent->request.dvalue_traded == 1000000);
    assert(parent->request.dfee_executed == 30);
    assert(parent->request.order_state.load(std::memory_order_acquire) == OrderState::Finished);

    assert(book->archive_order(child1_id));
    const auto children_after_archive = book->get_children(parent_id);
    assert(children_after_archive.size() == 2);
    assert(contains(children_after_archive, child1_id));

    reverse_parent = 0;
    assert(book->try_get_parent(child1_id, reverse_parent));
    assert(reverse_parent == parent_id);

    book->clear();
    assert(book->get_children(parent_id).empty());
    assert(!book->try_get_parent(child2_id, reverse_parent));
}

TEST(parent_error_latch) {
    auto book = std::make_unique<OrderBook>();

    const InternalOrderId parent_id = book->next_order_id();
    const InternalOrderId child1_id = book->next_order_id();
    const InternalOrderId child2_id = book->next_order_id();

    assert(book->add_order(make_new_entry(parent_id, 1000)));
    assert(book->add_order(make_new_entry(child1_id, 500, true, parent_id)));
    assert(book->add_order(make_new_entry(child2_id, 500, true, parent_id)));

    assert(book->update_state(parent_id, OrderState::TraderError));
    assert(book->update_state(child1_id, OrderState::Finished));
    assert(book->update_state(child2_id, OrderState::Finished));

    const OrderEntry* parent = book->find_order(parent_id);
    assert(parent != nullptr);
    assert(parent->request.order_state.load(std::memory_order_acquire) == OrderState::TraderError);
}

TEST(managed_parent_view_skips_legacy_child_aggregation) {
    auto book = std::make_unique<OrderBook>();

    const InternalOrderId parent_id = book->next_order_id();
    const InternalOrderId child_id = book->next_order_id();

    assert(book->add_order(make_new_entry(parent_id, 100)));
    assert(book->mark_managed_parent(parent_id));
    assert(book->add_order(make_new_entry(child_id, 40, true, parent_id)));

    ManagedParentView view{};
    view.execution_algo = PassiveExecutionAlgo::FixedSize;
    view.execution_state = ExecutionState::Running;
    view.target_volume = 100;
    view.working_volume = 40;
    view.schedulable_volume = 60;
    view.confirmed_traded_volume = 0;
    view.confirmed_traded_value = 0;
    view.confirmed_fee = 0;
    assert(book->sync_managed_parent_view(parent_id, view, OrderState::TraderPending));

    assert(book->update_trade(child_id, 40, 1000, 40000, 5));

    const OrderEntry* parent = book->find_order(parent_id);
    assert(parent != nullptr);
    assert(parent->request.execution_algo == PassiveExecutionAlgo::FixedSize);
    assert(parent->request.execution_state == ExecutionState::Running);
    assert(parent->request.target_volume == 100);
    assert(parent->request.working_volume == 40);
    assert(parent->request.schedulable_volume == 60);
    assert(parent->request.volume_traded == 0);
    assert(parent->request.volume_remain == 100);
}

TEST(ensure_next_order_id_at_least) {
    auto book = std::make_unique<OrderBook>();

    const InternalOrderId first = book->next_order_id();
    assert(first == static_cast<InternalOrderId>(1));

    book->ensure_next_order_id_at_least(static_cast<InternalOrderId>(5000));
    const InternalOrderId seeded = book->next_order_id();
    assert(seeded == static_cast<InternalOrderId>(5000));

    book->ensure_next_order_id_at_least(static_cast<InternalOrderId>(100));
    const InternalOrderId next = book->next_order_id();
    assert(next == static_cast<InternalOrderId>(5001));
}

int main() {
    printf("=== Order Book Split Tracking Test Suite ===\n\n");

    RUN_TEST(split_mapping_and_aggregation);
    RUN_TEST(parent_error_latch);
    RUN_TEST(managed_parent_view_skips_legacy_child_aggregation);
    RUN_TEST(ensure_next_order_id_at_least);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
