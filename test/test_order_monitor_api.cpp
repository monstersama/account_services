#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api/order_api.h"
#include "api/order_monitor_api.h"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

TEST(strerror) {
    assert(std::strcmp(acct_orders_mon_strerror(ACCT_MON_OK), "Success") == 0);
    assert(std::strcmp(acct_orders_mon_strerror(ACCT_MON_ERR_NOT_FOUND), "Order index not found") == 0);
}

TEST(open_read_close) {
    (void)setenv("ACCT_TRADING_DAY", "20260225", 1);
    assert(acct_cleanup_shm() == ACCT_OK);

    acct_ctx_t order_ctx = nullptr;
    assert(acct_init(&order_ctx) == ACCT_OK);
    assert(order_ctx != nullptr);

    uint32_t order_id = 0;
    assert(acct_submit_order(order_ctx, "000001", ACCT_SIDE_BUY, ACCT_MARKET_SZ, 100, 10.5, 0, &order_id) == ACCT_OK);
    assert(order_id > 0);

    acct_orders_mon_ctx_t mon_ctx = nullptr;
    assert(acct_orders_mon_open(nullptr, &mon_ctx) == ACCT_MON_OK);
    assert(mon_ctx != nullptr);

    acct_orders_mon_info_t info{};
    assert(acct_orders_mon_info(mon_ctx, &info) == ACCT_MON_OK);
    assert(info.capacity > 0);
    assert(info.next_index >= 1);
    assert(std::strcmp(info.trading_day, "20260225") == 0);

    acct_orders_mon_snapshot_t snapshot{};
    acct_mon_error_t rc = ACCT_MON_ERR_RETRY;
    for (int i = 0; i < 16 && rc == ACCT_MON_ERR_RETRY; ++i) {
        rc = acct_orders_mon_read(mon_ctx, 0, &snapshot);
    }
    assert(rc == ACCT_MON_OK);
    assert(snapshot.index == 0);
    assert(snapshot.internal_order_id == order_id);
    assert(snapshot.stage == ACCT_MON_STAGE_UPSTREAM_QUEUED);
    assert(snapshot.volume_entrust == 100);
    assert(std::strncmp(snapshot.security_id, "000001", 6) == 0);

    assert(acct_orders_mon_close(mon_ctx) == ACCT_MON_OK);
    assert(acct_destroy(order_ctx) == ACCT_OK);
    assert(acct_cleanup_shm() == ACCT_OK);
}

TEST(read_not_found) {
    (void)setenv("ACCT_TRADING_DAY", "20260225", 1);
    assert(acct_cleanup_shm() == ACCT_OK);

    acct_ctx_t order_ctx = nullptr;
    assert(acct_init(&order_ctx) == ACCT_OK);
    assert(order_ctx != nullptr);

    acct_orders_mon_ctx_t mon_ctx = nullptr;
    assert(acct_orders_mon_open(nullptr, &mon_ctx) == ACCT_MON_OK);

    acct_orders_mon_snapshot_t snapshot{};
    assert(acct_orders_mon_read(mon_ctx, 0, &snapshot) == ACCT_MON_ERR_NOT_FOUND);

    assert(acct_orders_mon_close(mon_ctx) == ACCT_MON_OK);
    assert(acct_destroy(order_ctx) == ACCT_OK);
    assert(acct_cleanup_shm() == ACCT_OK);
}

int main() {
    printf("=== Order Monitor API Test Suite ===\n\n");

    RUN_TEST(strerror);
    RUN_TEST(open_read_close);
    RUN_TEST(read_not_found);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
