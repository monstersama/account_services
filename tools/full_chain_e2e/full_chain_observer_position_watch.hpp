#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "api/position_monitor_api.h"

namespace acct_service {

// 持仓事件类型：header/fund/position/position_removed。
enum class full_chain_observer_position_event_kind : uint8_t {
    Header = 0,
    Fund = 1,
    Position = 2,
    PositionRemoved = 3,
};

// 单条持仓观测事件（结构化快照，避免字符串解析）。
struct full_chain_observer_position_event {
    uint64_t observed_time_ns{0};
    full_chain_observer_position_event_kind kind{full_chain_observer_position_event_kind::Header};
    std::string row_key{};
    acct_positions_mon_info_t info{};
    acct_positions_mon_fund_snapshot_t fund{};
    acct_positions_mon_position_snapshot_t position{};
};

// 持仓观测模块打开参数。
struct full_chain_observer_position_watch_options {
    std::string positions_shm_name{"/positions_shm"};
};

// 持仓监控封装：负责对接 position_monitor_api 并输出增量事件。
class full_chain_observer_position_watch {
public:
    full_chain_observer_position_watch() = default;
    ~full_chain_observer_position_watch();

    full_chain_observer_position_watch(const full_chain_observer_position_watch&) = delete;
    full_chain_observer_position_watch& operator=(const full_chain_observer_position_watch&) = delete;

    bool open(const full_chain_observer_position_watch_options& options, std::string* out_error);
    void close() noexcept;
    bool poll(std::vector<full_chain_observer_position_event>* out_events, std::string* out_error);

private:
    acct_positions_mon_ctx_t monitor_ctx_{nullptr};

    acct_positions_mon_info_t last_info_{};
    acct_positions_mon_fund_snapshot_t last_fund_{};
    std::vector<acct_positions_mon_position_snapshot_t> last_positions_{};
    std::vector<uint8_t> has_last_positions_{};

    bool has_last_info_{false};
    bool has_last_fund_{false};
};

}  // namespace acct_service
