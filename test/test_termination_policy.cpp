#include <cassert>
#include <cstdio>

#include "common/error.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

using namespace acct_service;

TEST(fatal_requests_shutdown) {
    clear_shutdown_reason();

    const error_status status =
        ACCT_MAKE_ERROR(error_domain::portfolio, error_code::PositionUpdateFailed, "test", "fatal update failure", 0);
    record_error(status);

    assert(shutdown_reason() == error_severity::Fatal);
    assert(should_stop_service());
    assert(should_exit_process());
}

TEST(critical_requests_shutdown) {
    clear_shutdown_reason();

    const error_status status =
        ACCT_MAKE_ERROR(error_domain::config, error_code::ConfigValidateFailed, "test", "critical config failure", 0);
    record_error(status);

    assert(shutdown_reason() == error_severity::Critical);
    assert(should_stop_service());
}

TEST(recoverable_keeps_running) {
    clear_shutdown_reason();

    const error_status status =
        ACCT_MAKE_ERROR(error_domain::order, error_code::QueuePushFailed, "test", "recoverable queue full", 0);
    record_error(status);

    assert(shutdown_reason() == error_severity::Recoverable);
    assert(!should_stop_service());
}

TEST(domain_matrix_applies_for_api) {
    clear_shutdown_reason();

    const error_policy& core_policy = classify(error_domain::core, error_code::InvalidState);
    assert(core_policy.severity == error_severity::Critical);
    assert(core_policy.stop_service);

    const error_policy& api_policy = classify(error_domain::api, error_code::InvalidState);
    assert(api_policy.severity == error_severity::Critical);
    assert(!api_policy.stop_service);
    assert(!api_policy.exit_process);

    const error_status status =
        ACCT_MAKE_ERROR(error_domain::api, error_code::InvalidState, "test", "api invalid state", 0);
    record_error(status);

    // API 域不应触发主进程停服闩锁。
    assert(shutdown_reason() == error_severity::Recoverable);
    assert(!should_stop_service());
    assert(!should_exit_process());
}

int main() {
    printf("=== Termination Policy Test Suite ===\n\n");

    RUN_TEST(fatal_requests_shutdown);
    RUN_TEST(critical_requests_shutdown);
    RUN_TEST(recoverable_keeps_running);
    RUN_TEST(domain_matrix_applies_for_api);

    printf("\n=== All tests passed! ===\n");
    return 0;
}

