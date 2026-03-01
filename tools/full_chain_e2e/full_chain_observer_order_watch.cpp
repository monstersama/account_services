#include "full_chain_observer_order_watch.hpp"

#include <chrono>
#include <thread>

namespace acct_service {
namespace {

// 生成 Unix Epoch 纳秒时间戳，用于事件打点。
uint64_t now_unix_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

// 统一写错误字符串，避免调用点重复判空逻辑。
void set_error(std::string* out_error, const std::string& message) {
    if (out_error != nullptr) {
        *out_error = message;
    }
}

// 在并发写场景下重试读取单个订单槽位，屏蔽短暂 seqlock 冲突。
acct_mon_error_t read_snapshot_with_retry(
    acct_orders_mon_ctx_t monitor_ctx, uint32_t index, acct_orders_mon_snapshot_t* out_snapshot) {
    for (int retry = 0; retry < 16; ++retry) {
        const acct_mon_error_t rc = acct_orders_mon_read(monitor_ctx, index, out_snapshot);
        if (rc == ACCT_MON_OK || rc == ACCT_MON_ERR_NOT_FOUND) {
            return rc;
        }
        if (rc != ACCT_MON_ERR_RETRY) {
            return rc;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ACCT_MON_ERR_RETRY;
}

}  // namespace

// 析构时兜底关闭监控句柄，确保资源释放路径确定。
full_chain_observer_order_watch::~full_chain_observer_order_watch() { close(); }

// 打开 orders_shm 监控并初始化增量去重状态。
bool full_chain_observer_order_watch::open(
    const full_chain_observer_order_watch_options& options, std::string* out_error) {
    close();

    acct_orders_mon_options_t api_options{};
    api_options.orders_shm_name = options.orders_shm_name.c_str();
    api_options.trading_day = options.trading_day.c_str();

    const acct_mon_error_t open_rc = acct_orders_mon_open(&api_options, &monitor_ctx_);
    if (open_rc != ACCT_MON_OK) {
        set_error(out_error, std::string("acct_orders_mon_open failed: ") + acct_orders_mon_strerror(open_rc));
        monitor_ctx_ = nullptr;
        return false;
    }

    acct_orders_mon_info_t info{};
    const acct_mon_error_t info_rc = acct_orders_mon_info(monitor_ctx_, &info);
    if (info_rc != ACCT_MON_OK) {
        set_error(out_error, std::string("acct_orders_mon_info failed: ") + acct_orders_mon_strerror(info_rc));
        close();
        return false;
    }

    last_seq_by_index_.assign(info.capacity, 0);
    return true;
}

// 主动关闭订单监控句柄，便于上层控制生命周期。
void full_chain_observer_order_watch::close() noexcept {
    if (monitor_ctx_ != nullptr) {
        (void)acct_orders_mon_close(monitor_ctx_);
        monitor_ctx_ = nullptr;
    }
    last_seq_by_index_.clear();
}

// 轮询当前可见订单并输出序号变化事件。
bool full_chain_observer_order_watch::poll(
    std::vector<full_chain_observer_order_event>* out_events, std::string* out_error) {
    if (out_events == nullptr) {
        set_error(out_error, "poll out_events is null");
        return false;
    }
    out_events->clear();

    if (monitor_ctx_ == nullptr) {
        set_error(out_error, "order watch is not opened");
        return false;
    }

    acct_orders_mon_info_t info{};
    const acct_mon_error_t info_rc = acct_orders_mon_info(monitor_ctx_, &info);
    if (info_rc != ACCT_MON_OK) {
        set_error(out_error, std::string("acct_orders_mon_info failed: ") + acct_orders_mon_strerror(info_rc));
        return false;
    }

    if (last_seq_by_index_.size() < info.next_index) {
        last_seq_by_index_.resize(info.next_index, 0);
    }

    for (uint32_t index = 0; index < info.next_index; ++index) {
        acct_orders_mon_snapshot_t snapshot{};
        const acct_mon_error_t read_rc = read_snapshot_with_retry(monitor_ctx_, index, &snapshot);
        if (read_rc == ACCT_MON_ERR_NOT_FOUND) {
            continue;
        }
        if (read_rc != ACCT_MON_OK) {
            set_error(out_error, std::string("acct_orders_mon_read failed: ") + acct_orders_mon_strerror(read_rc));
            return false;
        }

        if (index >= last_seq_by_index_.size()) {
            last_seq_by_index_.resize(index + 1, 0);
        }

        if (snapshot.seq == last_seq_by_index_[index]) {
            continue;
        }

        last_seq_by_index_[index] = snapshot.seq;
        full_chain_observer_order_event event{};
        event.observed_time_ns = now_unix_ns();
        event.snapshot = snapshot;
        out_events->push_back(event);
    }

    return true;
}

}  // namespace acct_service
