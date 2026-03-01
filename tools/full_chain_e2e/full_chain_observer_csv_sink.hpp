#pragma once

#include <filesystem>
#include <fstream>
#include <string>

#include "full_chain_observer_order_watch.hpp"
#include "full_chain_observer_position_watch.hpp"

namespace acct_service {

// CSV 输出封装：统一管理 orders/positions 两路事件落盘。
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
    std::ofstream orders_stream_{};
    std::ofstream positions_stream_{};
};

}  // namespace acct_service
