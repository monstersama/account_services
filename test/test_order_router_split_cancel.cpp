#include <cassert>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

#include "order/order_router.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");            \
    } while (0)

namespace {

using namespace acct_service;

order_entry make_parent_entry(internal_order_id_t order_id, volume_t volume) {
    order_entry entry{};
    entry.request.init_new(
        "000001", static_cast<internal_security_id_t>(1), order_id, trade_side_t::Buy, market_t::SZ, volume, 1000,
        93000000);
    entry.request.order_status.store(order_status_t::RiskControllerAccepted, std::memory_order_relaxed);
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<strategy_id_t>(1);
    entry.risk_result = risk_result_t::Pass;
    entry.retry_count = 0;
    entry.is_split_child = false;
    entry.parent_order_id = 0;
    return entry;
}

std::unique_ptr<downstream_shm_layout> make_downstream() {
    auto downstream = std::make_unique<downstream_shm_layout>();
    downstream->header.magic = shm_header::kMagic;
    downstream->header.version = shm_header::kVersion;
    downstream->header.create_time = now_ns();
    downstream->header.last_update = downstream->header.create_time;
    downstream->header.next_order_id.store(1, std::memory_order_relaxed);
    downstream->order_queue.init();
    return downstream;
}

split_config make_split_config() {
    split_config cfg;
    cfg.strategy = split_strategy_t::FixedSize;
    cfg.max_child_volume = 100;
    cfg.min_child_volume = 1;
    cfg.max_child_count = 16;
    return cfg;
}

}  // namespace

TEST(split_cancel_fanout_tracks_parent) {
    auto book = std::make_unique<order_book>();
    auto downstream = make_downstream();
    order_router router(*book, downstream.get(), make_split_config());

    const internal_order_id_t parent_id = book->next_order_id();
    order_entry parent = make_parent_entry(parent_id, 250);
    assert(book->add_order(parent));

    assert(router.route_order(parent));

    const std::vector<internal_order_id_t> children = book->get_children(parent_id);
    assert(children.size() == 3);

    std::set<internal_order_id_t> child_ids_from_new_orders;
    order_request request;
    while (downstream->order_queue.try_pop(request)) {
        if (request.order_type == order_type_t::New) {
            child_ids_from_new_orders.insert(request.internal_order_id);
        }
    }
    assert(child_ids_from_new_orders.size() == 3);

    for (internal_order_id_t child_id : children) {
        assert(child_ids_from_new_orders.find(child_id) != child_ids_from_new_orders.end());
    }

    const internal_order_id_t parent_cancel_id = book->next_order_id();
    assert(router.route_cancel(parent_id, parent_cancel_id, 93100000));

    std::set<internal_order_id_t> cancelled_child_ids;
    std::size_t cancel_count = 0;
    while (downstream->order_queue.try_pop(request)) {
        assert(request.order_type == order_type_t::Cancel);
        cancelled_child_ids.insert(request.orig_internal_order_id);

        internal_order_id_t reverse_parent = 0;
        assert(book->try_get_parent(request.internal_order_id, reverse_parent));
        assert(reverse_parent == parent_id);
        ++cancel_count;
    }

    assert(cancel_count == 3);
    for (internal_order_id_t child_id : children) {
        assert(cancelled_child_ids.find(child_id) != cancelled_child_ids.end());
    }
}

TEST(partial_send_failure_latches_parent_error) {
    auto book = std::make_unique<order_book>();
    auto downstream = make_downstream();
    order_router router(*book, downstream.get(), make_split_config());

    const std::size_t capacity = decltype(downstream->order_queue)::capacity();
    order_request dummy;
    dummy.init_new(
        "000001", static_cast<internal_security_id_t>(1), 999999, trade_side_t::Buy, market_t::SZ, 1, 1000, 93000000);
    dummy.order_status.store(order_status_t::TraderSubmitted, std::memory_order_relaxed);

    for (std::size_t i = 0; i < capacity - 1; ++i) {
        dummy.internal_order_id = static_cast<internal_order_id_t>(1000 + i);
        assert(downstream->order_queue.try_push(dummy));
    }

    const internal_order_id_t parent_id = book->next_order_id();
    order_entry parent = make_parent_entry(parent_id, 300);
    assert(book->add_order(parent));

    assert(router.route_order(parent));

    const order_entry* parent_after = book->find_order(parent_id);
    assert(parent_after != nullptr);
    assert(parent_after->request.order_status.load(std::memory_order_acquire) == order_status_t::TraderError);

    const std::vector<internal_order_id_t> children = book->get_children(parent_id);
    assert(children.size() == 3);

    std::size_t submitted_children = 0;
    std::size_t error_children = 0;
    for (internal_order_id_t child_id : children) {
        const order_entry* child = book->find_order(child_id);
        assert(child != nullptr);
        const order_status_t status = child->request.order_status.load(std::memory_order_acquire);
        if (status == order_status_t::TraderSubmitted) {
            ++submitted_children;
        } else if (status == order_status_t::TraderError) {
            ++error_children;
        }
    }

    assert(submitted_children >= 1);
    assert(error_children >= 1);
}

int main() {
    printf("=== Order Router Split Cancel Test Suite ===\n\n");

    RUN_TEST(split_cancel_fanout_tracks_parent);
    RUN_TEST(partial_send_failure_latches_parent_error);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
