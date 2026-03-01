#include "full_chain_observer_position_watch.hpp"

#include <chrono>
#include <cstring>
#include <thread>

namespace acct_service {
namespace {

// 统一写错误字符串，避免调用点重复判空逻辑。
void set_error(std::string* out_error, const std::string& message) {
    if (out_error != nullptr) {
        *out_error = message;
    }
}

// 生成 Unix Epoch 纳秒时间戳，用于事件打点。
uint64_t now_unix_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// 提取固定长度 C 字符数组中的有效字符串内容。
std::string make_fixed_string(const char* input, std::size_t capacity) {
    return std::string(input, ::strnlen(input, capacity));
}

// 比较 positions header 快照是否有变化。
bool is_same_info(const acct_positions_mon_info_t& lhs, const acct_positions_mon_info_t& rhs) {
    return lhs.magic == rhs.magic &&
           lhs.version == rhs.version &&
           lhs.capacity == rhs.capacity &&
           lhs.init_state == rhs.init_state &&
           lhs.position_count == rhs.position_count &&
           lhs.next_security_id == rhs.next_security_id &&
           lhs.create_time_ns == rhs.create_time_ns &&
           lhs.last_update_ns == rhs.last_update_ns;
}

// 比较资金行快照是否有变化。
bool is_same_fund(const acct_positions_mon_fund_snapshot_t& lhs, const acct_positions_mon_fund_snapshot_t& rhs) {
    return lhs.last_update_ns == rhs.last_update_ns &&
           std::memcmp(lhs.id, rhs.id, sizeof(lhs.id)) == 0 &&
           std::memcmp(lhs.name, rhs.name, sizeof(lhs.name)) == 0 &&
           lhs.total_asset == rhs.total_asset &&
           lhs.available == rhs.available &&
           lhs.frozen == rhs.frozen &&
           lhs.market_value == rhs.market_value &&
           lhs.count_order == rhs.count_order;
}

// 比较证券行快照是否有变化。
bool is_same_position(const acct_positions_mon_position_snapshot_t& lhs, const acct_positions_mon_position_snapshot_t& rhs) {
    return lhs.index == rhs.index &&
           lhs.row_index == rhs.row_index &&
           lhs.last_update_ns == rhs.last_update_ns &&
           std::memcmp(lhs.id, rhs.id, sizeof(lhs.id)) == 0 &&
           std::memcmp(lhs.name, rhs.name, sizeof(lhs.name)) == 0 &&
           lhs.available == rhs.available &&
           lhs.volume_available_t0 == rhs.volume_available_t0 &&
           lhs.volume_available_t1 == rhs.volume_available_t1 &&
           lhs.volume_buy == rhs.volume_buy &&
           lhs.dvalue_buy == rhs.dvalue_buy &&
           lhs.volume_buy_traded == rhs.volume_buy_traded &&
           lhs.dvalue_buy_traded == rhs.dvalue_buy_traded &&
           lhs.volume_sell == rhs.volume_sell &&
           lhs.dvalue_sell == rhs.dvalue_sell &&
           lhs.volume_sell_traded == rhs.volume_sell_traded &&
           lhs.dvalue_sell_traded == rhs.dvalue_sell_traded &&
           lhs.count_order == rhs.count_order;
}

// 从证券快照中提取稳定行键，用于终端与 CSV 定位。
std::string make_position_row_key(const acct_positions_mon_position_snapshot_t& snapshot) {
    const std::string id = make_fixed_string(snapshot.id, sizeof(snapshot.id));
    if (!id.empty()) {
        return id;
    }
    return std::string("index:") + std::to_string(snapshot.index);
}

// 在并发写场景下重试读取资金行，屏蔽短暂冲突。
acct_pos_mon_error_t read_fund_with_retry(
    acct_positions_mon_ctx_t monitor_ctx, acct_positions_mon_fund_snapshot_t* out_snapshot) {
    for (int retry = 0; retry < 16; ++retry) {
        const acct_pos_mon_error_t rc = acct_positions_mon_read_fund(monitor_ctx, out_snapshot);
        if (rc == ACCT_POS_MON_OK) {
            return rc;
        }
        if (rc != ACCT_POS_MON_ERR_RETRY) {
            return rc;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ACCT_POS_MON_ERR_RETRY;
}

// 在并发写场景下重试读取证券行，屏蔽短暂冲突。
acct_pos_mon_error_t read_position_with_retry(
    acct_positions_mon_ctx_t monitor_ctx, uint32_t index, acct_positions_mon_position_snapshot_t* out_snapshot) {
    for (int retry = 0; retry < 16; ++retry) {
        const acct_pos_mon_error_t rc = acct_positions_mon_read_position(monitor_ctx, index, out_snapshot);
        if (rc == ACCT_POS_MON_OK || rc == ACCT_POS_MON_ERR_NOT_FOUND) {
            return rc;
        }
        if (rc != ACCT_POS_MON_ERR_RETRY) {
            return rc;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ACCT_POS_MON_ERR_RETRY;
}

// 将 header 变化封装成统一事件。
void append_info_event(
    const acct_positions_mon_info_t& info, std::vector<full_chain_observer_position_event>* out_events) {
    full_chain_observer_position_event event{};
    event.observed_time_ns = now_unix_ns();
    event.kind = full_chain_observer_position_event_kind::Header;
    event.row_key = "positions_shm";
    event.info = info;
    out_events->push_back(event);
}

// 将资金行变化封装成统一事件。
void append_fund_event(
    const acct_positions_mon_fund_snapshot_t& fund, std::vector<full_chain_observer_position_event>* out_events) {
    full_chain_observer_position_event event{};
    event.observed_time_ns = now_unix_ns();
    event.kind = full_chain_observer_position_event_kind::Fund;
    event.row_key = make_fixed_string(fund.id, sizeof(fund.id));
    event.fund = fund;
    out_events->push_back(event);
}

// 将证券行变化封装成统一事件。
void append_position_event(
    const acct_positions_mon_position_snapshot_t& position, std::vector<full_chain_observer_position_event>* out_events) {
    full_chain_observer_position_event event{};
    event.observed_time_ns = now_unix_ns();
    event.kind = full_chain_observer_position_event_kind::Position;
    event.row_key = make_position_row_key(position);
    event.position = position;
    out_events->push_back(event);
}

// 在 position_count 收缩时产出行移除事件，避免监控端误判。
void append_position_removed_event(
    const acct_positions_mon_position_snapshot_t& snapshot, std::vector<full_chain_observer_position_event>* out_events) {
    full_chain_observer_position_event event{};
    event.observed_time_ns = now_unix_ns();
    event.kind = full_chain_observer_position_event_kind::PositionRemoved;
    event.row_key = make_position_row_key(snapshot);
    event.position = snapshot;
    out_events->push_back(event);
}

}  // namespace

// 析构时关闭持仓监控状态，保证生命周期闭合。
full_chain_observer_position_watch::~full_chain_observer_position_watch() { close(); }

// 打开持仓观测模块并建立只读监控上下文。
bool full_chain_observer_position_watch::open(
    const full_chain_observer_position_watch_options& options, std::string* out_error) {
    if (options.positions_shm_name.empty()) {
        set_error(out_error, "positions_shm_name is empty");
        return false;
    }

    close();

    acct_positions_mon_options_t api_options{};
    api_options.positions_shm_name = options.positions_shm_name.c_str();

    const acct_pos_mon_error_t open_rc = acct_positions_mon_open(&api_options, &monitor_ctx_);
    if (open_rc != ACCT_POS_MON_OK) {
        set_error(out_error, std::string("acct_positions_mon_open failed: ") + acct_positions_mon_strerror(open_rc));
        monitor_ctx_ = nullptr;
        return false;
    }

    acct_positions_mon_info_t info{};
    const acct_pos_mon_error_t info_rc = acct_positions_mon_info(monitor_ctx_, &info);
    if (info_rc != ACCT_POS_MON_OK) {
        set_error(out_error, std::string("acct_positions_mon_info failed: ") + acct_positions_mon_strerror(info_rc));
        close();
        return false;
    }

    last_info_ = acct_positions_mon_info_t{};
    last_fund_ = acct_positions_mon_fund_snapshot_t{};
    has_last_info_ = false;
    has_last_fund_ = false;
    last_positions_.assign(info.position_count, acct_positions_mon_position_snapshot_t{});
    has_last_positions_.assign(info.position_count, 0U);

    return true;
}

// 关闭持仓观测模块并清理内部状态。
void full_chain_observer_position_watch::close() noexcept {
    if (monitor_ctx_ != nullptr) {
        (void)acct_positions_mon_close(monitor_ctx_);
        monitor_ctx_ = nullptr;
    }

    last_info_ = acct_positions_mon_info_t{};
    last_fund_ = acct_positions_mon_fund_snapshot_t{};
    last_positions_.clear();
    has_last_positions_.clear();
    has_last_info_ = false;
    has_last_fund_ = false;
}

// 轮询持仓变化并输出 header/fund/position 增量事件。
bool full_chain_observer_position_watch::poll(
    std::vector<full_chain_observer_position_event>* out_events, std::string* out_error) {
    if (out_events == nullptr) {
        set_error(out_error, "poll out_events is null");
        return false;
    }
    out_events->clear();

    if (monitor_ctx_ == nullptr) {
        set_error(out_error, "position watch is not opened");
        return false;
    }

    acct_positions_mon_info_t info{};
    const acct_pos_mon_error_t info_rc = acct_positions_mon_info(monitor_ctx_, &info);
    if (info_rc != ACCT_POS_MON_OK) {
        set_error(out_error, std::string("acct_positions_mon_info failed: ") + acct_positions_mon_strerror(info_rc));
        return false;
    }

    if (!has_last_info_ || !is_same_info(last_info_, info)) {
        append_info_event(info, out_events);
        last_info_ = info;
        has_last_info_ = true;
    }

    acct_positions_mon_fund_snapshot_t fund{};
    const acct_pos_mon_error_t fund_rc = read_fund_with_retry(monitor_ctx_, &fund);
    if (fund_rc != ACCT_POS_MON_OK) {
        set_error(out_error, std::string("acct_positions_mon_read_fund failed: ") + acct_positions_mon_strerror(fund_rc));
        return false;
    }
    if (!has_last_fund_ || !is_same_fund(last_fund_, fund)) {
        append_fund_event(fund, out_events);
        last_fund_ = fund;
        has_last_fund_ = true;
    }

    const std::size_t visible_count = static_cast<std::size_t>(info.position_count);
    if (last_positions_.size() < visible_count) {
        last_positions_.resize(visible_count);
    }
    if (has_last_positions_.size() < visible_count) {
        has_last_positions_.resize(visible_count, 0U);
    }

    for (uint32_t index = 0; index < info.position_count; ++index) {
        acct_positions_mon_position_snapshot_t snapshot{};
        const acct_pos_mon_error_t position_rc = read_position_with_retry(monitor_ctx_, index, &snapshot);
        if (position_rc == ACCT_POS_MON_ERR_NOT_FOUND) {
            continue;
        }
        if (position_rc != ACCT_POS_MON_OK) {
            set_error(out_error, std::string("acct_positions_mon_read_position failed: ") +
                                     acct_positions_mon_strerror(position_rc));
            return false;
        }

        const std::size_t idx = static_cast<std::size_t>(index);
        if (has_last_positions_[idx] == 0U || !is_same_position(last_positions_[idx], snapshot)) {
            append_position_event(snapshot, out_events);
            last_positions_[idx] = snapshot;
            has_last_positions_[idx] = 1U;
        }
    }

    for (std::size_t idx = visible_count; idx < has_last_positions_.size(); ++idx) {
        if (has_last_positions_[idx] != 0U) {
            append_position_removed_event(last_positions_[idx], out_events);
            last_positions_[idx] = acct_positions_mon_position_snapshot_t{};
            has_last_positions_[idx] = 0U;
        }
    }

    return true;
}

}  // namespace acct_service
