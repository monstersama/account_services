#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "full_chain_observer_config.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

// 在作用域内切换当前目录，析构时自动恢复。
class scoped_current_path {
public:
    explicit scoped_current_path(const std::filesystem::path& target)
        : previous_path_(std::filesystem::current_path()) {
        std::filesystem::current_path(target);
    }

    ~scoped_current_path() noexcept { std::filesystem::current_path(previous_path_); }

    scoped_current_path(const scoped_current_path&) = delete;
    scoped_current_path& operator=(const scoped_current_path&) = delete;

private:
    std::filesystem::path previous_path_;
};

// 生成 /tmp 下唯一文件路径，避免并发测试冲突。
std::filesystem::path unique_path(const char* stem) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return std::filesystem::path("/tmp") /
           (std::string(stem) + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
               std::to_string(static_cast<unsigned long long>(now_ns)));
}

// 将 YAML 内容写入测试文件。
void write_file(const std::filesystem::path& file_path, const std::string& content) {
    std::ofstream out(file_path);
    assert(out.is_open());
    out << content;
}

// 统一封装参数向量构造，便于复用 parse 断言逻辑。
observer_parse_result parse_observer_args(
    const std::vector<std::string>& args_in, full_chain_observer_config* out_config, std::string* out_error) {
    std::vector<std::string> args = args_in;
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    return parse_args(static_cast<int>(argv.size()), argv.data(), out_config, out_error);
}

}  // namespace

TEST(load_from_config_flag) {
    const std::filesystem::path config_path = unique_path("observer_cfg_flag");
    write_file(config_path,
        "orders_shm_name: \"/orders_test\"\n"
        "trading_day: \"20260226\"\n"
        "positions_shm_name: \"/positions_test\"\n"
        "output_dir: \"/tmp/observer_out\"\n"
        "poll_interval_ms: 123\n"
        "timeout_ms: 456\n");

    full_chain_observer_config config;
    std::string error;
    const observer_parse_result result =
        parse_observer_args({"observer", "--config", config_path.string()}, &config, &error);
    (void)result;
    assert(result == observer_parse_result::Ok);
    assert(error.empty());
    assert(config.config_file == config_path.string());
    assert(config.orders_shm_name == "/orders_test");
    assert(config.trading_day == "20260226");
    assert(config.positions_shm_name == "/positions_test");
    assert(config.output_dir == "/tmp/observer_out");
    assert(config.poll_interval_ms == 123);
    assert(config.timeout_ms == 456);

    std::filesystem::remove(config_path);
}

TEST(load_from_positional_path) {
    const std::filesystem::path config_path = unique_path("observer_cfg_pos");
    write_file(config_path,
        "orders_shm_name: \"/orders_pos\"\n"
        "trading_day: \"20260227\"\n"
        "positions_shm_name: \"/positions_pos\"\n"
        "output_dir: \"/tmp/observer_pos\"\n");

    full_chain_observer_config config;
    std::string error;
    const observer_parse_result result = parse_observer_args({"observer", config_path.string()}, &config, &error);
    (void)result;
    assert(result == observer_parse_result::Ok);
    assert(error.empty());
    assert(config.config_file == config_path.string());
    assert(config.orders_shm_name == "/orders_pos");
    assert(config.trading_day == "20260227");
    assert(config.positions_shm_name == "/positions_pos");
    assert(config.output_dir == "/tmp/observer_pos");

    std::filesystem::remove(config_path);
}

TEST(load_from_default_config_path) {
    const std::filesystem::path temp_dir = unique_path("observer_default_dir");
    const std::filesystem::path default_config_dir = temp_dir / "config";
    const std::filesystem::path default_config_path = default_config_dir / "observer.yaml";
    std::filesystem::create_directories(default_config_dir);
    write_file(default_config_path,
        "orders_shm_name: \"/orders_default\"\n"
        "trading_day: \"20260228\"\n"
        "positions_shm_name: \"/positions_default\"\n"
        "output_dir: \"/tmp/observer_default\"\n");

    {
        scoped_current_path cwd_guard(temp_dir);

        full_chain_observer_config config;
        std::string error;
        const observer_parse_result result = parse_observer_args({"observer"}, &config, &error);
        (void)result;
        assert(result == observer_parse_result::Ok);
        assert(error.empty());
        assert(config.config_file == "config/observer.yaml");
        assert(config.orders_shm_name == "/orders_default");
        assert(config.trading_day == "20260228");
        assert(config.positions_shm_name == "/positions_default");
        assert(config.output_dir == "/tmp/observer_default");
    }

    std::filesystem::remove_all(temp_dir);
}

TEST(reject_legacy_command_line_options) {
    full_chain_observer_config config;
    std::string error;
    const observer_parse_result result =
        parse_observer_args({"observer", "--orders-shm", "/orders_legacy"}, &config, &error);
    (void)result;
    assert(result == observer_parse_result::Error);
    assert(!error.empty());
}

TEST(reject_unknown_yaml_key) {
    const std::filesystem::path config_path = unique_path("observer_cfg_unknown");
    write_file(config_path,
        "orders_shm_name: \"/orders_test\"\n"
        "trading_day: \"20260226\"\n"
        "positions_shm_name: \"/positions_test\"\n"
        "output_dir: \"/tmp/observer_out\"\n"
        "unknown_field: 1\n");

    full_chain_observer_config config;
    std::string error;
    const observer_parse_result result =
        parse_observer_args({"observer", "--config", config_path.string()}, &config, &error);
    (void)result;
    assert(result == observer_parse_result::Error);
    assert(!error.empty());

    std::filesystem::remove(config_path);
}

TEST(reject_invalid_trading_day) {
    const std::filesystem::path config_path = unique_path("observer_cfg_bad_day");
    write_file(config_path,
        "orders_shm_name: \"/orders_test\"\n"
        "trading_day: \"2026-02-26\"\n"
        "positions_shm_name: \"/positions_test\"\n"
        "output_dir: \"/tmp/observer_out\"\n");

    full_chain_observer_config config;
    std::string error;
    const observer_parse_result result =
        parse_observer_args({"observer", "--config", config_path.string()}, &config, &error);
    (void)result;
    assert(result == observer_parse_result::Error);
    assert(!error.empty());

    std::filesystem::remove(config_path);
}

TEST(reject_empty_shm_name) {
    const std::filesystem::path config_path = unique_path("observer_cfg_empty_shm");
    write_file(config_path,
        "orders_shm_name: \"\"\n"
        "trading_day: \"20260226\"\n"
        "positions_shm_name: \"/positions_test\"\n"
        "output_dir: \"/tmp/observer_out\"\n");

    full_chain_observer_config config;
    std::string error;
    const observer_parse_result result =
        parse_observer_args({"observer", "--config", config_path.string()}, &config, &error);
    (void)result;
    assert(result == observer_parse_result::Error);
    assert(!error.empty());

    std::filesystem::remove(config_path);
}

int main() {
    printf("=== Full Chain Observer Config Test Suite ===\n\n");

    RUN_TEST(load_from_config_flag);
    RUN_TEST(load_from_positional_path);
    RUN_TEST(load_from_default_config_path);
    RUN_TEST(reject_legacy_command_line_options);
    RUN_TEST(reject_unknown_yaml_key);
    RUN_TEST(reject_invalid_trading_day);
    RUN_TEST(reject_empty_shm_name);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
