#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "full_chain_observer_config.hpp"
#include "full_chain_observer_csv_sink.hpp"
#include "full_chain_observer_order_watch.hpp"
#include "full_chain_observer_position_watch.hpp"

namespace acct_service {
namespace {

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

    // 1) 解析参数并加载配置文件。
    full_chain_observer_config observer_config{};
    std::string parse_error;
    const observer_parse_result parse_result = parse_args(argc, argv, &observer_config, &parse_error);
    if (parse_result == observer_parse_result::Help) {
        return 0;
    }
    if (parse_result == observer_parse_result::Error) {
        if (!parse_error.empty()) {
            std::fprintf(stderr, "%s\n", parse_error.c_str());
        }
        print_usage(argv[0]);
        return 1;
    }

    // 2) 打开订单监控模块。
    full_chain_observer_order_watch order_watch{};
    std::string error_message;
    full_chain_observer_order_watch_options order_watch_options{};
    order_watch_options.orders_shm_name = observer_config.orders_shm_name;
    order_watch_options.trading_day = observer_config.trading_day;
    if (!order_watch.open(order_watch_options, &error_message)) {
        std::fprintf(stderr, "open order watch failed: %s\n", error_message.c_str());
        return 1;
    }

    // 3) 打开持仓监控模块（基于 position_monitor_api）。
    full_chain_observer_position_watch position_watch{};
    full_chain_observer_position_watch_options position_watch_options{};
    position_watch_options.positions_shm_name = observer_config.positions_shm_name;
    if (!position_watch.open(position_watch_options, &error_message)) {
        std::fprintf(stderr, "open position watch failed: %s\n", error_message.c_str());
        return 1;
    }

    // 4) 打开 CSV 汇总通道（仅维护最新快照，不记录事件流）。
    full_chain_observer_csv_sink csv_sink{};
    if (!csv_sink.open(observer_config.output_dir, &error_message)) {
        std::fprintf(stderr, "open csv sink failed: %s\n", error_message.c_str());
        return 1;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const auto poll_interval = std::chrono::milliseconds(observer_config.poll_interval_ms);

    // 5) 轮询并输出订单/持仓日志，同时刷新最终快照 CSV。
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

        if (observer_config.timeout_ms > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time);
            if (elapsed.count() >= observer_config.timeout_ms) {
                break;
            }
        }

        std::this_thread::sleep_for(poll_interval);
    }

    return 0;
}
