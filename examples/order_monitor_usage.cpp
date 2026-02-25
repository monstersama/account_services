/**
 * 监控 SDK 使用示例（纯只读）
 *
 * 运行前请确保账户服务已创建当日 orders_shm。
 */

#include <chrono>
#include <cstdio>
#include <thread>

#include "api/order_monitor_api.h"

namespace {

void print_snapshot(const acct_orders_mon_snapshot_t& snapshot) {
    std::printf(
        "idx=%u order_id=%u sec=%s type=%u side=%u stage=%u status=%u entrust=%llu traded=%llu remain=%llu\n",
        snapshot.index,
        snapshot.internal_order_id,
        snapshot.security_id,
        static_cast<unsigned>(snapshot.order_type),
        static_cast<unsigned>(snapshot.trade_side),
        static_cast<unsigned>(snapshot.stage),
        static_cast<unsigned>(snapshot.order_status),
        static_cast<unsigned long long>(snapshot.volume_entrust),
        static_cast<unsigned long long>(snapshot.volume_traded),
        static_cast<unsigned long long>(snapshot.volume_remain));
}

acct_mon_error_t read_snapshot_with_retry(
    acct_orders_mon_ctx_t mon_ctx, uint32_t index, acct_orders_mon_snapshot_t* out_snapshot) {
    for (int i = 0; i < 16; ++i) {
        const acct_mon_error_t rc = acct_orders_mon_read(mon_ctx, index, out_snapshot);
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

int main() {
    std::printf("=== 监控 SDK 示例（只读）===\n\n");

    acct_orders_mon_ctx_t mon_ctx = nullptr;
    acct_orders_mon_options_t options{};
    options.orders_shm_name = "/orders_shm";
    options.trading_day = "20260225";

    const acct_mon_error_t open_rc = acct_orders_mon_open(&options, &mon_ctx);
    if (open_rc != ACCT_MON_OK) {
        std::fprintf(stderr, "acct_orders_mon_open failed: %s\n", acct_orders_mon_strerror(open_rc));
        std::fprintf(stderr, "请先确认账户服务已创建对应交易日共享内存。\n");
        return 1;
    }

    acct_orders_mon_info_t info{};
    const acct_mon_error_t info_rc = acct_orders_mon_info(mon_ctx, &info);
    if (info_rc != ACCT_MON_OK) {
        std::fprintf(stderr, "acct_orders_mon_info failed: %s\n", acct_orders_mon_strerror(info_rc));
        (void)acct_orders_mon_close(mon_ctx);
        return 1;
    }

    std::printf(
        "[monitor] trading_day=%s capacity=%u next_index=%u reject=%llu\n",
        info.trading_day,
        info.capacity,
        info.next_index,
        static_cast<unsigned long long>(info.full_reject_count));

    for (uint32_t index = 0; index < info.next_index; ++index) {
        acct_orders_mon_snapshot_t snapshot{};
        const acct_mon_error_t read_rc = read_snapshot_with_retry(mon_ctx, index, &snapshot);
        if (read_rc == ACCT_MON_ERR_NOT_FOUND) {
            continue;
        }
        if (read_rc != ACCT_MON_OK) {
            std::fprintf(stderr, "read index=%u failed: %s\n", index, acct_orders_mon_strerror(read_rc));
            continue;
        }
        print_snapshot(snapshot);
    }

    (void)acct_orders_mon_close(mon_ctx);
    std::printf("\n=== 示例完成 ===\n");
    return 0;
}
