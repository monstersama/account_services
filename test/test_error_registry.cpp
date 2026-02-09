#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

#include "common/error.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

using namespace acct_service;

TEST(record_and_count) {
    global_error_registry().reset();
    clear_last_error();

    const error_status first = ACCT_MAKE_ERROR(error_domain::core, error_code::InternalError, "test", "internal", 0);
    record_error(first);

    assert(global_error_registry().count(error_code::InternalError) == 1);
    assert(!last_error().ok());
    assert(last_error().code == error_code::InternalError);
}

TEST(concurrent_recording) {
    global_error_registry().reset();
    clear_last_error();

    constexpr int kThreads = 4;
    constexpr int kPerThread = 256;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([]() {
            for (int j = 0; j < kPerThread; ++j) {
                record_error(ACCT_MAKE_ERROR(
                    error_domain::order, error_code::QueuePushFailed, "test", "queue push failed", 0));
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    assert(global_error_registry().count(error_code::QueuePushFailed) == static_cast<uint64_t>(kThreads * kPerThread));
    assert(!global_error_registry().recent_errors().empty());
}

int main() {
    printf("=== Error Registry Test Suite ===\n\n");

    RUN_TEST(record_and_count);
    RUN_TEST(concurrent_recording);

    printf("\n=== All tests passed! ===\n");
    return 0;
}

