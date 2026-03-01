#include "api/position_monitor_api.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "common/constants.hpp"
#include "portfolio/positions.h"
#include "shm/shm_layout.hpp"

namespace {

static_assert(ACCT_POS_MON_POSITION_ID_LEN == acct_service::kSecurityIdSize, "position id size mismatch");
static_assert(ACCT_POS_MON_POSITION_NAME_LEN == acct_service::kSecurityIdSize, "position name size mismatch");

constexpr const char* kDefaultPositionsShmName = "/positions_shm";
constexpr std::size_t kMaxSecurityPositions = acct_service::kMaxPositions - kFirstSecurityPositionIndex;

// 将共享内存中的证券行数量裁剪到 ABI 可表示范围。
uint32_t clamp_position_count(std::size_t raw_count) {
    if (raw_count > kMaxSecurityPositions) {
        return static_cast<uint32_t>(kMaxSecurityPositions);
    }
    return static_cast<uint32_t>(raw_count);
}

// 解析 open 参数并补齐默认值。
bool resolve_options(const acct_positions_mon_options_t* options, std::string& positions_shm_name) {
    positions_shm_name = kDefaultPositionsShmName;
    if (options != nullptr && options->positions_shm_name != nullptr && options->positions_shm_name[0] != '\0') {
        positions_shm_name = options->positions_shm_name;
    }
    return !positions_shm_name.empty();
}

// 校验持仓共享内存头部，确保读者视图与当前布局一致。
bool validate_header(const acct_service::positions_shm_layout* shm) noexcept {
    if (shm == nullptr) {
        return false;
    }

    const acct_service::positions_header& header = shm->header;
    if (header.magic != acct_service::positions_header::kMagic) {
        return false;
    }
    if (header.version != acct_service::positions_header::kVersion) {
        return false;
    }
    if (header.header_size != static_cast<uint32_t>(sizeof(acct_service::positions_header))) {
        return false;
    }
    if (header.total_size != static_cast<uint32_t>(sizeof(acct_service::positions_shm_layout))) {
        return false;
    }
    if (header.capacity != static_cast<uint32_t>(acct_service::kMaxPositions)) {
        return false;
    }
    return header.init_state == 1U;
}

// 抓取稳定资金行快照：仅在行锁前后都为未锁定时返回成功。
bool try_read_stable_fund_snapshot(
    const acct_service::positions_shm_layout* shm, acct_positions_mon_fund_snapshot_t& out_snapshot) {
    const position& fund_row = shm->positions[kFundPositionIndex];
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        if (fund_row.locked.load(std::memory_order_acquire) != 0U) {
            continue;
        }

        out_snapshot = acct_positions_mon_fund_snapshot_t{};
        out_snapshot.last_update_ns = shm->header.last_update;

        std::memcpy(out_snapshot.id, fund_row.id.data, sizeof(out_snapshot.id));
        std::memcpy(out_snapshot.name, fund_row.name.data, sizeof(out_snapshot.name));

        out_snapshot.total_asset = fund_total_asset_field(fund_row);
        out_snapshot.available = fund_available_field(fund_row);
        out_snapshot.frozen = fund_frozen_field(fund_row);
        out_snapshot.market_value = fund_market_value_field(fund_row);
        out_snapshot.count_order = fund_row.count_order;

        std::atomic_thread_fence(std::memory_order_acquire);
        if (fund_row.locked.load(std::memory_order_acquire) == 0U) {
            return true;
        }
    }
    return false;
}

// 抓取稳定证券行快照：返回逻辑索引对应的一致视图。
bool try_read_stable_position_snapshot(
    const acct_service::positions_shm_layout* shm, uint32_t index, acct_positions_mon_position_snapshot_t& out_snapshot) {
    const uint32_t row_index = index + static_cast<uint32_t>(kFirstSecurityPositionIndex);
    if (row_index >= acct_service::kMaxPositions) {
        return false;
    }

    const position& row = shm->positions[row_index];
    for (uint32_t attempt = 0; attempt < 32; ++attempt) {
        if (row.locked.load(std::memory_order_acquire) != 0U) {
            continue;
        }

        out_snapshot = acct_positions_mon_position_snapshot_t{};
        out_snapshot.index = index;
        out_snapshot.row_index = row_index;
        out_snapshot.last_update_ns = shm->header.last_update;

        std::memcpy(out_snapshot.id, row.id.data, sizeof(out_snapshot.id));
        std::memcpy(out_snapshot.name, row.name.data, sizeof(out_snapshot.name));

        out_snapshot.available = row.available;
        out_snapshot.volume_available_t0 = row.volume_available_t0;
        out_snapshot.volume_available_t1 = row.volume_available_t1;
        out_snapshot.volume_buy = row.volume_buy;
        out_snapshot.dvalue_buy = row.dvalue_buy;
        out_snapshot.volume_buy_traded = row.volume_buy_traded;
        out_snapshot.dvalue_buy_traded = row.dvalue_buy_traded;
        out_snapshot.volume_sell = row.volume_sell;
        out_snapshot.dvalue_sell = row.dvalue_sell;
        out_snapshot.volume_sell_traded = row.volume_sell_traded;
        out_snapshot.dvalue_sell_traded = row.dvalue_sell_traded;
        out_snapshot.count_order = row.count_order;

        std::atomic_thread_fence(std::memory_order_acquire);
        if (row.locked.load(std::memory_order_acquire) == 0U) {
            return !row.id.empty();
        }
    }
    return false;
}

}  // namespace

struct acct_positions_monitor_context {
    acct_positions_monitor_context() = default;

    int fd = -1;
    const acct_service::positions_shm_layout* positions_shm = nullptr;
    std::string positions_shm_name;
    bool initialized = false;

    // 释放 mmap 和 fd 资源，保证异常路径/提前返回不泄漏句柄。
    ~acct_positions_monitor_context() noexcept {
        if (positions_shm != nullptr) {
            (void)munmap(const_cast<acct_service::positions_shm_layout*>(positions_shm),
                         sizeof(acct_service::positions_shm_layout));
        }
        if (fd >= 0) {
            (void)::close(fd);
        }
    }

    acct_positions_monitor_context(const acct_positions_monitor_context&) = delete;
    acct_positions_monitor_context& operator=(const acct_positions_monitor_context&) = delete;
};

extern "C" {

// 打开持仓监控上下文，并校验共享内存布局兼容性。
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_open(
    const acct_positions_mon_options_t* options, acct_positions_mon_ctx_t* out_ctx) {
    if (out_ctx == nullptr) {
        return ACCT_POS_MON_ERR_INVALID_PARAM;
    }
    *out_ctx = nullptr;

    std::string positions_shm_name;
    if (!resolve_options(options, positions_shm_name)) {
        return ACCT_POS_MON_ERR_INVALID_PARAM;
    }

    auto ctx = std::unique_ptr<acct_positions_monitor_context>(new (std::nothrow) acct_positions_monitor_context());
    if (!ctx) {
        return ACCT_POS_MON_ERR_INTERNAL;
    }

    ctx->fd = shm_open(positions_shm_name.c_str(), O_RDONLY | O_CLOEXEC, 0644);
    if (ctx->fd < 0) {
        return ACCT_POS_MON_ERR_SHM_FAILED;
    }

    struct stat st {};
    if (fstat(ctx->fd, &st) < 0) {
        return ACCT_POS_MON_ERR_SHM_FAILED;
    }
    if (static_cast<std::size_t>(st.st_size) != sizeof(acct_service::positions_shm_layout)) {
        return ACCT_POS_MON_ERR_SHM_FAILED;
    }

    void* ptr = mmap(nullptr, sizeof(acct_service::positions_shm_layout), PROT_READ, MAP_SHARED, ctx->fd, 0);
    if (ptr == MAP_FAILED) {
        return ACCT_POS_MON_ERR_SHM_FAILED;
    }

    ctx->positions_shm = static_cast<const acct_service::positions_shm_layout*>(ptr);
    if (!validate_header(ctx->positions_shm)) {
        return ACCT_POS_MON_ERR_SHM_FAILED;
    }

    ctx->positions_shm_name = positions_shm_name;
    ctx->initialized = true;
    *out_ctx = ctx.release();
    return ACCT_POS_MON_OK;
}

// 关闭持仓监控上下文并释放资源。
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_close(acct_positions_mon_ctx_t ctx) {
    if (ctx == nullptr) {
        return ACCT_POS_MON_ERR_INVALID_PARAM;
    }
    auto* context = ctx;
    delete context;
    return ACCT_POS_MON_OK;
}

// 获取持仓共享内存头部信息，供外部轮询使用。
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_info(
    acct_positions_mon_ctx_t ctx, acct_positions_mon_info_t* out_info) {
    if (ctx == nullptr || out_info == nullptr) {
        return ACCT_POS_MON_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized || context->positions_shm == nullptr) {
        return ACCT_POS_MON_ERR_NOT_INITIALIZED;
    }

    const acct_service::positions_header& header = context->positions_shm->header;
    out_info->magic = header.magic;
    out_info->version = header.version;
    out_info->capacity = header.capacity;
    out_info->init_state = header.init_state;
    out_info->position_count =
        clamp_position_count(context->positions_shm->position_count.load(std::memory_order_acquire));
    out_info->next_security_id = header.id.load(std::memory_order_acquire);
    out_info->create_time_ns = header.create_time;
    out_info->last_update_ns = header.last_update;
    return ACCT_POS_MON_OK;
}

// 读取资金行快照。
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_read_fund(
    acct_positions_mon_ctx_t ctx, acct_positions_mon_fund_snapshot_t* out_snapshot) {
    if (ctx == nullptr || out_snapshot == nullptr) {
        return ACCT_POS_MON_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized || context->positions_shm == nullptr) {
        return ACCT_POS_MON_ERR_NOT_INITIALIZED;
    }

    if (!try_read_stable_fund_snapshot(context->positions_shm, *out_snapshot)) {
        return ACCT_POS_MON_ERR_RETRY;
    }
    return ACCT_POS_MON_OK;
}

// 按证券逻辑索引读取单条证券行快照。
ACCT_POS_MON_API acct_pos_mon_error_t acct_positions_mon_read_position(
    acct_positions_mon_ctx_t ctx, uint32_t index, acct_positions_mon_position_snapshot_t* out_snapshot) {
    if (ctx == nullptr || out_snapshot == nullptr) {
        return ACCT_POS_MON_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized || context->positions_shm == nullptr) {
        return ACCT_POS_MON_ERR_NOT_INITIALIZED;
    }

    const uint32_t count = clamp_position_count(context->positions_shm->position_count.load(std::memory_order_acquire));
    if (index >= count) {
        return ACCT_POS_MON_ERR_NOT_FOUND;
    }

    if (!try_read_stable_position_snapshot(context->positions_shm, index, *out_snapshot)) {
        return ACCT_POS_MON_ERR_RETRY;
    }
    return ACCT_POS_MON_OK;
}

// 将错误码转换为可读字符串，便于日志与脚本输出。
ACCT_POS_MON_API const char* acct_positions_mon_strerror(acct_pos_mon_error_t err) {
    switch (err) {
        case ACCT_POS_MON_OK:
            return "Success";
        case ACCT_POS_MON_ERR_NOT_INITIALIZED:
            return "Context not initialized";
        case ACCT_POS_MON_ERR_INVALID_PARAM:
            return "Invalid parameter";
        case ACCT_POS_MON_ERR_SHM_FAILED:
            return "Shared memory operation failed";
        case ACCT_POS_MON_ERR_NOT_FOUND:
            return "Position index not found";
        case ACCT_POS_MON_ERR_RETRY:
            return "Snapshot not stable, retry";
        case ACCT_POS_MON_ERR_INTERNAL:
            return "Internal error";
        default:
            return "Unknown error";
    }
}

}  // extern "C"
