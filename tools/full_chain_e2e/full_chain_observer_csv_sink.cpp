#include "full_chain_observer_csv_sink.hpp"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <system_error>

#include "full_chain_observer_time.hpp"

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

// 向当前行追加固定数量的空 CSV 字段，保证不同事件类型列宽一致。
void write_empty_csv_fields(std::ostream& stream, std::size_t count) {
    for (std::size_t index = 0; index < count; ++index) {
        stream << ",\"\"";
    }
}

// 将 trade_side 数值映射为可读字符串，便于最终 CSV 直接定位买卖方向。
const char* trade_side_name(uint8_t trade_side) {
    switch (trade_side) {
        case 1:
            return "buy";
        case 2:
            return "sell";
        case 0:
            return "not_set";
        default:
            return "unknown";
    }
}

// 将“分”单位价格格式化为带两位小数的字符串，便于人工核对。
std::string format_decimal_price(uint64_t dprice) {
    const uint64_t integer_part = dprice / 100;
    const uint64_t fraction_part = dprice % 100;
    std::string price = std::to_string(integer_part);
    price.push_back('.');
    if (fraction_part < 10) {
        price.push_back('0');
    }
    price += std::to_string(fraction_part);
    return price;
}

}  // namespace

// 析构时确保输出状态复位，避免下一次复用旧快照。
full_chain_observer_csv_sink::~full_chain_observer_csv_sink() { close(); }

// 打开输出目录并初始化最终快照 CSV。
bool full_chain_observer_csv_sink::open(const std::filesystem::path& output_dir, std::string* out_error) {
    close();

    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        set_error(out_error, std::string("create_directories failed: ") + ec.message());
        return false;
    }

    orders_csv_path_ = output_dir / "orders_final.csv";
    positions_csv_path_ = output_dir / "positions_final.csv";
    orders_snapshot_.clear();
    positions_snapshot_.clear();
    info_snapshot_ = acct_positions_mon_info_t{};
    fund_snapshot_ = acct_positions_mon_fund_snapshot_t{};
    has_info_snapshot_ = false;
    has_fund_snapshot_ = false;
    opened_ = true;
    flush();
    return true;
}

// 关闭输出并清理缓存快照。
void full_chain_observer_csv_sink::close() noexcept {
    opened_ = false;
    orders_csv_path_.clear();
    positions_csv_path_.clear();
    orders_snapshot_.clear();
    positions_snapshot_.clear();
    info_snapshot_ = acct_positions_mon_info_t{};
    fund_snapshot_ = acct_positions_mon_fund_snapshot_t{};
    has_info_snapshot_ = false;
    has_fund_snapshot_ = false;
}

// 记录订单最新快照；同一 internal_order_id 始终保留最新状态。
bool full_chain_observer_csv_sink::append_order_event(
    const full_chain_observer_order_event& event, std::string* out_error) {
    if (!opened_) {
        set_error(out_error, "csv sink is not opened");
        return false;
    }

    const acct_orders_mon_snapshot_t& snapshot = event.snapshot;
    if (snapshot.internal_order_id == 0) {
        return true;
    }
    orders_snapshot_[snapshot.internal_order_id] = snapshot;
    return true;
}

// 记录持仓最新快照；移除事件会删除对应行。
bool full_chain_observer_csv_sink::append_position_event(
    const full_chain_observer_position_event& event, std::string* out_error) {
    if (!opened_) {
        set_error(out_error, "csv sink is not opened");
        return false;
    }

    switch (event.kind) {
        case full_chain_observer_position_event_kind::Header:
            info_snapshot_ = event.info;
            has_info_snapshot_ = true;
            break;
        case full_chain_observer_position_event_kind::Fund:
            fund_snapshot_ = event.fund;
            has_fund_snapshot_ = true;
            break;
        case full_chain_observer_position_event_kind::Position:
            positions_snapshot_[event.row_key] = event.position;
            break;
        case full_chain_observer_position_event_kind::PositionRemoved:
            positions_snapshot_.erase(event.row_key);
            break;
        default:
            break;
    }
    return true;
}

// 覆盖写当前快照，确保 CSV 始终表示“当前最终态”而非事件流。
void full_chain_observer_csv_sink::flush() {
    if (!opened_) {
        return;
    }

    {
        std::ofstream orders_stream(orders_csv_path_, std::ios::out | std::ios::trunc);
        if (orders_stream.is_open()) {
            orders_stream << "last_update_time,last_update_ns,index,seq,internal_order_id,security_id,internal_security_id,"
                             "stage,status,volume_entrust,volume_traded,volume_remain,side,dprice_entrust,dprice_traded,"
                             "price_entrust,price_traded\n";
            for (const auto& [order_id, snapshot] : orders_snapshot_) {
                (void)order_id;
                orders_stream << csv_escape(observer_time::format_unix_time_ns(snapshot.last_update_ns)) << ','
                              << snapshot.last_update_ns << ',' << snapshot.index << ',' << snapshot.seq << ','
                              << snapshot.internal_order_id << ','
                              << csv_escape(make_fixed_string(snapshot.security_id, sizeof(snapshot.security_id))) << ','
                              << csv_escape(make_fixed_string(snapshot.internal_security_id, sizeof(snapshot.internal_security_id)))
                              << ',' << static_cast<unsigned>(snapshot.stage) << ','
                              << static_cast<unsigned>(snapshot.order_status) << ',' << snapshot.volume_entrust << ','
                              << snapshot.volume_traded << ',' << snapshot.volume_remain << ','
                              << csv_escape(trade_side_name(snapshot.trade_side)) << ',' << snapshot.dprice_entrust << ','
                              << snapshot.dprice_traded << ',' << csv_escape(format_decimal_price(snapshot.dprice_entrust))
                              << ',' << csv_escape(format_decimal_price(snapshot.dprice_traded)) << '\n';
            }
        }
    }

    {
        std::ofstream positions_stream(positions_csv_path_, std::ios::out | std::ios::trunc);
        if (!positions_stream.is_open()) {
            return;
        }
        positions_stream << "last_update_time,last_update_ns,event_kind,row_key,"
                            "header_position_count,header_last_update_time,header_last_update_ns,"
                            "fund_total_asset,fund_available,fund_frozen,fund_market_value,"
                            "position_index,position_id,position_name,position_available,"
                            "position_volume_available_t0,position_volume_available_t1,"
                            "position_volume_buy_traded,position_volume_sell_traded,"
                            "position_removed\n";

        if (has_info_snapshot_) {
            positions_stream << csv_escape(observer_time::format_unix_time_ns(info_snapshot_.last_update_ns)) << ','
                             << info_snapshot_.last_update_ns << ','
                             << csv_escape("header") << ','
                             << csv_escape("positions_shm") << ','
                             << info_snapshot_.position_count << ','
                             << csv_escape(observer_time::format_unix_time_ns(info_snapshot_.last_update_ns)) << ','
                             << info_snapshot_.last_update_ns;
            write_empty_csv_fields(positions_stream, 4);
            write_empty_csv_fields(positions_stream, 8);
            positions_stream << ','
                             << '0' << '\n';
        }

        if (has_fund_snapshot_) {
            positions_stream << csv_escape(observer_time::format_unix_time_ns(fund_snapshot_.last_update_ns)) << ','
                             << fund_snapshot_.last_update_ns << ','
                             << csv_escape("fund") << ','
                             << csv_escape(make_fixed_string(fund_snapshot_.id, sizeof(fund_snapshot_.id)));
            write_empty_csv_fields(positions_stream, 3);
            positions_stream << ','
                             << fund_snapshot_.total_asset << ','
                             << fund_snapshot_.available << ','
                             << fund_snapshot_.frozen << ','
                             << fund_snapshot_.market_value;
            write_empty_csv_fields(positions_stream, 8);
            positions_stream << ','
                             << '0' << '\n';
        }

        for (const auto& [row_key, snapshot] : positions_snapshot_) {
            positions_stream << csv_escape(observer_time::format_unix_time_ns(snapshot.last_update_ns)) << ','
                             << snapshot.last_update_ns << ','
                             << csv_escape("position") << ','
                             << csv_escape(row_key);
            write_empty_csv_fields(positions_stream, 7);
            positions_stream << ','
                             << snapshot.index << ','
                             << csv_escape(make_fixed_string(snapshot.id, sizeof(snapshot.id))) << ','
                             << csv_escape(make_fixed_string(snapshot.name, sizeof(snapshot.name))) << ','
                             << snapshot.available << ','
                             << snapshot.volume_available_t0 << ','
                             << snapshot.volume_available_t1 << ','
                             << snapshot.volume_buy_traded << ','
                             << snapshot.volume_sell_traded << ','
                             << '0' << '\n';
        }
    }
}

}  // namespace acct_service
