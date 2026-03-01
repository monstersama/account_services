#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "full_chain_observer_csv_sink.hpp"
#include "full_chain_observer_order_watch.hpp"
#include "full_chain_observer_position_watch.hpp"

namespace acct_service {
namespace {

// 命令行参数集合：覆盖观测器最小可运行配置。
struct full_chain_observer_cli_options {
    std::string orders_shm_name{"/orders_shm"};
    std::string trading_day{"19700101"};
    std::string positions_shm_name{"/positions_shm"};
    std::string output_dir{"."};
    uint32_t poll_interval_ms{200};
    uint32_t timeout_ms{30000};
};

// 打印命令行帮助并说明默认值。
void print_usage(const char* program_name) {
    std::fprintf(stderr,
        "Usage: %s [--orders-shm NAME] [--trading-day YYYYMMDD] [--positions-shm NAME]\n"
        "          [--poll-ms N] [--timeout-ms N] [--output-dir DIR]\n",
        program_name);
}

// 解析无符号整数参数，失败时返回 false。
bool parse_uint32(const char* text, uint32_t* out_value) {
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    if (parsed > 0xFFFFFFFFUL) {
        return false;
    }
    *out_value = static_cast<uint32_t>(parsed);
    return true;
}

// 解析命令行参数并填充运行选项。
bool parse_cli_args(int argc, char** argv, full_chain_observer_cli_options* out_options) {
    if (out_options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", arg.c_str());
            return false;
        }

        const char* value = argv[++i];
        if (arg == "--orders-shm") {
            out_options->orders_shm_name = value;
        } else if (arg == "--trading-day") {
            out_options->trading_day = value;
        } else if (arg == "--positions-shm") {
            out_options->positions_shm_name = value;
        } else if (arg == "--output-dir") {
            out_options->output_dir = value;
        } else if (arg == "--poll-ms") {
            if (!parse_uint32(value, &out_options->poll_interval_ms)) {
                std::fprintf(stderr, "invalid --poll-ms value: %s\n", value);
                return false;
            }
        } else if (arg == "--timeout-ms") {
            if (!parse_uint32(value, &out_options->timeout_ms)) {
                std::fprintf(stderr, "invalid --timeout-ms value: %s\n", value);
                return false;
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }
    return true;
}

// 判断是否仅请求帮助信息。
bool wants_help(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return true;
        }
    }
    return false;
}

// 输出订单事件到终端，便于运行中实时观察。
void print_order_event(const full_chain_observer_order_event& event) {
    const acct_orders_mon_snapshot_t& snapshot = event.snapshot;
    std::printf(
        "[order] t=%llu idx=%u seq=%llu order_id=%u stage=%u status=%u entrust=%llu traded=%llu remain=%llu\n",
        static_cast<unsigned long long>(event.observed_time_ns),
        snapshot.index,
        static_cast<unsigned long long>(snapshot.seq),
        snapshot.internal_order_id,
        static_cast<unsigned>(snapshot.stage),
        static_cast<unsigned>(snapshot.order_status),
        static_cast<unsigned long long>(snapshot.volume_entrust),
        static_cast<unsigned long long>(snapshot.volume_traded),
        static_cast<unsigned long long>(snapshot.volume_remain));
}

// 输出持仓事件到终端，保持 orders/positions 输出格式一致。
void print_position_event(const full_chain_observer_position_event& event) {
    switch (event.kind) {
        case full_chain_observer_position_event_kind::Header:
            std::printf(
                "[position] t=%llu type=header key=%s position_count=%u last_update_ns=%llu\n",
                static_cast<unsigned long long>(event.observed_time_ns),
                event.row_key.c_str(),
                event.info.position_count,
                static_cast<unsigned long long>(event.info.last_update_ns));
            return;
        case full_chain_observer_position_event_kind::Fund:
            std::printf(
                "[position] t=%llu type=fund key=%s available=%llu frozen=%llu market_value=%llu\n",
                static_cast<unsigned long long>(event.observed_time_ns),
                event.row_key.c_str(),
                static_cast<unsigned long long>(event.fund.available),
                static_cast<unsigned long long>(event.fund.frozen),
                static_cast<unsigned long long>(event.fund.market_value));
            return;
        case full_chain_observer_position_event_kind::Position:
            std::printf(
                "[position] t=%llu type=position key=%s idx=%u buy_traded=%llu sell_traded=%llu\n",
                static_cast<unsigned long long>(event.observed_time_ns),
                event.row_key.c_str(),
                event.position.index,
                static_cast<unsigned long long>(event.position.volume_buy_traded),
                static_cast<unsigned long long>(event.position.volume_sell_traded));
            return;
        case full_chain_observer_position_event_kind::PositionRemoved:
            std::printf(
                "[position] t=%llu type=position_removed key=%s idx=%u\n",
                static_cast<unsigned long long>(event.observed_time_ns),
                event.row_key.c_str(),
                event.position.index);
            return;
        default:
            std::printf(
                "[position] t=%llu type=unknown key=%s\n",
                static_cast<unsigned long long>(event.observed_time_ns),
                event.row_key.c_str());
            return;
    }
}

}  // namespace
}  // namespace acct_service

int main(int argc, char** argv) {
    using namespace acct_service;

    // 1) 解析参数并构造运行时配置。
    if (wants_help(argc, argv)) {
        print_usage(argv[0]);
        return 0;
    }
    full_chain_observer_cli_options cli_options{};
    if (!parse_cli_args(argc, argv, &cli_options)) {
        return 1;
    }

    // 2) 打开订单监控模块。
    full_chain_observer_order_watch order_watch{};
    std::string error_message;
    full_chain_observer_order_watch_options order_watch_options{};
    order_watch_options.orders_shm_name = cli_options.orders_shm_name;
    order_watch_options.trading_day = cli_options.trading_day;
    if (!order_watch.open(order_watch_options, &error_message)) {
        std::fprintf(stderr, "open order watch failed: %s\n", error_message.c_str());
        return 1;
    }

    // 3) 打开持仓监控模块（基于 position_monitor_api）。
    full_chain_observer_position_watch position_watch{};
    full_chain_observer_position_watch_options position_watch_options{};
    position_watch_options.positions_shm_name = cli_options.positions_shm_name;
    if (!position_watch.open(position_watch_options, &error_message)) {
        std::fprintf(stderr, "open position watch failed: %s\n", error_message.c_str());
        return 1;
    }

    // 4) 打开 CSV 落盘通道。
    full_chain_observer_csv_sink csv_sink{};
    if (!csv_sink.open(cli_options.output_dir, &error_message)) {
        std::fprintf(stderr, "open csv sink failed: %s\n", error_message.c_str());
        return 1;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const auto poll_interval = std::chrono::milliseconds(cli_options.poll_interval_ms);

    // 5) 轮询并输出订单/持仓事件，直到达到超时条件。
    for (;;) {
        std::vector<full_chain_observer_order_event> order_events;
        if (!order_watch.poll(&order_events, &error_message)) {
            std::fprintf(stderr, "poll order watch failed: %s\n", error_message.c_str());
            return 1;
        }
        for (const full_chain_observer_order_event& event : order_events) {
            print_order_event(event);
            if (!csv_sink.append_order_event(event, &error_message)) {
                std::fprintf(stderr, "append order event failed: %s\n", error_message.c_str());
                return 1;
            }
        }

        std::vector<full_chain_observer_position_event> position_events;
        if (!position_watch.poll(&position_events, &error_message)) {
            std::fprintf(stderr, "poll position watch failed: %s\n", error_message.c_str());
            return 1;
        }
        for (const full_chain_observer_position_event& event : position_events) {
            print_position_event(event);
            if (!csv_sink.append_position_event(event, &error_message)) {
                std::fprintf(stderr, "append position event failed: %s\n", error_message.c_str());
                return 1;
            }
        }

        csv_sink.flush();

        if (cli_options.timeout_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time);
            if (elapsed.count() >= cli_options.timeout_ms) {
                break;
            }
        }

        std::this_thread::sleep_for(poll_interval);
    }

    return 0;
}
