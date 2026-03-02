#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "full_chain_observer_order_watch.hpp"
#include "full_chain_observer_position_watch.hpp"

namespace acct_service {

// CSV 输出封装：维护 orders/positions 最新快照并覆盖写最终结果。
class full_chain_observer_csv_sink {
public:
    full_chain_observer_csv_sink() = default;
    ~full_chain_observer_csv_sink();

    full_chain_observer_csv_sink(const full_chain_observer_csv_sink&) = delete;
    full_chain_observer_csv_sink& operator=(const full_chain_observer_csv_sink&) = delete;

    bool open(const std::filesystem::path& output_dir, std::string* out_error);
    void close() noexcept;

    bool append_order_event(const full_chain_observer_order_event& event, std::string* out_error);
    bool append_position_event(const full_chain_observer_position_event& event, std::string* out_error);
    void flush();

private:
    bool opened_{false};
    std::filesystem::path orders_csv_path_{};
    std::filesystem::path positions_csv_path_{};
    std::map<uint32_t, acct_orders_mon_snapshot_t> orders_snapshot_{};
    std::map<std::string, acct_positions_mon_position_snapshot_t> positions_snapshot_{};
    acct_positions_mon_info_t info_snapshot_{};
    acct_positions_mon_fund_snapshot_t fund_snapshot_{};
    bool has_info_snapshot_{false};
    bool has_fund_snapshot_{false};
};

}  // namespace acct_service
