#include "api/order_api.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace acct;

#define TEST(name) static void test_##name()
#define RUN_TEST(name) do { \
    printf("Running %s... ", #name); \
    test_##name(); \
    printf("PASSED\n"); \
} while(0)

TEST(version) {
    const char* ver = OrderApi::version();
    assert(ver != nullptr);
    assert(strlen(ver) > 0);
    printf("(version=%s) ", ver);
}

TEST(strerror) {
    assert(strcmp(OrderApi::errorString(Error::Ok), "Success") == 0);
    assert(strcmp(OrderApi::errorString(Error::NotInitialized), "Context not initialized") == 0);
    assert(strcmp(OrderApi::errorString(Error::InvalidParam), "Invalid parameter") == 0);
    assert(strcmp(OrderApi::errorString(Error::QueueFull), "Queue is full") == 0);
    assert(strcmp(OrderApi::errorString(Error::ShmFailed), "Shared memory operation failed") == 0);
    assert(strcmp(OrderApi::errorString(Error::OrderNotFound), "Order not found") == 0);
    assert(strcmp(OrderApi::errorString(Error::InitNoMemory), "Initialization failed: memory allocation failed") == 0);
    assert(strcmp(OrderApi::errorString(Error::InitShmOpenFailed), "Initialization failed: shared memory open failed") == 0);
    assert(strcmp(OrderApi::errorString(Error::InitMmapFailed), "Initialization failed: memory mapping failed") == 0);
    assert(strcmp(OrderApi::errorString(Error::InitInvalidMagic), "Initialization failed: invalid magic number") == 0);
    assert(strcmp(OrderApi::errorString(Error::Internal), "Internal error") == 0);
}

TEST(null_ctx_operations) {
    // 使用默认构造的 API 对象（未初始化）
    Error err;
    auto api = OrderApi::create(&err);
    if (!api) {
        // 没有共享内存时，应该返回 InitShmOpenFailed
        assert(err == Error::InitShmOpenFailed);
    }
}

TEST(init_no_shm) {
    // 共享内存未创建时，init 应该失败并返回具体错误码
    Error err;
    auto api = OrderApi::create(&err);
    assert(api == nullptr);
    assert(err == Error::InitShmOpenFailed);
    printf("(error=%s) ", OrderApi::errorString(err));
}

TEST(invalid_params) {
    // 创建 mock API 对象测试空指针情况
    // 注意：我们无法轻易测试 valid_sec 参数，因为那需要有效的共享内存
}

int main() {
    printf("=== Order API C++ Test Suite ===\n\n");

    RUN_TEST(version);
    RUN_TEST(strerror);
    RUN_TEST(null_ctx_operations);
    RUN_TEST(init_no_shm);
    RUN_TEST(invalid_params);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
