#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

#include <unistd.h>

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
        out << "split:\n";
        out << "  strategy: \"fixed_size\"\n";
        out << "  max_child_volume: 500\n";
    }

    config_manager manager;
    assert(manager.load_from_file(in_path));
    assert(manager.account_id() == 7);
    assert(manager.get().trading_day == "20260225");
    assert(manager.shm().upstream_shm_name == "/u_test");
    assert(manager.shm().trades_shm_name == "/t_test");
    assert(manager.shm().orders_shm_name == "/o_test");
    assert(manager.event_loop().poll_batch_size == 32);
    assert(manager.split().strategy == split_strategy_t::FixedSize);
    assert(manager.split().max_child_volume == 500);

    assert(manager.export_to_file(out_path));

    config_manager reloaded;
    assert(reloaded.load_from_file(out_path));
    assert(reloaded.account_id() == 7);
    assert(reloaded.get().trading_day == "20260225");
    assert(reloaded.shm().downstream_shm_name == "/d_test");
    assert(reloaded.shm().trades_shm_name == "/t_test");
    assert(reloaded.event_loop().idle_sleep_us == 10);

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

    config_manager manager;
    assert(!manager.load_from_file(in_path));

    std::remove(in_path.c_str());
}

TEST(parse_command_line_and_validate) {
    config_manager manager;

    char arg0[] = "test";
    char arg1[] = "--account-id";
    char arg2[] = "9";
    char arg3[] = "--poll-batch";
    char arg4[] = "128";
    char arg5[] = "--split-strategy";
    char arg6[] = "iceberg";
    char arg7[] = "--trades-shm";
    char arg8[] = "/trades_cli";
    char arg9[] = "--orders-shm";
    char arg10[] = "/orders_cli";
    char arg11[] = "--trading-day";
    char arg12[] = "20260225";

    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9, arg10, arg11, arg12};
    assert(manager.parse_command_line(static_cast<int>(sizeof(argv) / sizeof(argv[0])), argv));

    assert(manager.account_id() == 9);
    assert(manager.event_loop().poll_batch_size == 128);
    assert(manager.split().strategy == split_strategy_t::Iceberg);
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
    RUN_TEST(parse_command_line_and_validate);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
