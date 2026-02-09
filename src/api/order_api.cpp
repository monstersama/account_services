#include "api/order_api.h"

#include <fcntl.h>
#include <sys/mman.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include "common/constants.hpp"
#include "common/error.hpp"
#include "common/log.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "shm/shm_layout.hpp"
#include "version.h"

// 前向声明，避免 <unistd.h> 中 acct() 与 namespace acct 冲突
extern "C" {
int close(int __fd) noexcept;
int ftruncate(int __fd, long __length) noexcept;
}

namespace {

// 最大缓存订单数
constexpr std::size_t kMaxCachedOrders = 1024;

// 获取当前系统时间的 md_time（HHMMSSmmm 格式）
inline acct_service::md_time_t get_current_md_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    uint32_t hours = tm_info.tm_hour;
    uint32_t minutes = tm_info.tm_min;
    uint32_t seconds = tm_info.tm_sec;
    uint32_t milliseconds = static_cast<uint32_t>(ts.tv_nsec / 1000000);
    return hours * 10000000 + minutes * 100000 + seconds * 1000 + milliseconds;
}

acct_error_t api_error(acct_error_t rc, acct_service::error_code code, std::string_view message, int sys_errno = 0) {
    acct_service::error_status status = ACCT_MAKE_ERROR(acct_service::error_domain::api, code, "order_api", message, sys_errno);
    acct_service::record_error(status);
    ACCT_LOG_ERROR_STATUS(status);
    return rc;
}

}  // namespace

using namespace acct_service;

// 内部上下文结构
struct acct_context {
    acct_service::upstream_shm_layout* shm_ptr = nullptr;
    int shm_fd = -1;
    std::size_t shm_size = 0;
    // next_order_id 现在存储在共享内存 header 中

    // 缓存的订单（new_order 创建，send_order 发送）
    std::unordered_map<uint32_t, acct_service::order_request> cached_orders;

    bool initialized = false;
};

extern "C" {

ACCT_API acct_error_t acct_init(acct_ctx_t* out_ctx) {
    if (!out_ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_init out_ctx is null");
    }
    *out_ctx = nullptr;

    auto* ctx = new (std::nothrow) acct_context();
    if (!ctx) {
        return api_error(ACCT_ERR_INTERNAL, error_code::InternalError, "acct_context allocation failed");
    }

    ctx->shm_size = sizeof(acct_service::upstream_shm_layout);

    // 尝试打开已存在的共享内存
    ctx->shm_fd = shm_open(acct_service::kStrategyOrderShmName, O_RDWR, 0666);
    bool is_new = false;

    if (ctx->shm_fd < 0) {
        // 不存在则创建
        ctx->shm_fd = shm_open(acct_service::kStrategyOrderShmName, O_CREAT | O_RDWR, 0666);
        if (ctx->shm_fd < 0) {
            delete ctx;
            return api_error(ACCT_ERR_SHM_FAILED, error_code::ShmOpenFailed, "acct_init shm_open create failed", errno);
        }
        is_new = true;

        // 设置共享内存大小
        if (ftruncate(ctx->shm_fd, ctx->shm_size) < 0) {
            close(ctx->shm_fd);
            shm_unlink(acct_service::kStrategyOrderShmName);
            delete ctx;
            return api_error(ACCT_ERR_SHM_FAILED, error_code::ShmResizeFailed, "acct_init ftruncate failed", errno);
        }
    }

    // 映射共享内存
    void* ptr = mmap(nullptr, ctx->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->shm_fd, 0);
    if (ptr == MAP_FAILED) {
        close(ctx->shm_fd);
        if (is_new) {
            shm_unlink(acct_service::kStrategyOrderShmName);
        }
        delete ctx;
        return api_error(ACCT_ERR_SHM_FAILED, error_code::ShmMmapFailed, "acct_init mmap failed", errno);
    }

    ctx->shm_ptr = static_cast<acct_service::upstream_shm_layout*>(ptr);

    // 新创建的共享内存需要初始化
    if (is_new) {
        ctx->shm_ptr->header.magic = acct_service::shm_header::kMagic;
        ctx->shm_ptr->header.version = acct_service::shm_header::kVersion;
        ctx->shm_ptr->header.create_time = 0;
        ctx->shm_ptr->header.last_update = 0;
        ctx->shm_ptr->header.next_order_id.store(1, std::memory_order_relaxed);
        // 队列构造时已自动初始化
    } else {
        // 验证魔数
        if (ctx->shm_ptr->header.magic != acct_service::shm_header::kMagic) {
            munmap(ptr, ctx->shm_size);
            close(ctx->shm_fd);
            delete ctx;
            return api_error(ACCT_ERR_SHM_FAILED, error_code::ShmHeaderInvalid, "acct_init shm magic invalid");
        }
        // 验证版本
        if (ctx->shm_ptr->header.version != acct_service::shm_header::kVersion) {
            munmap(ptr, ctx->shm_size);
            close(ctx->shm_fd);
            delete ctx;
            return api_error(ACCT_ERR_SHM_FAILED, error_code::ShmHeaderInvalid, "acct_init shm version invalid");
        }
    }

    ctx->initialized = true;
    *out_ctx = ctx;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_destroy(acct_ctx_t ctx) {
    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_destroy ctx is null");
    }

    auto* context = ctx;

    if (context->shm_ptr) {
        munmap(context->shm_ptr, context->shm_size);
    }

    if (context->shm_fd >= 0) {
        close(context->shm_fd);
    }

    delete context;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_new_order(acct_ctx_t ctx, const char* security_id, uint8_t side, uint8_t market,
    uint64_t volume, double price, uint32_t valid_sec, uint32_t* out_order_id) {
    if (!out_order_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_new_order out_order_id is null");
    }
    *out_order_id = 0;

    if (!ctx || !security_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_new_order invalid ctx/security_id");
    }

    auto* context = ctx;
    if (!context->initialized) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, error_code::InvalidState, "acct_new_order called before init");
    }

    // 验证参数
    if (side != ACCT_SIDE_BUY && side != ACCT_SIDE_SELL) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_new_order invalid side");
    }
    if (market < ACCT_MARKET_SZ || market > ACCT_MARKET_HK) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_new_order invalid market");
    }
    if (volume == 0) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_new_order zero volume");
    }

    // 检查缓存容量
    if (context->cached_orders.size() >= kMaxCachedOrders) {
        return api_error(ACCT_ERR_CACHE_FULL, error_code::QueueFull, "acct_new_order cache full");
    }

    // 分配订单ID（从共享内存 header 中原子递增）
    uint32_t order_id = context->shm_ptr->header.next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 将浮点价格转换为内部整数格式（元 -> 分，乘以 100）
    acct_service::dprice_t internal_price = static_cast<acct_service::dprice_t>(price * 100.0 + 0.5);

    // 使用当前系统时间作为 md_time（HHMMSSmmm 格式）
    acct_service::md_time_t md_time = get_current_md_time();

    // valid_sec 参数暂时不使用（为避免未使用警告）
    (void)valid_sec;

    // 创建订单请求（internal_security_id 内部实现，暂时使用0）
    acct_service::order_request request;
    request.init_new(std::string_view(security_id),
        static_cast<acct_service::internal_security_id_t>(0),  // 内部实现，暂时使用0
        static_cast<acct_service::internal_order_id_t>(order_id), static_cast<acct_service::trade_side_t>(side),
        static_cast<acct_service::market_t>(market), static_cast<acct_service::volume_t>(volume), internal_price,
        md_time);
    request.order_status.store(acct_service::order_status_t::NotSet, std::memory_order_relaxed);

    // 缓存订单
    context->cached_orders[order_id] = request;

    *out_order_id = order_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_send_order(acct_ctx_t ctx, uint32_t order_id) {
    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_send_order ctx is null");
    }

    auto* context = ctx;
    if (!context->initialized) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, error_code::InvalidState, "acct_send_order called before init");
    }

    // 查找缓存的订单
    auto it = context->cached_orders.find(order_id);
    if (it == context->cached_orders.end()) {
        return api_error(ACCT_ERR_ORDER_NOT_FOUND, error_code::OrderNotFound, "acct_send_order order not cached");
    }

    // 更新状态
    it->second.order_status.store(acct_service::order_status_t::StrategySubmitted, std::memory_order_release);

    // 推入队列
    bool success = context->shm_ptr->strategy_order_queue.try_push(it->second);
    if (!success) {
        return api_error(ACCT_ERR_QUEUE_FULL, error_code::QueuePushFailed, "acct_send_order queue push failed");
    }

    // 从缓存中移除
    context->cached_orders.erase(it);

    return ACCT_OK;
}

ACCT_API acct_error_t acct_submit_order(acct_ctx_t ctx, const char* security_id, uint8_t side, uint8_t market,
    uint64_t volume, double price, uint32_t valid_sec, uint32_t* out_order_id) {
    if (!out_order_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_submit_order out_order_id is null");
    }
    *out_order_id = 0;

    if (!ctx || !security_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_submit_order invalid ctx/security_id");
    }

    auto* context = ctx;
    if (!context->initialized) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, error_code::InvalidState, "acct_submit_order called before init");
    }

    // 验证参数
    if (side != ACCT_SIDE_BUY && side != ACCT_SIDE_SELL) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_submit_order invalid side");
    }
    if (market < ACCT_MARKET_SZ || market > ACCT_MARKET_HK) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_submit_order invalid market");
    }
    if (volume == 0) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_submit_order zero volume");
    }

    // 分配订单ID（从共享内存 header 中原子递增）
    uint32_t order_id = context->shm_ptr->header.next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 将浮点价格转换为内部整数格式（元 -> 分，乘以 100）
    dprice_t internal_price = static_cast<dprice_t>(price * 100.0 + 0.5);

    // 使用当前系统时间作为 md_time（HHMMSSmmm 格式）
    md_time_t md_time = get_current_md_time();

    // valid_sec 参数暂时不使用（为避免未使用警告）
    (void)valid_sec;

    // 创建订单请求（internal_security_id 内部实现，暂时使用0）
    order_request request;
    request.init_new(std::string_view(security_id),
        static_cast<internal_security_id_t>(0),  // 内部实现，暂时使用0
        static_cast<internal_order_id_t>(order_id), static_cast<trade_side_t>(side), static_cast<market_t>(market),
        static_cast<volume_t>(volume), internal_price, md_time);
    request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 直接推入队列
    bool success = context->shm_ptr->strategy_order_queue.try_push(request);
    if (!success) {
        return api_error(ACCT_ERR_QUEUE_FULL, error_code::QueuePushFailed, "acct_submit_order queue push failed");
    }

    *out_order_id = order_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_cancel_order(
    acct_ctx_t ctx, uint32_t orig_order_id, uint32_t valid_sec, uint32_t* out_cancel_id) {
    if (!out_cancel_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_cancel_order out_cancel_id is null");
    }
    *out_cancel_id = 0;

    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_cancel_order ctx is null");
    }

    auto* context = ctx;
    if (!context->initialized) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, error_code::InvalidState, "acct_cancel_order called before init");
    }

    // 分配撤单请求ID（从共享内存 header 中原子递增）
    uint32_t cancel_id = context->shm_ptr->header.next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 使用当前系统时间作为 md_time（HHMMSSmmm 格式）
    md_time_t md_time = get_current_md_time();

    // valid_sec 参数暂时不使用（为避免未使用警告）
    (void)valid_sec;

    // 创建撤单请求
    order_request request;
    request.init_cancel(
        static_cast<internal_order_id_t>(cancel_id), md_time, static_cast<internal_order_id_t>(orig_order_id));
    request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 推入队列
    bool success = context->shm_ptr->strategy_order_queue.try_push(request);
    if (!success) {
        return api_error(ACCT_ERR_QUEUE_FULL, error_code::QueuePushFailed, "acct_cancel_order queue push failed");
    }

    *out_cancel_id = cancel_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_queue_size(acct_ctx_t ctx, size_t* out_size) {
    if (!out_size) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_queue_size out_size is null");
    }
    *out_size = 0;

    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, error_code::InvalidParam, "acct_queue_size ctx is null");
    }

    auto* context = ctx;
    if (!context->initialized || !context->shm_ptr) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, error_code::InvalidState, "acct_queue_size called before init");
    }

    *out_size = context->shm_ptr->strategy_order_queue.size();
    return ACCT_OK;
}

ACCT_API const char* acct_strerror(acct_error_t err) {
    switch (err) {
        case ACCT_OK:
            return "Success";
        case ACCT_ERR_NOT_INITIALIZED:
            return "Context not initialized";
        case ACCT_ERR_INVALID_PARAM:
            return "Invalid parameter";
        case ACCT_ERR_QUEUE_FULL:
            return "Queue is full";
        case ACCT_ERR_SHM_FAILED:
            return "Shared memory operation failed";
        case ACCT_ERR_ORDER_NOT_FOUND:
            return "Order not found";
        case ACCT_ERR_CACHE_FULL:
            return "Order cache is full";
        case ACCT_ERR_INTERNAL:
            return "Internal error";
        default:
            return "Unknown error";
    }
}

ACCT_API const char* acct_version(void) { return ACCT_API_VERSION; }

ACCT_API acct_error_t acct_cleanup_shm(void) {
    if (shm_unlink(acct_service::kStrategyOrderShmName) < 0) {
        // 如果共享内存不存在，返回成功（幂等操作）
        if (errno == ENOENT) {
            return ACCT_OK;
        }
        return api_error(ACCT_ERR_SHM_FAILED, error_code::ShmOpenFailed, "acct_cleanup_shm failed", errno);
    }
    return ACCT_OK;
}

}  // extern "C"
