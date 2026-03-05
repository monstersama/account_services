#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "order/order_router.hpp"
#include "shm/orders_shm.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");            \
    } while (0)

namespace {

using namespace acct_service;

order_entry make_parent_entry(InternalOrderId order_id, Volume volume) {
    order_entry entry{};
    entry.request.init_new(
        "000001", InternalSecurityId("SZ.000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000,
        93000000);
    entry.request.order_state.store(OrderState::RiskControllerAccepted, std::memory_order_relaxed);
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<StrategyId>(1);
    entry.risk_result = RiskResult::Pass;
    entry.retry_count = 0;
    entry.is_split_child = false;
    entry.parent_order_id = 0;
    return entry;
}

std::unique_ptr<downstream_shm_layout> make_downstream() {
    auto downstream = std::make_unique<downstream_shm_layout>();
    downstream->header.magic = SHMHeader::kMagic;
    downstream->header.version = SHMHeader::kVersion;
    downstream->header.create_time = now_ns();
    downstream->header.last_update = downstream->header.create_time;
    downstream->header.next_order_id.store(1, std::memory_order_relaxed);
    downstream->order_queue.init();
    return downstream;
}

std::unique_ptr<orders_shm_layout> make_orders() {
    auto orders = std::make_unique<orders_shm_layout>();
    orders->header.magic = OrdersHeader::kMagic;
    orders->header.version = OrdersHeader::kVersion;
    orders->header.header_size = static_cast<uint32_t>(sizeof(OrdersHeader));
    orders->header.total_size = static_cast<uint32_t>(sizeof(orders_shm_layout));
    orders->header.capacity = static_cast<uint32_t>(kDailyOrderPoolCapacity);
    orders->header.init_state = 1;
    orders->header.create_time = now_ns();
    orders->header.last_update = orders->header.create_time;
    orders->header.next_index.store(0, std::memory_order_relaxed);
    orders->header.full_reject_count.store(0, std::memory_order_relaxed);
    std::memcpy(orders->header.trading_day, "19700101", 9);
    return orders;
}

split_config make_split_config() {
    split_config cfg;
    cfg.strategy = SplitStrategy::FixedSize;
    cfg.max_child_volume = 100;
    cfg.min_child_volume = 1;
    cfg.max_child_count = 16;
    return cfg;
}

}  // namespace

TEST(split_cancel_fanout_tracks_parent) {
    auto book = std::make_unique<OrderBook>();
    auto downstream = make_downstream();
    auto orders = make_orders();
    order_router router(*book, downstream.get(), orders.get(), make_split_config());

    const InternalOrderId parent_id = book->next_order_id();
    order_entry parent = make_parent_entry(parent_id, 250);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(), parent.request, OrderSlotState::UpstreamDequeued,
        order_slot_source_t::AccountInternal, now_ns(), parent_index));
    parent.shm_order_index = parent_index;
    assert(book->add_order(parent));

    assert(router.route_order(parent));

    const std::vector<InternalOrderId> children = book->get_children(parent_id);
    assert(children.size() == 3);

    std::set<InternalOrderId> child_ids_from_new_orders;
    OrderIndex index = kInvalidOrderIndex;
    while (downstream->order_queue.try_pop(index)) {
        order_slot_snapshot snapshot;
        assert(orders_shm_read_snapshot(orders.get(), index, snapshot));
        const OrderRequest& request = snapshot.request;
        if (request.order_type == OrderType::New) {
            child_ids_from_new_orders.insert(request.internal_order_id);
        }
    }
    assert(child_ids_from_new_orders.size() == 3);

    for (InternalOrderId child_id : children) {
        assert(child_ids_from_new_orders.find(child_id) != child_ids_from_new_orders.end());
    }

    const InternalOrderId parent_cancel_id = book->next_order_id();
    assert(router.route_cancel(parent_id, parent_cancel_id, 93100000));

    std::set<InternalOrderId> cancelled_child_ids;
    std::size_t cancel_count = 0;
    while (downstream->order_queue.try_pop(index)) {
        order_slot_snapshot snapshot;
        assert(orders_shm_read_snapshot(orders.get(), index, snapshot));
        const OrderRequest& request = snapshot.request;
        assert(request.order_type == OrderType::Cancel);
        cancelled_child_ids.insert(request.orig_internal_order_id);

        InternalOrderId reverse_parent = 0;
        assert(book->try_get_parent(request.internal_order_id, reverse_parent));
        assert(reverse_parent == parent_id);
        ++cancel_count;
    }

    assert(cancel_count == 3);
    for (InternalOrderId child_id : children) {
        assert(cancelled_child_ids.find(child_id) != cancelled_child_ids.end());
    }
}

TEST(partial_send_failure_latches_parent_error) {
    auto book = std::make_unique<OrderBook>();
    auto downstream = make_downstream();
    auto orders = make_orders();
    order_router router(*book, downstream.get(), orders.get(), make_split_config());

    const std::size_t capacity = decltype(downstream->order_queue)::capacity();
    OrderRequest dummy;
    dummy.init_new(
        "000001", InternalSecurityId("SZ.000001"), 999999, TradeSide::Buy, Market::SZ, 1, 1000, 93000000);
    dummy.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);

    for (std::size_t i = 0; i < capacity - 1; ++i) {
        dummy.internal_order_id = static_cast<InternalOrderId>(1000 + i);
        OrderIndex dummy_index = kInvalidOrderIndex;
        assert(orders_shm_append(orders.get(), dummy, OrderSlotState::DownstreamQueued,
            order_slot_source_t::AccountInternal, now_ns(), dummy_index));
        assert(downstream->order_queue.try_push(dummy_index));
    }

    const InternalOrderId parent_id = book->next_order_id();
    order_entry parent = make_parent_entry(parent_id, 300);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(), parent.request, OrderSlotState::UpstreamDequeued,
        order_slot_source_t::AccountInternal, now_ns(), parent_index));
    parent.shm_order_index = parent_index;
    assert(book->add_order(parent));

    assert(router.route_order(parent));

    const order_entry* parent_after = book->find_order(parent_id);
    assert(parent_after != nullptr);
    assert(parent_after->request.order_state.load(std::memory_order_acquire) == OrderState::TraderError);

    const std::vector<InternalOrderId> children = book->get_children(parent_id);
    assert(children.size() == 3);

    std::size_t submitted_children = 0;
    std::size_t error_children = 0;
    for (InternalOrderId child_id : children) {
        const order_entry* child = book->find_order(child_id);
        assert(child != nullptr);
        const OrderState status = child->request.order_state.load(std::memory_order_acquire);
        if (status == OrderState::TraderSubmitted) {
            ++submitted_children;
        } else if (status == OrderState::TraderError) {
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
