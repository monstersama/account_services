#include "api/order_api.h"

#include <cassert>
#include <cstdio>
#include <cstring>

#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Running %s... ", #name); \
    test_##name(); \
    printf("PASSED\n"); \
} while(0)

TEST(version) {
    const char* ver = acct_version();
    assert(ver != nullptr);
    assert(strlen(ver) > 0);
    printf("(version=%s) ", ver);
}

TEST(strerror) {
    assert(strcmp(acct_strerror(ACCT_OK), "Success") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_NOT_INITIALIZED), "Context not initialized") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_INVALID_PARAM), "Invalid parameter") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_QUEUE_FULL), "Queue is full") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_SHM_FAILED), "Shared memory operation failed") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_ORDER_NOT_FOUND), "Order not found") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_CACHE_FULL), "Order cache is full") == 0);
    assert(strcmp(acct_strerror(ACCT_ERR_INTERNAL), "Internal error") == 0);
}

TEST(null_ctx_operations) {
    // 所有操作对 NULL 上下文应该返回错误码
    uint32_t order_id = 0;
    acct_error_t err = acct_new_order(
        nullptr, "000001", ACCT_SIDE_BUY, ACCT_MARKET_SZ,
        100, 10.5, 93000000, &order_id
    );
    assert(err == ACCT_ERR_INVALID_PARAM);
    assert(order_id == 0);

    err = acct_send_order(nullptr, 1);
    assert(err == ACCT_ERR_INVALID_PARAM);

    err = acct_submit_order(
        nullptr, "000001", ACCT_SIDE_BUY, ACCT_MARKET_SZ,
        100, 10.5, 93000000, &order_id
    );
    assert(err == ACCT_ERR_INVALID_PARAM);
    assert(order_id == 0);

    err = acct_cancel_order(nullptr, 1, 93000000, &order_id);
    assert(err == ACCT_ERR_INVALID_PARAM);
    assert(order_id == 0);

    size_t queue_size = 0;
    err = acct_queue_size(nullptr, &queue_size);
    assert(err == ACCT_ERR_INVALID_PARAM);

    // destroy NULL 应该返回错误码
    err = acct_destroy(nullptr);
    assert(err == ACCT_ERR_INVALID_PARAM);
}

TEST(init_with_auto_create) {
    // 共享内存会自动创建
    acct_ctx_t ctx = nullptr;
    acct_error_t err = acct_init(&ctx);
    assert(err == ACCT_OK);
    assert(ctx != nullptr);

    // 销毁上下文
    err = acct_destroy(ctx);
    assert(err == ACCT_OK);

    // 清理共享内存
    err = acct_cleanup_shm();
    assert(err == ACCT_OK);
}

TEST(invalid_params) {
    // 注意: ctx 为 nullptr 时会返回 INVALID_PARAM
    // 实际参数验证（如 side、market、volume）需要有效的 ctx
}

int main() {
    printf("=== Order API Test Suite ===\n\n");

    RUN_TEST(version);
    RUN_TEST(strerror);
    RUN_TEST(null_ctx_operations);
    RUN_TEST(init_with_auto_create);
    RUN_TEST(invalid_params);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
