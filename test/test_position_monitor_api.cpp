#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

#include "api/position_monitor_api.h"
#include "portfolio/position_manager.hpp"
#include "shm/shm_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

// 生成唯一 SHM 名称，避免并发测试冲突。
std::string unique_shm_name(const char* prefix) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return std::string("/") + prefix + "_" + std::to_string(static_cast<unsigned long long>(now_ns));
}

// 重试读取资金行，屏蔽短暂并发冲突。
acct_pos_mon_error_t read_fund_with_retry(
    acct_positions_mon_ctx_t mon_ctx, acct_positions_mon_fund_snapshot_t* out_snapshot) {
    acct_pos_mon_error_t rc = ACCT_POS_MON_ERR_RETRY;
    for (int i = 0; i < 16 && rc == ACCT_POS_MON_ERR_RETRY; ++i) {
        rc = acct_positions_mon_read_fund(mon_ctx, out_snapshot);
    }
    return rc;
}

// 重试读取证券行，屏蔽短暂并发冲突。
acct_pos_mon_error_t read_position_with_retry(
    acct_positions_mon_ctx_t mon_ctx, uint32_t index, acct_positions_mon_position_snapshot_t* out_snapshot) {
    acct_pos_mon_error_t rc = ACCT_POS_MON_ERR_RETRY;
    for (int i = 0; i < 16 && rc == ACCT_POS_MON_ERR_RETRY; ++i) {
        rc = acct_positions_mon_read_position(mon_ctx, index, out_snapshot);
    }
    return rc;
}

}  // namespace

TEST(strerror) {
    // 校验关键错误码字符串，保证对外诊断可读。
    assert(std::strcmp(acct_positions_mon_strerror(ACCT_POS_MON_OK), "Success") == 0);
    assert(std::strcmp(acct_positions_mon_strerror(ACCT_POS_MON_ERR_NOT_FOUND), "Position index not found") == 0);
}

TEST(open_info_read_close) {
    // 准备可读的持仓 SHM，并写入一条证券仓位。
    const std::string shm_name = unique_shm_name("acct_positions_mon");
    SHMManager writer_manager;
    positions_shm_layout* positions_shm = writer_manager.open_positions(shm_name, shm_mode::Create, 1001);
    assert(positions_shm != nullptr);

    position_manager positions(positions_shm);
    assert(positions.initialize(1001));
    const internal_security_id_t sec_id = positions.add_security("000001", "PingAn", market_t::SZ);
    assert(sec_id == std::string_view("SZ.000001"));
    assert(positions.add_position(sec_id, 300, 1000, 5001));

    // 打开监控并读取头部信息。
    acct_positions_mon_options_t options{};
    options.positions_shm_name = shm_name.c_str();

    acct_positions_mon_ctx_t mon_ctx = nullptr;
    assert(acct_positions_mon_open(&options, &mon_ctx) == ACCT_POS_MON_OK);
    assert(mon_ctx != nullptr);

    acct_positions_mon_info_t info{};
    assert(acct_positions_mon_info(mon_ctx, &info) == ACCT_POS_MON_OK);
    assert(info.capacity == kMaxPositions);
    assert(info.position_count >= 1);

    // 读取资金行并校验关键字段。
    acct_positions_mon_fund_snapshot_t fund{};
    assert(read_fund_with_retry(mon_ctx, &fund) == ACCT_POS_MON_OK);
    assert(std::strncmp(fund.id, "FUND", 4) == 0);
    assert(fund.total_asset > 0);

    // 读取首条证券行并校验新增持仓可见。
    acct_positions_mon_position_snapshot_t snapshot{};
    assert(read_position_with_retry(mon_ctx, 0, &snapshot) == ACCT_POS_MON_OK);
    assert(snapshot.index == 0);
    assert(snapshot.row_index == 1);
    assert(std::strncmp(snapshot.id, "SZ.000001", 9) == 0);
    assert(snapshot.volume_buy_traded >= 300);
    assert(snapshot.volume_available_t1 >= 300);

    assert(acct_positions_mon_close(mon_ctx) == ACCT_POS_MON_OK);
    writer_manager.close();
    (void)SHMManager::unlink(shm_name);
}

TEST(read_not_found) {
    // 无证券行时，读取 index=0 应返回 NOT_FOUND。
    const std::string shm_name = unique_shm_name("acct_positions_mon_nf");
    SHMManager writer_manager;
    positions_shm_layout* positions_shm = writer_manager.open_positions(shm_name, shm_mode::Create, 1002);
    assert(positions_shm != nullptr);

    position_manager positions(positions_shm);
    assert(positions.initialize(1002));

    acct_positions_mon_options_t options{};
    options.positions_shm_name = shm_name.c_str();

    acct_positions_mon_ctx_t mon_ctx = nullptr;
    assert(acct_positions_mon_open(&options, &mon_ctx) == ACCT_POS_MON_OK);
    assert(mon_ctx != nullptr);

    acct_positions_mon_position_snapshot_t snapshot{};
    assert(acct_positions_mon_read_position(mon_ctx, 0, &snapshot) == ACCT_POS_MON_ERR_NOT_FOUND);

    assert(acct_positions_mon_close(mon_ctx) == ACCT_POS_MON_OK);
    writer_manager.close();
    (void)SHMManager::unlink(shm_name);
}

int main() {
    printf("=== Position Monitor API Test Suite ===\n\n");

    RUN_TEST(strerror);
    RUN_TEST(open_info_read_close);
    RUN_TEST(read_not_found);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
