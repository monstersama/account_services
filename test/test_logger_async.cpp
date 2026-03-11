#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "common/error.hpp"
#include "common/log.hpp"
#include "core/config_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

using namespace acct_service;

TEST(async_logger_write_and_flush) {
    LogConfig cfg;
    cfg.log_dir = "./build/test_logs";
    cfg.log_level = "debug";
    cfg.async_logging = true;
    cfg.async_queue_size = 2048;
    const std::string output_path = cfg.log_dir + "/account_999.log";
    std::error_code ec;
    std::filesystem::remove(output_path, ec);

    assert(init_logger(cfg, 999));

    for (int i = 0; i < 1000; ++i) {
        ACCT_LOG_INFO("test_logger", "hello logger");
    }

    assert(flush_logger(500));

    std::ifstream in(output_path);
    assert(in.is_open());
    std::string content;
    std::getline(in, content);
    assert(content.find("[INFO][test_logger]") != std::string::npos);
    assert(content.find("msg=hello logger") != std::string::npos);

    shutdown_logger();
}

TEST(queue_full_drop_counter) {
    LogConfig cfg;
    cfg.log_dir = "./build/test_logs";
    cfg.log_level = "debug";
    cfg.async_logging = true;
    cfg.async_queue_size = 16;

    assert(init_logger(cfg, 1000));
    for (int i = 0; i < 5000; ++i) {
        log_message(LogLevel::debug, "test_logger", __FILE__, static_cast<uint32_t>(__LINE__), "drop pressure");
    }

    assert(flush_logger(500));
    const uint64_t dropped = logger_dropped_count();
    shutdown_logger();

    assert(dropped > 0);
}

int main() {
    printf("=== Async Logger Test Suite ===\n\n");

    RUN_TEST(async_logger_write_and_flush);
    RUN_TEST(queue_full_drop_counter);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
