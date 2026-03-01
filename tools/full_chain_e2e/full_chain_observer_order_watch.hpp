#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "api/order_monitor_api.h"

namespace acct_service {

// 单条订单观测事件（包含抓取时刻和稳定快照）。
struct full_chain_observer_order_event {
    uint64_t observed_time_ns{0};
    acct_orders_mon_snapshot_t snapshot{};
};

// 订单观测模块打开参数。
struct full_chain_observer_order_watch_options {
    std::string orders_shm_name{"/orders_shm"};
    std::string trading_day{"19700101"};
};

// 订单监控封装：负责只读打开、轮询增量事件与安全关闭。
class full_chain_observer_order_watch {
public:
    full_chain_observer_order_watch() = default;
    ~full_chain_observer_order_watch();

    full_chain_observer_order_watch(const full_chain_observer_order_watch&) = delete;
    full_chain_observer_order_watch& operator=(const full_chain_observer_order_watch&) = delete;

    bool open(const full_chain_observer_order_watch_options& options, std::string* out_error);
    void close() noexcept;
    bool poll(std::vector<full_chain_observer_order_event>* out_events, std::string* out_error);

private:
    acct_orders_mon_ctx_t monitor_ctx_{nullptr};
    std::vector<uint64_t> last_seq_by_index_{};
};

}  // namespace acct_service
