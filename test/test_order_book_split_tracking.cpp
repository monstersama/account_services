#include <algorithm>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

#include "order/order_book.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");            \
    } while (0)

namespace {

using namespace acct_service;

order_entry make_new_entry(internal_order_id_t order_id, volume_t volume, bool is_split_child = false,
    internal_order_id_t parent_order_id = 0) {
    order_entry entry{};
    entry.request.init_new(
        "000001", internal_security_id_t("SZ.000001"), order_id, trade_side_t::Buy, market_t::SZ, volume, 1000,
        93000000);
    entry.request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_relaxed);
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<strategy_id_t>(1);
    entry.risk_result = risk_result_t::Pass;
    entry.retry_count = 0;
    entry.is_split_child = is_split_child;
    entry.parent_order_id = parent_order_id;
    return entry;
}

bool contains(const std::vector<internal_order_id_t>& ids, internal_order_id_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

TEST(split_mapping_and_aggregation) {
    auto book = std::make_unique<order_book>();

    const internal_order_id_t parent_id = book->next_order_id();
    const internal_order_id_t child1_id = book->next_order_id();
    const internal_order_id_t child2_id = book->next_order_id();

    assert(book->add_order(make_new_entry(parent_id, 1000)));
    assert(book->add_order(make_new_entry(child1_id, 400, true, parent_id)));
    assert(book->add_order(make_new_entry(child2_id, 600, true, parent_id)));

    const auto children = book->get_children(parent_id);
    assert(children.size() == 2);
    assert(contains(children, child1_id));
    assert(contains(children, child2_id));

    internal_order_id_t reverse_parent = 0;
    assert(book->try_get_parent(child1_id, reverse_parent));
    assert(reverse_parent == parent_id);

    assert(book->update_status(child1_id, order_status_t::TraderSubmitted));
    assert(book->update_status(child2_id, order_status_t::TraderSubmitted));
    assert(book->update_trade(child1_id, 400, 1000, 400000, 10));
    assert(book->update_trade(child2_id, 600, 1000, 600000, 20));

    const order_entry* parent = book->find_order(parent_id);
    assert(parent != nullptr);
    assert(parent->request.volume_traded == 1000);
    assert(parent->request.volume_remain == 0);
    assert(parent->request.dvalue_traded == 1000000);
    assert(parent->request.dfee_executed == 30);
    assert(parent->request.order_status.load(std::memory_order_acquire) == order_status_t::Finished);

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
    auto book = std::make_unique<order_book>();

    const internal_order_id_t parent_id = book->next_order_id();
    const internal_order_id_t child1_id = book->next_order_id();
    const internal_order_id_t child2_id = book->next_order_id();

    assert(book->add_order(make_new_entry(parent_id, 1000)));
    assert(book->add_order(make_new_entry(child1_id, 500, true, parent_id)));
    assert(book->add_order(make_new_entry(child2_id, 500, true, parent_id)));

    assert(book->update_status(parent_id, order_status_t::TraderError));
    assert(book->update_status(child1_id, order_status_t::Finished));
    assert(book->update_status(child2_id, order_status_t::Finished));

    const order_entry* parent = book->find_order(parent_id);
    assert(parent != nullptr);
    assert(parent->request.order_status.load(std::memory_order_acquire) == order_status_t::TraderError);
}

int main() {
    printf("=== Order Book Split Tracking Test Suite ===\n\n");

    RUN_TEST(split_mapping_and_aggregation);
    RUN_TEST(parent_error_latch);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
