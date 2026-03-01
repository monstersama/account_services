#include "full_chain_observer_csv_sink.hpp"

#include <cerrno>
#include <cstring>
#include <system_error>

namespace acct_service {
namespace {

// 统一写错误字符串，避免调用点重复判空逻辑。
void set_error(std::string* out_error, const std::string& message) {
    if (out_error != nullptr) {
        *out_error = message;
    }
}

// 提取固定长度 C 字符数组中的有效字符串内容。
std::string make_fixed_string(const char* input, std::size_t capacity) {
    const std::size_t length = ::strnlen(input, capacity);
    return std::string(input, length);
}

// 对 CSV 字段做基础转义，避免逗号和引号破坏列结构。
std::string csv_escape(const std::string& value) {
    std::string output;
    output.reserve(value.size() + 2);
    output.push_back('"');
    for (const char ch : value) {
        if (ch == '"') {
            output.push_back('"');
        }
        output.push_back(ch);
    }
    output.push_back('"');
    return output;
}

// 将持仓事件类型转换为稳定字符串。
const char* position_event_kind_name(full_chain_observer_position_event_kind kind) {
    switch (kind) {
        case full_chain_observer_position_event_kind::Header:
            return "header";
        case full_chain_observer_position_event_kind::Fund:
            return "fund";
        case full_chain_observer_position_event_kind::Position:
            return "position";
        case full_chain_observer_position_event_kind::PositionRemoved:
            return "position_removed";
        default:
            return "unknown";
    }
}

}  // namespace

// 析构时确保文件句柄关闭，避免写缓冲遗留。
full_chain_observer_csv_sink::~full_chain_observer_csv_sink() { close(); }

// 打开输出目录并创建 orders/positions 两个 CSV 文件。
bool full_chain_observer_csv_sink::open(const std::filesystem::path& output_dir, std::string* out_error) {
    close();

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        set_error(out_error, std::string("create_directories failed: ") + ec.message());
        return false;
    }

    const std::filesystem::path orders_csv = output_dir / "orders_events.csv";
    const std::filesystem::path positions_csv = output_dir / "positions_events.csv";

    orders_stream_.open(orders_csv, std::ios::out | std::ios::trunc);
    if (!orders_stream_.is_open()) {
        set_error(out_error, std::string("open orders_events.csv failed: ") + std::strerror(errno));
        close();
        return false;
    }

    positions_stream_.open(positions_csv, std::ios::out | std::ios::trunc);
    if (!positions_stream_.is_open()) {
        set_error(out_error, std::string("open positions_events.csv failed: ") + std::strerror(errno));
        close();
        return false;
    }

    orders_stream_ << "observed_time_ns,index,seq,internal_order_id,security_id,internal_security_id,stage,status,"
                      "volume_entrust,volume_traded,volume_remain\n";
    positions_stream_ << "observed_time_ns,event_kind,row_key,"
                         "header_position_count,header_last_update_ns,"
                         "fund_total_asset,fund_available,fund_frozen,fund_market_value,"
                         "position_index,position_id,position_name,position_available,"
                         "position_volume_available_t0,position_volume_available_t1,"
                         "position_volume_buy_traded,position_volume_sell_traded,"
                         "position_removed\n";
    return true;
}

// 关闭文件句柄并清理写入状态。
void full_chain_observer_csv_sink::close() noexcept {
    if (orders_stream_.is_open()) {
        orders_stream_.flush();
        orders_stream_.close();
    }
    if (positions_stream_.is_open()) {
        positions_stream_.flush();
        positions_stream_.close();
    }
}

// 追加一条订单事件行到 orders_events.csv。
bool full_chain_observer_csv_sink::append_order_event(
    const full_chain_observer_order_event& event, std::string* out_error) {
    if (!orders_stream_.is_open()) {
        set_error(out_error, "orders_events.csv is not opened");
        return false;
    }

    const acct_orders_mon_snapshot_t& snapshot = event.snapshot;
    orders_stream_ << event.observed_time_ns << ',' << snapshot.index << ',' << snapshot.seq << ','
                   << snapshot.internal_order_id << ','
                   << csv_escape(make_fixed_string(snapshot.security_id, sizeof(snapshot.security_id))) << ','
                   << csv_escape(make_fixed_string(snapshot.internal_security_id, sizeof(snapshot.internal_security_id)))
                   << ',' << static_cast<unsigned>(snapshot.stage) << ','
                   << static_cast<unsigned>(snapshot.order_status) << ',' << snapshot.volume_entrust << ','
                   << snapshot.volume_traded << ',' << snapshot.volume_remain << '\n';
    return true;
}

// 追加一条持仓事件行到 positions_events.csv。
bool full_chain_observer_csv_sink::append_position_event(
    const full_chain_observer_position_event& event, std::string* out_error) {
    if (!positions_stream_.is_open()) {
        set_error(out_error, "positions_events.csv is not opened");
        return false;
    }

    const bool is_header = event.kind == full_chain_observer_position_event_kind::Header;
    const bool is_fund = event.kind == full_chain_observer_position_event_kind::Fund;
    const bool is_position = event.kind == full_chain_observer_position_event_kind::Position;
    const bool is_removed = event.kind == full_chain_observer_position_event_kind::PositionRemoved;

    auto num_or_empty = [](bool enabled, uint64_t value) -> std::string {
        return enabled ? std::to_string(value) : std::string();
    };
    auto str_or_empty = [](bool enabled, const std::string& value) -> std::string {
        return enabled ? value : std::string();
    };

    const std::string position_id = make_fixed_string(event.position.id, sizeof(event.position.id));
    const std::string position_name = make_fixed_string(event.position.name, sizeof(event.position.name));

    positions_stream_ << event.observed_time_ns << ','
                      << csv_escape(position_event_kind_name(event.kind)) << ','
                      << csv_escape(event.row_key) << ','
                      << num_or_empty(is_header, event.info.position_count) << ','
                      << num_or_empty(is_header, event.info.last_update_ns) << ','
                      << num_or_empty(is_fund, event.fund.total_asset) << ','
                      << num_or_empty(is_fund, event.fund.available) << ','
                      << num_or_empty(is_fund, event.fund.frozen) << ','
                      << num_or_empty(is_fund, event.fund.market_value) << ','
                      << num_or_empty(is_position || is_removed, event.position.index) << ','
                      << csv_escape(str_or_empty(is_position || is_removed, position_id)) << ','
                      << csv_escape(str_or_empty(is_position || is_removed, position_name)) << ','
                      << num_or_empty(is_position || is_removed, event.position.available) << ','
                      << num_or_empty(is_position || is_removed, event.position.volume_available_t0) << ','
                      << num_or_empty(is_position || is_removed, event.position.volume_available_t1) << ','
                      << num_or_empty(is_position || is_removed, event.position.volume_buy_traded) << ','
                      << num_or_empty(is_position || is_removed, event.position.volume_sell_traded) << ','
                      << (is_removed ? 1 : 0) << '\n';
    return true;
}

// 将缓冲区主动刷盘，降低进程异常退出时的数据丢失窗口。
void full_chain_observer_csv_sink::flush() {
    if (orders_stream_.is_open()) {
        orders_stream_.flush();
    }
    if (positions_stream_.is_open()) {
        positions_stream_.flush();
    }
}

}  // namespace acct_service
