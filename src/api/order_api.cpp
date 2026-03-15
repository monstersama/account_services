#include "api/order_api.h"

#include <sys/mman.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>

#include "common/constants.hpp"
#include "common/error.hpp"
#include "common/log.hpp"
#include "common/security_identity.hpp"
#include "order/order_request.hpp"
#include "shm/orders_shm.hpp"
#include "shm/shm_manager.hpp"
#include "version.h"

namespace {

// 最大缓存订单数
constexpr std::size_t kMaxCachedOrders = 1024;

// 获取当前系统时间的 md_time（HHMMSSmmm 格式）
inline acct_service::MdTime get_current_md_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    const uint32_t hours = static_cast<uint32_t>(tm_info.tm_hour);
    const uint32_t minutes = static_cast<uint32_t>(tm_info.tm_min);
    const uint32_t seconds = static_cast<uint32_t>(tm_info.tm_sec);
    const uint32_t milliseconds = static_cast<uint32_t>(ts.tv_nsec / 1000000);
    return hours * 10000000 + minutes * 100000 + seconds * 1000 + milliseconds;
}

std::string default_trading_day() {
    const char* env_day = std::getenv("ACCT_TRADING_DAY");
    if (env_day && acct_service::is_valid_trading_day(env_day)) {
        return std::string(env_day);
    }
    return "19700101";
}

acct_error_t api_error(acct_error_t rc, acct_service::ErrorCode code, std::string_view message, int sys_errno = 0) {
    acct_service::ErrorStatus status =
        ACCT_MAKE_ERROR(acct_service::ErrorDomain::api, code, "order_api", message, sys_errno);
    acct_service::record_error(status);
    ACCT_LOG_ERROR_STATUS(status);
    return rc;
}

bool resolve_init_options(const acct_init_options_t* options, std::string& upstream_name, std::string& orders_base_name,
                          std::string& trading_day, bool& create_if_not_exist) {
    upstream_name = acct_service::kUpstreamOrderShmName;
    orders_base_name = acct_service::kOrdersShmName;
    trading_day = default_trading_day();
    create_if_not_exist = true;

    if (!options) {
        return true;
    }

    if (options->upstream_shm_name && options->upstream_shm_name[0] != '\0') {
        upstream_name = options->upstream_shm_name;
    }
    if (options->orders_shm_name && options->orders_shm_name[0] != '\0') {
        orders_base_name = options->orders_shm_name;
    }
    if (options->trading_day && options->trading_day[0] != '\0') {
        trading_day = options->trading_day;
    }
    create_if_not_exist = options->create_if_not_exist != 0;

    if (upstream_name.empty() || orders_base_name.empty()) {
        return false;
    }
    if (!acct_service::is_valid_trading_day(trading_day)) {
        return false;
    }
    return true;
}

bool should_recreate_shm_on_init_failure(acct_service::ErrorCode code) {
    return code == acct_service::ErrorCode::ShmResizeFailed || code == acct_service::ErrorCode::ShmHeaderInvalid;
}

// 解析 C ABI 侧的逐单执行选项，统一转成内部被动算法枚举。
bool resolve_passive_execution_algo(const acct_order_exec_options_t* exec_options,
                                    acct_service::PassiveExecutionAlgo& out_algo) {
    out_algo = acct_service::PassiveExecutionAlgo::Default;
    if (!exec_options) {
        return true;
    }

    switch (exec_options->passive_exec_algo) {
        case ACCT_PASSIVE_EXEC_DEFAULT:
            out_algo = acct_service::PassiveExecutionAlgo::Default;
            return true;
        case ACCT_PASSIVE_EXEC_NONE:
            out_algo = acct_service::PassiveExecutionAlgo::None;
            return true;
        case ACCT_PASSIVE_EXEC_FIXED_SIZE:
            out_algo = acct_service::PassiveExecutionAlgo::FixedSize;
            return true;
        case ACCT_PASSIVE_EXEC_TWAP:
            out_algo = acct_service::PassiveExecutionAlgo::TWAP;
            return true;
        case ACCT_PASSIVE_EXEC_VWAP:
            out_algo = acct_service::PassiveExecutionAlgo::VWAP;
            return true;
        case ACCT_PASSIVE_EXEC_ICEBERG:
            out_algo = acct_service::PassiveExecutionAlgo::Iceberg;
            return true;
        default:
            return false;
    }
}

// 构造新单请求并填充统一的内部字段，避免 new/submit 两条路径重复漂移。
bool build_new_order_request(const char* security_id, uint8_t side, uint8_t market, uint64_t volume, double price,
                             acct_service::PassiveExecutionAlgo passive_execution_algo, uint32_t order_id,
                             acct_service::OrderRequest& out_request, acct_error_t& out_error) {
    using namespace acct_service;

    if (side != ACCT_SIDE_BUY && side != ACCT_SIDE_SELL) {
        out_error = api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "invalid side");
        return false;
    }
    if (market < ACCT_MARKET_SZ || market > ACCT_MARKET_HK) {
        out_error = api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "invalid market");
        return false;
    }
    if (volume == 0) {
        out_error = api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "zero volume");
        return false;
    }

    InternalSecurityId internal_security_id;
    if (!build_internal_security_id(static_cast<Market>(market), std::string_view(security_id), internal_security_id)) {
        out_error = api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "invalid security_id length/market");
        return false;
    }

    const DPrice internal_price = static_cast<DPrice>(price * 100.0 + 0.5);
    const MdTime md_time = get_current_md_time();

    out_request.init_new(std::string_view(security_id), internal_security_id, static_cast<InternalOrderId>(order_id),
                         static_cast<TradeSide>(side), static_cast<Market>(market), static_cast<Volume>(volume),
                         internal_price, md_time);
    out_request.passive_execution_algo = passive_execution_algo;
    out_error = ACCT_OK;
    return true;
}

}  // namespace

using namespace acct_service;

// 内部上下文结构
struct acct_context {
    SHMManager upstream_shm_manager;
    SHMManager orders_shm_manager;

    upstream_shm_layout* upstream_shm = nullptr;
    orders_shm_layout* orders_shm = nullptr;

    std::string upstream_shm_name;
    std::string orders_base_name;
    std::string orders_dated_name;
    std::string trading_day;

    // 缓存的订单（new_order 创建，send_order 发送）
    std::unordered_map<uint32_t, OrderRequest> cached_orders;

    bool initialized = false;
};

namespace {

acct_error_t enqueue_order(acct_context* context, const OrderRequest& request, order_slot_source_t source,
                           OrderIndex* out_index = nullptr) {
    if (!context || !context->upstream_shm || !context->orders_shm) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, ErrorCode::InvalidState, "enqueue called before init");
    }

    OrderIndex index = kInvalidOrderIndex;
    if (!orders_shm_append(context->orders_shm, request, OrderSlotState::UpstreamQueued, source, now_ns(), index)) {
        return api_error(ACCT_ERR_ORDER_POOL_FULL, ErrorCode::OrderPoolFull, "orders shm pool full");
    }

    if (!context->upstream_shm->upstream_order_queue.try_push(index)) {
        (void)orders_shm_update_stage(context->orders_shm, index, OrderSlotState::QueuePushFailed, now_ns());
        const std::size_t queue_size = context->upstream_shm->upstream_order_queue.size();
        const std::size_t queue_capacity = kUpstreamOrderQueueCapacity - 1;
        const std::string message = "enqueue upstream queue push failed: queue_size=" + std::to_string(queue_size) +
                                    "/" + std::to_string(queue_capacity);
        return api_error(ACCT_ERR_QUEUE_FULL, ErrorCode::QueuePushFailed, message);
    }

    context->upstream_shm->header.last_update = now_ns();
    if (out_index) {
        *out_index = index;
    }
    return ACCT_OK;
}

}  // namespace

extern "C" {

ACCT_API acct_error_t acct_init(acct_ctx_t* out_ctx) { return acct_init_ex(nullptr, out_ctx); }

ACCT_API acct_error_t acct_init_ex(const acct_init_options_t* options, acct_ctx_t* out_ctx) {
    if (!out_ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_init_ex out_ctx is null");
    }
    *out_ctx = nullptr;

    std::string upstream_name;
    std::string orders_base_name;
    std::string trading_day;
    bool create_if_not_exist = true;
    if (!resolve_init_options(options, upstream_name, orders_base_name, trading_day, create_if_not_exist)) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "invalid acct_init_ex options");
    }

    auto ctx = std::unique_ptr<acct_context>(new (std::nothrow) acct_context());
    if (!ctx) {
        return api_error(ACCT_ERR_INTERNAL, ErrorCode::InternalError, "acct_context allocation failed");
    }

    const shm_mode mode = create_if_not_exist ? shm_mode::OpenOrCreate : shm_mode::Open;
    ctx->upstream_shm = ctx->upstream_shm_manager.open_upstream(upstream_name, mode, 0);
    if (!ctx->upstream_shm && create_if_not_exist && should_recreate_shm_on_init_failure(latest_error().code)) {
        (void)SHMManager::unlink(upstream_name);
        ctx->upstream_shm = ctx->upstream_shm_manager.open_upstream(upstream_name, shm_mode::Create, 0);
    }
    if (!ctx->upstream_shm) {
        return api_error(ACCT_ERR_SHM_FAILED, ErrorCode::ShmOpenFailed, "acct_init_ex open upstream shm failed");
    }

    ctx->orders_dated_name = make_orders_shm_name(orders_base_name, trading_day);
    ctx->orders_shm = ctx->orders_shm_manager.open_orders(ctx->orders_dated_name, mode, 0);
    if (!ctx->orders_shm && create_if_not_exist && should_recreate_shm_on_init_failure(latest_error().code)) {
        (void)SHMManager::unlink(ctx->orders_dated_name);
        ctx->orders_shm = ctx->orders_shm_manager.open_orders(ctx->orders_dated_name, shm_mode::Create, 0);
    }
    if (!ctx->orders_shm) {
        return api_error(ACCT_ERR_SHM_FAILED, ErrorCode::ShmOpenFailed, "acct_init_ex open orders shm failed");
    }

    ctx->upstream_shm_name = upstream_name;
    ctx->orders_base_name = orders_base_name;
    ctx->trading_day = trading_day;
    ctx->initialized = true;
    *out_ctx = ctx.release();
    return ACCT_OK;
}

ACCT_API acct_error_t acct_destroy(acct_ctx_t ctx) {
    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_destroy ctx is null");
    }

    auto* context = ctx;
    context->upstream_shm = nullptr;
    context->orders_shm = nullptr;
    delete context;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_new_order(acct_ctx_t ctx, const char* security_id, uint8_t side, uint8_t market,
                                     uint64_t volume, double price, uint32_t valid_sec, uint32_t* out_order_id) {
    return acct_new_order_ex(ctx, security_id, side, market, volume, price, valid_sec, nullptr, out_order_id);
}

ACCT_API acct_error_t acct_new_order_ex(acct_ctx_t ctx, const char* security_id, uint8_t side, uint8_t market,
                                        uint64_t volume, double price, uint32_t valid_sec,
                                        const acct_order_exec_options_t* exec_options, uint32_t* out_order_id) {
    if (!out_order_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_new_order_ex out_order_id is null");
    }
    *out_order_id = 0;

    if (!ctx || !security_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_new_order_ex invalid ctx/security_id");
    }

    auto* context = ctx;
    if (!context->initialized || !context->upstream_shm) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, ErrorCode::InvalidState, "acct_new_order_ex called before init");
    }

    if (context->cached_orders.size() >= kMaxCachedOrders) {
        return api_error(ACCT_ERR_CACHE_FULL, ErrorCode::QueueFull, "acct_new_order_ex cache full");
    }

    PassiveExecutionAlgo passive_execution_algo = PassiveExecutionAlgo::Default;
    if (!resolve_passive_execution_algo(exec_options, passive_execution_algo)) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam,
                         "acct_new_order_ex invalid passive_exec_algo");
    }

    uint32_t order_id = context->upstream_shm->header.next_order_id.fetch_add(1, std::memory_order_relaxed);
    (void)valid_sec;

    OrderRequest request{};
    acct_error_t build_rc = ACCT_OK;
    if (!build_new_order_request(security_id, side, market, volume, price, passive_execution_algo, order_id, request,
                                 build_rc)) {
        return build_rc;
    }
    request.order_state.store(OrderState::NotSet, std::memory_order_relaxed);

    context->cached_orders[order_id] = request;
    *out_order_id = order_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_send_order(acct_ctx_t ctx, uint32_t order_id) {
    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_send_order ctx is null");
    }

    auto* context = ctx;
    if (!context->initialized) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, ErrorCode::InvalidState, "acct_send_order called before init");
    }

    auto it = context->cached_orders.find(order_id);
    if (it == context->cached_orders.end()) {
        return api_error(ACCT_ERR_ORDER_NOT_FOUND, ErrorCode::OrderNotFound, "acct_send_order order not cached");
    }

    it->second.order_state.store(OrderState::StrategySubmitted, std::memory_order_release);
    const acct_error_t rc = enqueue_order(context, it->second, order_slot_source_t::Strategy, nullptr);
    if (rc != ACCT_OK) {
        return rc;
    }

    context->cached_orders.erase(it);
    return ACCT_OK;
}

ACCT_API acct_error_t acct_submit_order(acct_ctx_t ctx, const char* security_id, uint8_t side, uint8_t market,
                                        uint64_t volume, double price, uint32_t valid_sec, uint32_t* out_order_id) {
    return acct_submit_order_ex(ctx, security_id, side, market, volume, price, valid_sec, nullptr, out_order_id);
}

ACCT_API acct_error_t acct_submit_order_ex(acct_ctx_t ctx, const char* security_id, uint8_t side, uint8_t market,
                                           uint64_t volume, double price, uint32_t valid_sec,
                                           const acct_order_exec_options_t* exec_options, uint32_t* out_order_id) {
    if (!out_order_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_submit_order_ex out_order_id is null");
    }
    *out_order_id = 0;

    if (!ctx || !security_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam,
                         "acct_submit_order_ex invalid ctx/security_id");
    }

    auto* context = ctx;
    if (!context->initialized || !context->upstream_shm) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, ErrorCode::InvalidState, "acct_submit_order_ex called before init");
    }

    PassiveExecutionAlgo passive_execution_algo = PassiveExecutionAlgo::Default;
    if (!resolve_passive_execution_algo(exec_options, passive_execution_algo)) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam,
                         "acct_submit_order_ex invalid passive_exec_algo");
    }

    const uint32_t order_id = context->upstream_shm->header.next_order_id.fetch_add(1, std::memory_order_relaxed);
    (void)valid_sec;

    OrderRequest request{};
    acct_error_t build_rc = ACCT_OK;
    if (!build_new_order_request(security_id, side, market, volume, price, passive_execution_algo, order_id, request,
                                 build_rc)) {
        return build_rc;
    }
    request.order_state.store(OrderState::StrategySubmitted, std::memory_order_release);

    const acct_error_t rc = enqueue_order(context, request, order_slot_source_t::Strategy, nullptr);
    if (rc != ACCT_OK) {
        return rc;
    }

    *out_order_id = order_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_cancel_order(acct_ctx_t ctx, uint32_t orig_order_id, uint32_t valid_sec,
                                        uint32_t* out_cancel_id) {
    if (!out_cancel_id) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_cancel_order out_cancel_id is null");
    }
    *out_cancel_id = 0;

    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_cancel_order ctx is null");
    }

    auto* context = ctx;
    if (!context->initialized || !context->upstream_shm) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, ErrorCode::InvalidState, "acct_cancel_order called before init");
    }

    const uint32_t cancel_id = context->upstream_shm->header.next_order_id.fetch_add(1, std::memory_order_relaxed);
    const MdTime md_time = get_current_md_time();
    (void)valid_sec;

    OrderRequest request;
    request.init_cancel(static_cast<InternalOrderId>(cancel_id), md_time, static_cast<InternalOrderId>(orig_order_id));
    request.order_state.store(OrderState::StrategySubmitted, std::memory_order_release);

    const acct_error_t rc = enqueue_order(context, request, order_slot_source_t::Strategy, nullptr);
    if (rc != ACCT_OK) {
        return rc;
    }

    *out_cancel_id = cancel_id;
    return ACCT_OK;
}

ACCT_API acct_error_t acct_queue_size(acct_ctx_t ctx, size_t* out_size) {
    if (!out_size) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_queue_size out_size is null");
    }
    *out_size = 0;

    if (!ctx) {
        return api_error(ACCT_ERR_INVALID_PARAM, ErrorCode::InvalidParam, "acct_queue_size ctx is null");
    }

    auto* context = ctx;
    if (!context->initialized || !context->upstream_shm) {
        return api_error(ACCT_ERR_NOT_INITIALIZED, ErrorCode::InvalidState, "acct_queue_size called before init");
    }

    *out_size = context->upstream_shm->upstream_order_queue.size();
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
        case ACCT_ERR_ORDER_POOL_FULL:
            return "Order pool is full";
        case ACCT_ERR_INTERNAL:
            return "Internal error";
        default:
            return "Unknown error";
    }
}

ACCT_API const char* acct_version(void) { return ACCT_API_VERSION; }

ACCT_API acct_error_t acct_cleanup_shm(void) {
    if (shm_unlink(acct_service::kUpstreamOrderShmName) < 0 && errno != ENOENT) {
        return api_error(ACCT_ERR_SHM_FAILED, ErrorCode::ShmOpenFailed, "acct_cleanup_shm upstream failed", errno);
    }

    const std::string orders_default = make_orders_shm_name(acct_service::kOrdersShmName, default_trading_day());
    if (shm_unlink(orders_default.c_str()) < 0 && errno != ENOENT) {
        return api_error(ACCT_ERR_SHM_FAILED, ErrorCode::ShmOpenFailed, "acct_cleanup_shm orders failed", errno);
    }

    return ACCT_OK;
}

}  // extern "C"
