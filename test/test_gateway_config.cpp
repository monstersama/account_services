#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "gateway_config.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

std::string unique_path(const char* stem, const char* ext) {
    return std::string("/tmp/") + stem + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(now_ns())) + ext;
}

gateway::parse_result_t parse_gateway_args(
    const std::vector<std::string>& args_in, gateway::gateway_config& out_config, std::string& out_error) {
    std::vector<std::string> args = args_in;
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    return gateway::parse_args(static_cast<int>(argv.size()), argv.data(), out_config, out_error);
}

}  // namespace

TEST(load_from_yaml_file) {
    const std::string path = unique_path("gateway_cfg", ".yaml");
    {
        std::ofstream out(path);
        assert(out.is_open());
        out << "account_id: 7\n";
        out << "downstream_shm: \"/downstream_test\"\n";
        out << "trades_shm: \"/trades_test\"\n";
        out << "orders_shm: \"/orders_test\"\n";
        out << "trading_day: \"20260226\"\n";
        out << "broker_type: \"plugin\"\n";
        out << "adapter_so: \"/tmp/adapter.so\"\n";
        out << "create_if_not_exist: true\n";
        out << "poll_batch_size: 32\n";
        out << "idle_sleep_us: 10\n";
        out << "stats_interval_ms: 200\n";
        out << "max_retries: 8\n";
        out << "retry_interval_us: 900\n";
    }

    gateway::gateway_config config;
    std::string error;
    const gateway::parse_result_t result = parse_gateway_args({"test_gateway", "--config", path}, config, error);
    assert(result == gateway::parse_result_t::Ok);
    assert(error.empty());
    assert(config.config_file == path);
    assert(config.account_id == 7);
    assert(config.downstream_shm_name == "/downstream_test");
    assert(config.trades_shm_name == "/trades_test");
    assert(config.orders_shm_name == "/orders_test");
    assert(config.trading_day == "20260226");
    assert(config.broker_type == "plugin");
    assert(config.adapter_plugin_so == "/tmp/adapter.so");
    assert(config.create_if_not_exist);
    assert(config.poll_batch_size == 32);
    assert(config.idle_sleep_us == 10);
    assert(config.stats_interval_ms == 200);
    assert(config.max_retry_attempts == 8);
    assert(config.retry_interval_us == 900);

    std::remove(path.c_str());
}

TEST(load_from_positional_path) {
    const std::string path = unique_path("gateway_cfg_override", ".yaml");
    {
        std::ofstream out(path);
        assert(out.is_open());
        out << "account_id: 7\n";
        out << "poll_batch_size: 32\n";
        out << "create_if_not_exist: false\n";
        out << "downstream_shm: \"/downstream_yaml\"\n";
        out << "trades_shm: \"/trades_yaml\"\n";
        out << "orders_shm: \"/orders_yaml\"\n";
        out << "trading_day: \"20260226\"\n";
        out << "broker_type: \"sim\"\n";
    }

    gateway::gateway_config config;
    std::string error;
    const gateway::parse_result_t result = parse_gateway_args({"test_gateway", path}, config, error);
    assert(result == gateway::parse_result_t::Ok);
    assert(error.empty());
    assert(config.config_file == path);
    assert(config.account_id == 7);
    assert(config.poll_batch_size == 32);
    assert(!config.create_if_not_exist);
    assert(config.downstream_shm_name == "/downstream_yaml");
    assert(config.trades_shm_name == "/trades_yaml");
    assert(config.orders_shm_name == "/orders_yaml");

    std::remove(path.c_str());
}

TEST(reject_legacy_command_line_options) {
    const std::string path = unique_path("gateway_cfg_legacy_opt", ".yaml");
    {
        std::ofstream out(path);
        assert(out.is_open());
        out << "account_id: 7\n";
        out << "downstream_shm: \"/downstream_test\"\n";
        out << "trades_shm: \"/trades_test\"\n";
        out << "orders_shm: \"/orders_test\"\n";
        out << "trading_day: \"20260226\"\n";
        out << "broker_type: \"sim\"\n";
    }

    gateway::gateway_config config;
    std::string error;
    const gateway::parse_result_t result =
        parse_gateway_args({"test_gateway", "--config", path, "--account-id", "9"}, config, error);
    assert(result == gateway::parse_result_t::Error);
    assert(!error.empty());

    std::remove(path.c_str());
}

TEST(reject_unknown_yaml_key) {
    const std::string path = unique_path("gateway_cfg_unknown", ".yaml");
    {
        std::ofstream out(path);
        assert(out.is_open());
        out << "account_id: 7\n";
        out << "downstream_shm: \"/downstream_test\"\n";
        out << "trades_shm: \"/trades_test\"\n";
        out << "orders_shm: \"/orders_test\"\n";
        out << "trading_day: \"20260226\"\n";
        out << "broker_type: \"sim\"\n";
        out << "unknown_field: 1\n";
    }

    gateway::gateway_config config;
    std::string error;
    const gateway::parse_result_t result = parse_gateway_args({"test_gateway", "--config", path}, config, error);
    assert(result == gateway::parse_result_t::Error);
    assert(!error.empty());

    std::remove(path.c_str());
}

int main() {
    printf("=== Gateway Config Test Suite ===\n\n");

    RUN_TEST(load_from_yaml_file);
    RUN_TEST(load_from_positional_path);
    RUN_TEST(reject_legacy_command_line_options);
    RUN_TEST(reject_unknown_yaml_key);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
