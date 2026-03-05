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

    const ErrorStatus status =
        ACCT_MAKE_ERROR(error_domain::portfolio, ErrorCode::PositionUpdateFailed, "test", "fatal update failure", 0);
    record_error(status);

    assert(shutdown_reason() == ErrorSeverity::Fatal);
    assert(should_stop_service());
    assert(should_exit_process());
}

TEST(critical_requests_shutdown) {
    clear_shutdown_reason();

    const ErrorStatus status =
        ACCT_MAKE_ERROR(error_domain::config, ErrorCode::ConfigValidateFailed, "test", "critical config failure", 0);
    record_error(status);

    assert(shutdown_reason() == ErrorSeverity::Critical);
    assert(should_stop_service());
}

TEST(recoverable_keeps_running) {
    clear_shutdown_reason();

    const ErrorStatus status =
        ACCT_MAKE_ERROR(error_domain::order, ErrorCode::QueuePushFailed, "test", "recoverable queue full", 0);
    record_error(status);

    assert(shutdown_reason() == ErrorSeverity::Recoverable);
    assert(!should_stop_service());
}

TEST(domain_matrix_applies_for_api) {
    clear_shutdown_reason();

    const ErrorPolicy& core_policy = classify(error_domain::core, ErrorCode::InvalidState);
    assert(core_policy.severity == ErrorSeverity::Critical);
    assert(core_policy.stop_service);

    const ErrorPolicy& api_policy = classify(error_domain::api, ErrorCode::InvalidState);
    assert(api_policy.severity == ErrorSeverity::Critical);
    assert(!api_policy.stop_service);
    assert(!api_policy.exit_process);

    const ErrorStatus status =
        ACCT_MAKE_ERROR(error_domain::api, ErrorCode::InvalidState, "test", "api invalid state", 0);
    record_error(status);

    // API 域不应触发主进程停服闩锁。
    assert(shutdown_reason() == ErrorSeverity::Recoverable);
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

