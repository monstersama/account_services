#include "api/order_api.h"
#include "common/constants.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "shm/shm_layout.hpp"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unordered_map>

// close() 前向声明，避免 <unistd.h> 中 acct() 与 namespace acct 冲突
extern "C" int close(int __fd) noexcept;

namespace {

// 版本号
constexpr const char* kVersion = "1.0.0";

// 最大缓存订单数
constexpr std::size_t kMaxCachedOrders = 1024;

}  // namespace

// 内部上下文结构
struct acct_context {
    acct::upstream_shm_layout* shm_ptr = nullptr;
    int shm_fd = -1;
    std::size_t shm_size = 0;
    std::atomic<uint32_t> next_order_id{1};

    // 缓存的订单（new_order 创建，send_order 发送）
    std::unordered_map<uint32_t, order_request> cached_orders;

    bool initialized = false;
};

extern "C" {

ACCT_API acct_ctx_t acct_init(const char* shm_name) {
    if (!shm_name || shm_name[0] == '\0') {
        return nullptr;
    }

    auto* ctx = new (std::nothrow) acct_context();
    if (!ctx) {
        return nullptr;
    }

    // 打开共享内存（只读写模式，假设账户服务已创建）
    ctx->shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (ctx->shm_fd < 0) {
        delete ctx;
        return nullptr;
    }

    ctx->shm_size = sizeof(acct::upstream_shm_layout);

    // 映射共享内存
    void* ptr = mmap(nullptr, ctx->shm_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, ctx->shm_fd, 0);
    if (ptr == MAP_FAILED) {
        close(ctx->shm_fd);
        delete ctx;
        return nullptr;
    }

    ctx->shm_ptr = static_cast<acct::upstream_shm_layout*>(ptr);

    // 验证魔数
    if (ctx->shm_ptr->header.magic != acct::shm_header::kMagic) {
        munmap(ptr, ctx->shm_size);
        close(ctx->shm_fd);
        delete ctx;
        return nullptr;
    }

    ctx->initialized = true;
    return ctx;
}

ACCT_API void acct_destroy(acct_ctx_t ctx) {
    if (!ctx) {
        return;
    }

    auto* context = ctx;

    if (context->shm_ptr) {
        munmap(context->shm_ptr, context->shm_size);
    }

    if (context->shm_fd >= 0) {
        close(context->shm_fd);
    }

    delete context;
}

ACCT_API acct_error_t acct_new_order(
    acct_ctx_t ctx,
    const char* security_id,
    uint16_t internal_security_id,
    uint8_t side,
    uint8_t market,
    uint64_t volume,
    uint64_t price,
    uint32_t md_time,
    uint32_t* out_order_id
) {
    if (!ctx || !security_id || !out_order_id) {
        return ACCT_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized) {
        return ACCT_ERR_NOT_INITIALIZED;
    }

    // 验证参数
    if (side != ACCT_SIDE_BUY && side != ACCT_SIDE_SELL) {
        return ACCT_ERR_INVALID_PARAM;
    }
    if (market < ACCT_MARKET_SZ || market > ACCT_MARKET_HK) {
        return ACCT_ERR_INVALID_PARAM;
    }
    if (volume == 0) {
        return ACCT_ERR_INVALID_PARAM;
    }

    // 检查缓存容量
    if (context->cached_orders.size() >= kMaxCachedOrders) {
        return ACCT_ERR_INTERNAL;
    }

    // 分配订单ID
    uint32_t order_id = context->next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 创建订单请求
    order_request request;
    request.init_new(
        std::string_view(security_id),
        static_cast<internal_security_id_t>(internal_security_id),
        static_cast<internal_order_id_t>(order_id),
        static_cast<trade_side_t>(side),
        static_cast<market_t>(market),
        static_cast<volume_t>(volume),
        static_cast<dprice_t>(price),
        static_cast<md_time_t>(md_time)
    );
    request.order_status.store(order_status_t::NotSet, std::memory_order_relaxed);

    // 缓存订单
    context->cached_orders[order_id] = request;

    *out_order_id = order_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_send_order(acct_ctx_t ctx, uint32_t order_id) {
    if (!ctx) {
        return ACCT_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized) {
        return ACCT_ERR_NOT_INITIALIZED;
    }

    // 查找缓存的订单
    auto it = context->cached_orders.find(order_id);
    if (it == context->cached_orders.end()) {
        return ACCT_ERR_ORDER_NOT_FOUND;
    }

    // 更新状态
    it->second.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 推入队列
    bool success = context->shm_ptr->strategy_order_queue.try_push(it->second);
    if (!success) {
        return ACCT_ERR_QUEUE_FULL;
    }

    // 从缓存中移除
    context->cached_orders.erase(it);

    return ACCT_OK;
}

ACCT_API acct_error_t acct_submit_order(
    acct_ctx_t ctx,
    const char* security_id,
    uint16_t internal_security_id,
    uint8_t side,
    uint8_t market,
    uint64_t volume,
    uint64_t price,
    uint32_t md_time,
    uint32_t* out_order_id
) {
    if (!ctx || !security_id || !out_order_id) {
        return ACCT_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized) {
        return ACCT_ERR_NOT_INITIALIZED;
    }

    // 验证参数
    if (side != ACCT_SIDE_BUY && side != ACCT_SIDE_SELL) {
        return ACCT_ERR_INVALID_PARAM;
    }
    if (market < ACCT_MARKET_SZ || market > ACCT_MARKET_HK) {
        return ACCT_ERR_INVALID_PARAM;
    }
    if (volume == 0) {
        return ACCT_ERR_INVALID_PARAM;
    }

    // 分配订单ID
    uint32_t order_id = context->next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 创建订单请求
    order_request request;
    request.init_new(
        std::string_view(security_id),
        static_cast<internal_security_id_t>(internal_security_id),
        static_cast<internal_order_id_t>(order_id),
        static_cast<trade_side_t>(side),
        static_cast<market_t>(market),
        static_cast<volume_t>(volume),
        static_cast<dprice_t>(price),
        static_cast<md_time_t>(md_time)
    );
    request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 直接推入队列
    bool success = context->shm_ptr->strategy_order_queue.try_push(request);
    if (!success) {
        return ACCT_ERR_QUEUE_FULL;
    }

    *out_order_id = order_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_cancel_order(
    acct_ctx_t ctx,
    uint32_t orig_order_id,
    uint32_t md_time,
    uint32_t* out_cancel_id
) {
    if (!ctx || !out_cancel_id) {
        return ACCT_ERR_INVALID_PARAM;
    }

    auto* context = ctx;
    if (!context->initialized) {
        return ACCT_ERR_NOT_INITIALIZED;
    }

    // 分配撤单请求ID
    uint32_t cancel_id = context->next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 创建撤单请求
    order_request request;
    request.init_cancel(
        static_cast<internal_order_id_t>(cancel_id),
        static_cast<md_time_t>(md_time),
        static_cast<internal_order_id_t>(orig_order_id)
    );
    request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 推入队列
    bool success = context->shm_ptr->strategy_order_queue.try_push(request);
    if (!success) {
        return ACCT_ERR_QUEUE_FULL;
    }

    *out_cancel_id = cancel_id;
    return ACCT_OK;
}

ACCT_API size_t acct_queue_size(acct_ctx_t ctx) {
    if (!ctx) {
        return 0;
    }

    auto* context = ctx;
    if (!context->initialized || !context->shm_ptr) {
        return 0;
    }

    return context->shm_ptr->strategy_order_queue.size();
}

ACCT_API const char* acct_strerror(acct_error_t err) {
    switch (err) {
        case ACCT_OK:                   return "Success";
        case ACCT_ERR_NOT_INITIALIZED:  return "Context not initialized";
        case ACCT_ERR_INVALID_PARAM:    return "Invalid parameter";
        case ACCT_ERR_QUEUE_FULL:       return "Queue is full";
        case ACCT_ERR_SHM_FAILED:       return "Shared memory operation failed";
        case ACCT_ERR_ORDER_NOT_FOUND:  return "Order not found";
        case ACCT_ERR_INTERNAL:         return "Internal error";
        default:                        return "Unknown error";
    }
}

ACCT_API const char* acct_version(void) {
    return kVersion;
}

}  // extern "C"
