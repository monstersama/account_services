#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

#include "order/order_event_recorder.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

// 创建唯一测试目录，避免不同运行之间互相覆盖日志文件。
std::filesystem::path unique_dir(const char* prefix) {
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
            std::to_string(static_cast<unsigned long long>(now_ns())));
}

// 轮询等待异步 writer 完成文件落盘。
bool wait_until(const std::function<bool()>& predicate, int timeout_ms = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// 读取完整文件内容，文件尚未创建时返回空字符串。
std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// 返回包含目标片段的首行文本，便于按事件类型断言字段集。
std::string find_line_containing(const std::string& content, std::string_view needle) {
    std::istringstream stream(content);
    for (std::string line; std::getline(stream, line);) {
        if (line.find(needle) != std::string::npos) {
            return line;
        }
    }
    return {};
}

}  // namespace

TEST(records_business_events_and_debug_trace) {
    const std::filesystem::path output_dir = unique_dir("order_event_recorder");
    std::filesystem::create_directories(output_dir);

    BusinessLogConfig config;
    config.enabled = true;
    config.output_dir = output_dir.string();
    config.queue_capacity = 16;
    config.flush_interval_ms = 10;

    OrderEventRecorder recorder;
    assert(recorder.init(config, 77, "20260225"));

    OrderEntry entry{};
    entry.request.init_new("000001", InternalSecurityId("XSHE_000001"), static_cast<InternalOrderId>(7001),
                           TradeSide::Buy, Market::SZ, static_cast<Volume>(200), static_cast<DPrice>(1234), 93000000);
    entry.request.order_state.store(OrderState::TraderSubmitted, std::memory_order_relaxed);
    entry.request.execution_algo = PassiveExecutionAlgo::TWAP;
    entry.request.execution_state = ExecutionState::Running;
    entry.request.target_volume = static_cast<Volume>(200);
    entry.request.working_volume = static_cast<Volume>(100);
    entry.request.schedulable_volume = static_cast<Volume>(100);
    entry.submit_time_ns = now_ns();
    entry.last_update_ns = entry.submit_time_ns;
    entry.strategy_id = static_cast<StrategyId>(9);
    entry.is_split_child = true;
    entry.parent_order_id = static_cast<InternalOrderId>(6001);
    entry.shm_order_index = static_cast<OrderIndex>(12);

    recorder.record_order_event(entry, order_book_event_t::Added);
    recorder.record_session_started(entry.request, entry.strategy_id, PassiveExecutionAlgo::TWAP);
    recorder.record_child_submit_attempt(
        entry.request, entry.strategy_id, PassiveExecutionAlgo::TWAP, entry.request.internal_order_id,
        static_cast<Volume>(100), static_cast<DPrice>(1234), ExecutionState::Running, static_cast<Volume>(200),
        static_cast<Volume>(100), static_cast<Volume>(100), true,
        "price_src=market md_seq=11 pub_seq=12 bid1=1233 ask1=1234");
    recorder.record_child_submit_result(entry.request, entry.strategy_id, PassiveExecutionAlgo::TWAP,
                                        entry.request.internal_order_id, entry.shm_order_index,
                                        static_cast<DPrice>(1234), true, "submitted");
    recorder.record_child_finalized(
        entry.request, entry.strategy_id, PassiveExecutionAlgo::TWAP, entry.request.internal_order_id,
        entry.shm_order_index, OrderState::Finished, static_cast<Volume>(100), static_cast<DPrice>(1234),
        static_cast<Volume>(100), static_cast<DPrice>(1235), static_cast<DValue>(123500),
        static_cast<DValue>(12), static_cast<Volume>(0), false, ExecutionState::Finished, static_cast<Volume>(200),
        static_cast<Volume>(0), static_cast<Volume>(100));

    const std::filesystem::path business_path = output_dir / "order_events_77_20260225.log";
    assert(wait_until([&business_path]() {
        const std::string content = read_text_file(business_path);
        const std::string order_added = find_line_containing(content, "event=\"order_added\"");
        return !order_added.empty() && order_added.find("stream=\"business\"") != std::string::npos &&
               order_added.find("order_id=7001") != std::string::npos &&
               order_added.find("parent_order_id=6001") != std::string::npos &&
               order_added.find("execution_algo=\"twap\"") != std::string::npos &&
               order_added.find("shm_order_index=12") != std::string::npos &&
               order_added.find("success=") == std::string::npos;
    }));

    if (should_emit_debug_order_trace()) {
        const std::filesystem::path debug_path = output_dir / "order_debug_77_20260225.log";
        assert(wait_until([&debug_path]() {
            const std::string content = read_text_file(debug_path);
            const std::string session_started = find_line_containing(content, "trace=\"session_started\"");
            const std::string child_submit_attempt = find_line_containing(content, "trace=\"child_submit_attempt\"");
            const std::string child_submit_result = find_line_containing(content, "trace=\"child_submit_result\"");
            const std::string child_finalized = find_line_containing(content, "trace=\"child_finalized\"");
            return !session_started.empty() && !child_submit_attempt.empty() && !child_submit_result.empty() &&
                   !child_finalized.empty() &&
                   session_started.find("stream=\"debug\"") != std::string::npos &&
                   session_started.find("trace=\"session_started\"") != std::string::npos &&
                   session_started.find("success=") == std::string::npos &&
                   session_started.find("shm_order_index=") == std::string::npos &&
                   session_started.find("parent_order_id=7001") != std::string::npos &&
                   session_started.find("dprice_entrust=1234") != std::string::npos &&
                   child_submit_attempt.find("trace=\"child_submit_attempt\"") != std::string::npos &&
                   child_submit_attempt.find("child_order_id=7001") != std::string::npos &&
                   child_submit_attempt.find("dprice_entrust=1234") != std::string::npos &&
                   child_submit_attempt.find("active_strategy_claimed=true") != std::string::npos &&
                   child_submit_attempt.find(
                       "detail=\"price_src=market md_seq=11 pub_seq=12 bid1=1233 ask1=1234\"") !=
                       std::string::npos &&
                   child_submit_result.find("trace=\"child_submit_result\"") != std::string::npos &&
                   child_submit_result.find("child_order_id=7001") != std::string::npos &&
                   child_submit_result.find("dprice_entrust=1234") != std::string::npos &&
                   child_submit_result.find("success=true") != std::string::npos &&
                   child_submit_result.find("result=\"submitted\"") != std::string::npos &&
                   child_submit_result.find("shm_order_index=12") != std::string::npos &&
                   child_finalized.find("trace=\"child_finalized\"") != std::string::npos &&
                   child_finalized.find("child_order_id=7001") != std::string::npos &&
                   child_finalized.find("dprice_entrust=1234") != std::string::npos &&
                   child_finalized.find("dprice_traded=1235") != std::string::npos &&
                   child_finalized.find("dvalue_traded=123500") != std::string::npos &&
                   child_finalized.find("dfee_executed=12") != std::string::npos;
        }));
    }

    recorder.shutdown();
    std::filesystem::remove_all(output_dir);
}

int main() {
    printf("=== Order Event Recorder Test Suite ===\n\n");

    RUN_TEST(records_business_events_and_debug_trace);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
