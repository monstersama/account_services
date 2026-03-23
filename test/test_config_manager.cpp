#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/error.hpp"
#include "core/config_manager.hpp"

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

std::filesystem::path repo_root() {
    namespace fs = std::filesystem;
    fs::path probe = fs::current_path();
    for (int depth = 0; depth < 8; ++depth) {
        if (fs::exists(probe / "CMakeLists.txt") && fs::exists(probe / "config" / "default.yaml")) {
            return probe;
        }
        if (!probe.has_parent_path()) {
            break;
        }
        const fs::path parent = probe.parent_path();
        if (parent == probe) {
            break;
        }
        probe = parent;
    }
    assert(false && "failed to locate repository root");
    return {};
}

void assert_yaml_map_has_keys(const YAML::Node& map, std::initializer_list<std::string_view> keys) {
    assert(map);
    assert(map.IsMap());
    for (std::string_view key : keys) {
        assert(map[std::string(key)]);
    }
}

}  // namespace

TEST(load_and_export_roundtrip) {
    const std::string in_path = unique_path("config_mgr_in", ".yaml");
    const std::string out_path = unique_path("config_mgr_out", ".yaml");

    {
        std::ofstream out(in_path);
        assert(out.is_open());
        out << "account_id: 7\n";
        out << "trading_day: \"20260225\"\n";
        out << "shm:\n";
        out << "  upstream_shm_name: \"/u_test\"\n";
        out << "  downstream_shm_name: \"/d_test\"\n";
        out << "  trades_shm_name: \"/t_test\"\n";
        out << "  orders_shm_name: \"/o_test\"\n";
        out << "  positions_shm_name: \"/p_test\"\n";
        out << "  create_if_not_exist: true\n";
        out << "event_loop:\n";
        out << "  poll_batch_size: 32\n";
        out << "  idle_sleep_us: 10\n";
        out << "market_data:\n";
        out << "  enabled: true\n";
        out << "  snapshot_shm_name: \"/mdsnap_test\"\n";
        out << "  allow_order_price_fallback: true\n";
        out << "split:\n";
        out << "  strategy: \"fixed_size\"\n";
        out << "  max_child_volume: 500\n";
    }

    ConfigManager manager;
    assert(manager.load_from_file(in_path));
    assert(manager.account_id() == 7);
    assert(manager.get().trading_day == "20260225");
    assert(manager.shm().upstream_shm_name == "/u_test");
    assert(manager.shm().trades_shm_name == "/t_test");
    assert(manager.shm().orders_shm_name == "/o_test");
    assert(manager.EventLoop().poll_batch_size == 32);
    assert(manager.market_data().enabled);
    assert(manager.market_data().allow_order_price_fallback);
    assert(manager.split().strategy == SplitStrategy::FixedSize);
    assert(manager.split().max_child_volume == 500);

    assert(manager.export_to_file(out_path));

    ConfigManager reloaded;
    assert(reloaded.load_from_file(out_path));
    assert(reloaded.account_id() == 7);
    assert(reloaded.get().trading_day == "20260225");
    assert(reloaded.shm().downstream_shm_name == "/d_test");
    assert(reloaded.shm().trades_shm_name == "/t_test");
    assert(reloaded.EventLoop().idle_sleep_us == 10);

    std::remove(in_path.c_str());
    std::remove(out_path.c_str());
}

TEST(load_rejects_unknown_key) {
    const std::string in_path = unique_path("config_mgr_unknown", ".yaml");

    {
        std::ofstream out(in_path);
        assert(out.is_open());
        out << "account_id: 7\n";
        out << "trading_day: \"20260225\"\n";
        out << "shm:\n";
        out << "  upstream_shm_name: \"/u_test\"\n";
        out << "  downstream_shm_name: \"/d_test\"\n";
        out << "  trades_shm_name: \"/t_test\"\n";
        out << "  orders_shm_name: \"/o_test\"\n";
        out << "  positions_shm_name: \"/p_test\"\n";
        out << "  create_if_not_exist: true\n";
        out << "  unknown_field: 1\n";
    }

    ConfigManager manager;
    assert(!manager.load_from_file(in_path));

    std::remove(in_path.c_str());
}

TEST(load_rejects_invalid_bool_value) {
    const std::string in_path = unique_path("config_mgr_invalid_bool", ".yaml");

    {
        std::ofstream out(in_path);
        assert(out.is_open());
        out << "shm:\n";
        out << "  create_if_not_exist: maybe\n";
    }

    clear_last_error();
    ConfigManager manager;
    assert(!manager.load_from_file(in_path));
    assert(last_error().code == ErrorCode::ConfigParseFailed);
    assert(std::string(last_error().message.view()).find("invalid boolean value") != std::string::npos);

    std::remove(in_path.c_str());
}

TEST(load_rejects_invalid_split_strategy) {
    const std::string in_path = unique_path("config_mgr_invalid_split", ".yaml");

    {
        std::ofstream out(in_path);
        assert(out.is_open());
        out << "split:\n";
        out << "  strategy: \"not_a_strategy\"\n";
    }

    clear_last_error();
    ConfigManager manager;
    assert(!manager.load_from_file(in_path));
    assert(last_error().code == ErrorCode::ConfigParseFailed);
    assert(std::string(last_error().message.view()).find("invalid split strategy") != std::string::npos);

    std::remove(in_path.c_str());
}

TEST(to_log_string_includes_all_sections) {
    const std::string in_path = unique_path("config_mgr_to_yaml", ".yaml");

    {
        std::ofstream out(in_path);
        assert(out.is_open());
        out << "account_id: 19\n";
        out << "trading_day: \"20260301\"\n";
        out << "shm:\n";
        out << "  upstream_shm_name: \"/yaml_upstream\"\n";
        out << "  downstream_shm_name: \"/yaml_downstream\"\n";
        out << "  trades_shm_name: \"/yaml_trades\"\n";
        out << "  orders_shm_name: \"/yaml_orders\"\n";
        out << "  positions_shm_name: \"/yaml_positions\"\n";
        out << "  create_if_not_exist: false\n";
        out << "event_loop:\n";
        out << "  busy_polling: false\n";
        out << "  poll_batch_size: 11\n";
        out << "  idle_sleep_us: 7\n";
        out << "  stats_interval_ms: 99\n";
        out << "  archive_terminal_orders: true\n";
        out << "  terminal_archive_delay_ms: 1234\n";
        out << "  pin_cpu: true\n";
        out << "  cpu_core: 2\n";
        out << "market_data:\n";
        out << "  enabled: true\n";
        out << "  snapshot_shm_name: \"/yaml_snapshot\"\n";
        out << "  allow_order_price_fallback: true\n";
        out << "active_strategy:\n";
        out << "  enabled: true\n";
        out << "  name: \"mean_revert\"\n";
        out << "  signal_threshold: 1.25\n";
        out << "risk:\n";
        out << "  max_order_value: 1001\n";
        out << "  max_order_volume: 1002\n";
        out << "  max_daily_turnover: 1003\n";
        out << "  max_orders_per_second: 1004\n";
        out << "  enable_price_limit_check: false\n";
        out << "  enable_duplicate_check: false\n";
        out << "  enable_fund_check: false\n";
        out << "  enable_position_check: false\n";
        out << "  duplicate_window_ns: 1005\n";
        out << "split:\n";
        out << "  strategy: \"twap\"\n";
        out << "  max_child_volume: 2001\n";
        out << "  min_child_volume: 2002\n";
        out << "  max_child_count: 2\n";
        out << "  interval_ms: 2003\n";
        out << "  randomize_factor: 0.5\n";
        out << "log:\n";
        out << "  log_dir: \"/tmp/config_mgr_logs\"\n";
        out << "  log_level: \"debug\"\n";
        out << "  async_logging: false\n";
        out << "  async_queue_size: 256\n";
        out << "business_log:\n";
        out << "  enabled: true\n";
        out << "  output_dir: \"/tmp/business_logs\"\n";
        out << "  queue_capacity: 1024\n";
        out << "  flush_interval_ms: 25\n";
        out << "db:\n";
        out << "  db_path: \"/tmp/config_mgr.sqlite\"\n";
        out << "  enable_persistence: false\n";
        out << "  sync_interval_ms: 600\n";
    }

    ConfigManager manager;
    assert(manager.load_from_file(in_path));

    const std::string log_text = manager.to_log_string();
    assert(log_text.find("[config] [meta] config_file=" + in_path) != std::string::npos);
    assert(log_text.find("[config] [root] account_id=19") != std::string::npos);
    assert(log_text.find("[config] [root] trading_day=20260301") != std::string::npos);
    assert(log_text.find("[config] [shm] upstream_shm_name=/yaml_upstream") != std::string::npos);
    assert(log_text.find("[config] [event_loop] archive_terminal_orders=true") != std::string::npos);
    assert(log_text.find("[config] [market_data] snapshot_shm_name=/yaml_snapshot") != std::string::npos);
    assert(log_text.find("[config] [market_data] allow_order_price_fallback=true") != std::string::npos);
    assert(log_text.find("[config] [active_strategy] name=mean_revert") != std::string::npos);
    assert(log_text.find("[config] [risk] duplicate_window_ns=1005") != std::string::npos);
    assert(log_text.find("[config] [split] strategy=twap") != std::string::npos);
    assert(log_text.find("[config] [log] log_level=debug") != std::string::npos);
    assert(log_text.find("[config] [business_log] output_dir=/tmp/business_logs") != std::string::npos);
    assert(log_text.find("[config] [db] db_path=/tmp/config_mgr.sqlite") != std::string::npos);

    std::remove(in_path.c_str());
}

TEST(bundled_account_configs_cover_all_keys) {
    const std::filesystem::path config_dir = repo_root() / "config";

    for (const char* file_name : {"default.yaml", "acct.dev.yaml"}) {
        const YAML::Node root = YAML::LoadFile((config_dir / file_name).string());
        assert_yaml_map_has_keys(
            root, {"account_id", "trading_day", "shm", "market_data", "active_strategy", "risk", "split", "log", "db"});

        const YAML::Node event_loop = root["event_loop"] ? root["event_loop"] : root["EventLoop"];
        assert_yaml_map_has_keys(root["shm"], {"upstream_shm_name", "downstream_shm_name", "trades_shm_name",
                                               "orders_shm_name", "positions_shm_name", "create_if_not_exist"});
        assert_yaml_map_has_keys(event_loop,
                                 {"busy_polling", "poll_batch_size", "idle_sleep_us", "stats_interval_ms",
                                  "archive_terminal_orders", "terminal_archive_delay_ms", "pin_cpu", "cpu_core"});
        assert_yaml_map_has_keys(root["market_data"], {"enabled", "snapshot_shm_name", "allow_order_price_fallback"});
        assert_yaml_map_has_keys(root["active_strategy"], {"enabled", "name", "signal_threshold"});
        assert_yaml_map_has_keys(root["risk"],
                                 {"max_order_value", "max_order_volume", "max_daily_turnover", "max_orders_per_second",
                                  "enable_price_limit_check", "enable_duplicate_check", "enable_fund_check",
                                  "enable_position_check", "duplicate_window_ns"});
        assert_yaml_map_has_keys(root["split"], {"strategy", "max_child_volume", "min_child_volume", "max_child_count",
                                                 "interval_ms", "randomize_factor"});
        assert_yaml_map_has_keys(root["log"], {"log_dir", "log_level", "async_logging", "async_queue_size"});
        assert_yaml_map_has_keys(root["business_log"],
                                 {"enabled", "output_dir", "queue_capacity", "flush_interval_ms"});
        assert_yaml_map_has_keys(root["db"], {"db_path", "enable_persistence", "sync_interval_ms"});
    }
}

TEST(parse_command_line_and_validate) {
    ConfigManager manager;

    char arg0[] = "test";
    char arg1[] = "--account-id";
    char arg2[] = "9";
    char arg3[] = "--poll-batch";
    char arg4[] = "128";
    char arg5[] = "--split-strategy";
    char arg6[] = "none";
    char arg7[] = "--trades-shm";
    char arg8[] = "/trades_cli";
    char arg9[] = "--orders-shm";
    char arg10[] = "/orders_cli";
    char arg11[] = "--trading-day";
    char arg12[] = "20260225";

    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12};
    assert(manager.parse_command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv));

    assert(manager.account_id() == 9);
    assert(manager.EventLoop().poll_batch_size == 128);
    assert(manager.split().strategy == SplitStrategy::None);
    assert(manager.shm().trades_shm_name == "/trades_cli");
    assert(manager.shm().orders_shm_name == "/orders_cli");
    assert(manager.get().trading_day == "20260225");
    assert(manager.validate());

    manager.get().account_id = 0;
    assert(!manager.validate());
}

int main() {
    printf("=== Config Manager Test Suite ===\n\n");

    RUN_TEST(load_and_export_roundtrip);
    RUN_TEST(load_rejects_unknown_key);
    RUN_TEST(load_rejects_invalid_bool_value);
    RUN_TEST(load_rejects_invalid_split_strategy);
    RUN_TEST(to_log_string_includes_all_sections);
    RUN_TEST(bundled_account_configs_cover_all_keys);
    RUN_TEST(parse_command_line_and_validate);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
