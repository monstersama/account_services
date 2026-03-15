#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <vector>

#include "order/order_router.hpp"
#include "shm/orders_shm.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        printf("Running %s... ", #name);                                                                               \
        test_##name();                                                                                                 \
        printf("PASSED\n");                                                                                            \
    } while (0)

namespace {

using namespace acct_service;

OrderEntry make_parent_entry(InternalOrderId order_id, Volume volume) {
    OrderEntry entry{};
    entry.request.init_new(
        "000001", InternalSecurityId("XSHE_000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000, 93000000);
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

// 构造一个已登记父单的内部子单，供路由器验证撤单扇出语义。
OrderEntry make_child_entry(InternalOrderId order_id, InternalOrderId parent_id, Volume volume) {
    OrderEntry entry{};
    entry.request.init_new(
        "000001", InternalSecurityId("XSHE_000001"), order_id, TradeSide::Buy, Market::SZ, volume, 1000, 93000000);
    entry.request.order_state.store(OrderState::RiskControllerAccepted, std::memory_order_relaxed);
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<StrategyId>(1);
    entry.risk_result = RiskResult::Pass;
    entry.retry_count = 0;
    entry.is_split_child = true;
    entry.parent_order_id = parent_id;
    return entry;
}

} // namespace

TEST(internal_child_cancel_fanout_tracks_parent) {
    auto book = std::make_unique<OrderBook>();
    auto downstream = make_downstream();
    auto orders = make_orders();
    order_router router(*book, downstream.get(), orders.get());

    const InternalOrderId parent_id = book->next_order_id();
    OrderEntry parent = make_parent_entry(parent_id, 250);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(),
        parent.request,
        OrderSlotState::UpstreamDequeued,
        order_slot_source_t::AccountInternal,
        now_ns(),
        parent_index));
    parent.shm_order_index = parent_index;
    assert(book->add_order(parent));

    const InternalOrderId child1_id = book->next_order_id();
    const InternalOrderId child2_id = book->next_order_id();
    const InternalOrderId child3_id = book->next_order_id();
    OrderEntry child1 = make_child_entry(child1_id, parent_id, 100);
    OrderEntry child2 = make_child_entry(child2_id, parent_id, 80);
    OrderEntry child3 = make_child_entry(child3_id, parent_id, 70);

    assert(router.submit_internal_order(child1));
    assert(router.submit_internal_order(child2));
    assert(router.submit_internal_order(child3));

    const std::vector<InternalOrderId> children = book->get_children(parent_id);
    assert(children.size() == 3);

    const InternalOrderId parent_cancel_id = book->next_order_id();
    assert(router.route_cancel(parent_id, parent_cancel_id, 93100000));

    std::set<InternalOrderId> cancelled_child_ids;
    std::size_t cancel_count = 0;
    OrderIndex index = kInvalidOrderIndex;
    while (downstream->order_queue.try_pop(index)) {
        order_slot_snapshot snapshot;
        assert(orders_shm_read_snapshot(orders.get(), index, snapshot));
        const OrderRequest& request = snapshot.request;
        if (request.order_type != OrderType::Cancel) {
            continue;
        }
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

TEST(cancel_send_failure_latches_parent_error) {
    auto book = std::make_unique<OrderBook>();
    auto downstream = make_downstream();
    auto orders = make_orders();
    order_router router(*book, downstream.get(), orders.get());

    const std::size_t capacity = decltype(downstream->order_queue)::capacity();
    for (std::size_t i = 0; i < capacity - 1; ++i) {
        OrderRequest dummy;
        dummy.init_new("000001", InternalSecurityId("XSHE_000001"), static_cast<InternalOrderId>(1000 + i),
                       TradeSide::Buy, Market::SZ, 1, 1000, 93000000);
        dummy.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);
        OrderIndex dummy_index = kInvalidOrderIndex;
        assert(orders_shm_append(orders.get(),
            dummy,
            OrderSlotState::DownstreamQueued,
            order_slot_source_t::AccountInternal,
            now_ns(),
            dummy_index));
        assert(downstream->order_queue.try_push(dummy_index));
    }

    const InternalOrderId parent_id = book->next_order_id();
    OrderEntry parent = make_parent_entry(parent_id, 300);
    OrderIndex parent_index = kInvalidOrderIndex;
    assert(orders_shm_append(orders.get(),
        parent.request,
        OrderSlotState::UpstreamDequeued,
        order_slot_source_t::AccountInternal,
        now_ns(),
        parent_index));
    parent.shm_order_index = parent_index;
    assert(book->add_order(parent));

    const InternalOrderId child1_id = book->next_order_id();
    const InternalOrderId child2_id = book->next_order_id();
    const InternalOrderId child3_id = book->next_order_id();
    OrderEntry child1 = make_child_entry(child1_id, parent_id, 100);
    OrderEntry child2 = make_child_entry(child2_id, parent_id, 100);
    OrderEntry child3 = make_child_entry(child3_id, parent_id, 100);

    assert(book->add_order(child1));
    assert(book->add_order(child2));
    assert(book->add_order(child3));

    assert(router.route_cancel(parent_id, book->next_order_id(), 93100000));

    const OrderEntry* parent_after = book->find_order(parent_id);
    assert(parent_after != nullptr);
    assert(parent_after->request.order_state.load(std::memory_order_acquire) == OrderState::TraderError);

    const std::vector<InternalOrderId> children = book->get_children(parent_id);
    assert(children.size() >= 3);

    std::size_t submitted_children = 0;
    std::size_t error_children = 0;
    for (InternalOrderId child_id : children) {
        const OrderEntry* child = book->find_order(child_id);
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

    RUN_TEST(internal_child_cancel_fanout_tracks_parent);
    RUN_TEST(cancel_send_failure_latches_parent_error);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
