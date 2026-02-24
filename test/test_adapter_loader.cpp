#include <cassert>
#include <cstdio>
#include <string>

#include "adapter_loader.hpp"
#include "broker_api/broker_api.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

#ifndef TEST_ADAPTER_PLUGIN_PATH
#error "TEST_ADAPTER_PLUGIN_PATH is not defined"
#endif

std::string plugin_path() {
    return TEST_ADAPTER_PLUGIN_PATH;
}

TEST(load_plugin_success) {
    gateway::loaded_adapter loaded;
    std::string error_message;

    assert(gateway::load_adapter_plugin(plugin_path(), loaded, error_message));
    assert(loaded.valid());
    assert(loaded.get() != nullptr);

    broker_api::broker_runtime_config cfg;
    cfg.account_id = 1;
    cfg.auto_fill = true;
    assert(loaded.get()->initialize(cfg));
    loaded.get()->shutdown();
}

TEST(load_plugin_missing_file) {
    gateway::loaded_adapter loaded;
    std::string error_message;

    assert(!gateway::load_adapter_plugin("/tmp/not_exists_adapter.so", loaded, error_message));
    assert(!error_message.empty());
    assert(!loaded.valid());
}

}  // namespace

int main() {
    printf("=== Adapter Loader Test Suite ===\n\n");

    RUN_TEST(load_plugin_success);
    RUN_TEST(load_plugin_missing_file);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
