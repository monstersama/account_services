#include "api/order_api.h"

#include "common/constants.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "shm/shm_layout.hpp"
#include "version.h"

#include <atomic>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unordered_map>

// 前向声明 close()，避免包含 unistd.h（其中 acct() 函数与 acct 命名空间冲突）
extern "C" int close(int fd) noexcept;

namespace acct {

// ============ 内部上下文结构 ============
struct acct_context {
    upstream_shm_layout* shm_ptr = nullptr;
    int shm_fd = -1;
    std::size_t shm_size = 0;
    std::atomic<uint32_t> next_order_id{1};
    std::unordered_map<uint32_t, order_request> cached_orders;
    bool initialized = false;
};

namespace {

// 最大缓存订单数
constexpr std::size_t kMaxCachedOrders = 1024;

// 获取当前系统时间的 md_time（HHMMSSmmm 格式）
inline md_time_t getCurrentMdTime() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    uint32_t hours = static_cast<uint32_t>(tm_info.tm_hour);
    uint32_t minutes = static_cast<uint32_t>(tm_info.tm_min);
    uint32_t seconds = static_cast<uint32_t>(tm_info.tm_sec);
    uint32_t milliseconds = static_cast<uint32_t>(ts.tv_nsec / 1000000);
    return hours * 10000000 + minutes * 100000 + seconds * 1000 + milliseconds;
}

}  // namespace

// ============ OrderApi 实现 ============

OrderApi::OrderApi() = default;

OrderApi::~OrderApi() {
    if (ctx_) {
        if (ctx_->shm_ptr) {
            munmap(ctx_->shm_ptr, ctx_->shm_size);
        }
        if (ctx_->shm_fd >= 0) {
            close(ctx_->shm_fd);
        }
        delete ctx_;
    }
}

OrderApi::OrderApi(OrderApi&& other) noexcept
    : ctx_(other.ctx_) {
    other.ctx_ = nullptr;
}

OrderApi& OrderApi::operator=(OrderApi&& other) noexcept {
    if (this != &other) {
        // 清理当前资源
        if (ctx_) {
            if (ctx_->shm_ptr) {
                munmap(ctx_->shm_ptr, ctx_->shm_size);
            }
            if (ctx_->shm_fd >= 0) {
                close(ctx_->shm_fd);
            }
            delete ctx_;
        }
        // 转移所有权
        ctx_ = other.ctx_;
        other.ctx_ = nullptr;
    }
    return *this;
}

std::unique_ptr<OrderApi> OrderApi::create(Error* out_error) {
    auto set_error = [out_error](Error err) {
        if (out_error) {
            *out_error = err;
        }
    };

    auto api = std::unique_ptr<OrderApi>(new OrderApi());

    api->ctx_ = new (std::nothrow) acct_context();
    if (!api->ctx_) {
        set_error(Error::InitNoMemory);
        return nullptr;
    }

    // 打开共享内存（只读写模式，假设账户服务已创建）
    api->ctx_->shm_fd = shm_open(kStrategyOrderShmName, O_RDWR, 0666);
    if (api->ctx_->shm_fd < 0) {
        set_error(Error::InitShmOpenFailed);
        delete api->ctx_;
        api->ctx_ = nullptr;
        return nullptr;
    }

    api->ctx_->shm_size = sizeof(upstream_shm_layout);

    // 映射共享内存
    void* ptr = mmap(nullptr, api->ctx_->shm_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, api->ctx_->shm_fd, 0);
    if (ptr == MAP_FAILED) {
        set_error(Error::InitMmapFailed);
        close(api->ctx_->shm_fd);
        delete api->ctx_;
        api->ctx_ = nullptr;
        return nullptr;
    }

    api->ctx_->shm_ptr = static_cast<upstream_shm_layout*>(ptr);

    // 验证魔数
    if (api->ctx_->shm_ptr->header.magic != shm_header::kMagic) {
        set_error(Error::InitInvalidMagic);
        munmap(ptr, api->ctx_->shm_size);
        close(api->ctx_->shm_fd);
        delete api->ctx_;
        api->ctx_ = nullptr;
        return nullptr;
    }

    api->ctx_->initialized = true;
    set_error(Error::Ok);
    return api;
}

uint32_t OrderApi::newOrder(
    std::string_view security_id,
    Side side,
    Market market,
    uint64_t volume,
    double price,
    uint32_t valid_sec
) {
    if (!ctx_ || !ctx_->initialized || security_id.empty()) {
        return 0;
    }

    // 验证参数
    if (side != Side::Buy && side != Side::Sell) {
        return 0;
    }
    if (volume == 0) {
        return 0;
    }

    // 检查缓存容量
    if (ctx_->cached_orders.size() >= kMaxCachedOrders) {
        return 0;
    }

    // 分配订单ID
    uint32_t order_id = ctx_->next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 将浮点价格转换为内部整数格式（元 -> 分，乘以 100）
    dprice_t internal_price = static_cast<dprice_t>(price * 100.0 + 0.5);

    // 使用当前系统时间作为 md_time
    md_time_t md_time = getCurrentMdTime();

    // valid_sec 参数暂时不使用（为避免未使用警告）
    (void)valid_sec;

    // 创建订单请求
    order_request request;
    request.init_new(
        security_id,
        static_cast<internal_security_id_t>(0),
        static_cast<internal_order_id_t>(order_id),
        static_cast<trade_side_t>(side),
        static_cast<market_t>(market),
        static_cast<volume_t>(volume),
        internal_price,
        md_time
    );
    request.order_status.store(order_status_t::NotSet, std::memory_order_relaxed);

    // 缓存订单
    ctx_->cached_orders[order_id] = request;

    return order_id;
}

Error OrderApi::sendOrder(uint32_t order_id) {
    if (!ctx_) {
        return Error::InvalidParam;
    }

    if (!ctx_->initialized) {
        return Error::NotInitialized;
    }

    // 查找缓存的订单
    auto it = ctx_->cached_orders.find(order_id);
    if (it == ctx_->cached_orders.end()) {
        return Error::OrderNotFound;
    }

    // 更新状态
    it->second.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 推入队列
    bool success = ctx_->shm_ptr->strategy_order_queue.try_push(it->second);
    if (!success) {
        return Error::QueueFull;
    }

    // 从缓存中移除
    ctx_->cached_orders.erase(it);

    return Error::Ok;
}

uint32_t OrderApi::submitOrder(
    std::string_view security_id,
    Side side,
    Market market,
    uint64_t volume,
    double price,
    uint32_t valid_sec
) {
    if (!ctx_ || !ctx_->initialized) {
        return 0;
    }

    // 验证参数
    if (side != Side::Buy && side != Side::Sell) {
        return 0;
    }
    if (volume == 0) {
        return 0;
    }

    // 分配订单ID
    uint32_t order_id = ctx_->next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 将浮点价格转换为内部整数格式
    dprice_t internal_price = static_cast<dprice_t>(price * 100.0 + 0.5);

    // 使用当前系统时间
    md_time_t md_time = getCurrentMdTime();

    // valid_sec 参数暂时不使用
    (void)valid_sec;

    // 创建订单请求
    order_request request;
    request.init_new(
        security_id,
        static_cast<internal_security_id_t>(0),
        static_cast<internal_order_id_t>(order_id),
        static_cast<trade_side_t>(side),
        static_cast<market_t>(market),
        static_cast<volume_t>(volume),
        internal_price,
        md_time
    );
    request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 直接推入队列
    bool success = ctx_->shm_ptr->strategy_order_queue.try_push(request);
    if (!success) {
        return 0;
    }

    return order_id;
}

uint32_t OrderApi::cancelOrder(uint32_t orig_order_id, uint32_t valid_sec) {
    if (!ctx_ || !ctx_->initialized) {
        return 0;
    }

    // 分配撤单请求ID
    uint32_t cancel_id = ctx_->next_order_id.fetch_add(1, std::memory_order_relaxed);

    // 使用当前系统时间
    md_time_t md_time = getCurrentMdTime();

    // valid_sec 参数暂时不使用
    (void)valid_sec;

    // 创建撤单请求
    order_request request;
    request.init_cancel(
        static_cast<internal_order_id_t>(cancel_id),
        md_time,
        static_cast<internal_order_id_t>(orig_order_id)
    );
    request.order_status.store(order_status_t::StrategySubmitted, std::memory_order_release);

    // 推入队列
    bool success = ctx_->shm_ptr->strategy_order_queue.try_push(request);
    if (!success) {
        return 0;
    }

    return cancel_id;
}

size_t OrderApi::queueSize() const {
    if (!ctx_ || !ctx_->initialized || !ctx_->shm_ptr) {
        return 0;
    }

    return ctx_->shm_ptr->strategy_order_queue.size();
}

bool OrderApi::isInitialized() const noexcept {
    return ctx_ && ctx_->initialized;
}

const char* OrderApi::errorString(Error err) {
    switch (err) {
        case Error::Ok:                         return "Success";
        case Error::NotInitialized:             return "Context not initialized";
        case Error::InvalidParam:               return "Invalid parameter";
        case Error::QueueFull:                  return "Queue is full";
        case Error::ShmFailed:                  return "Shared memory operation failed";
        case Error::OrderNotFound:              return "Order not found";
        case Error::InitNoMemory:               return "Initialization failed: memory allocation failed";
        case Error::InitShmOpenFailed:          return "Initialization failed: shared memory open failed";
        case Error::InitMmapFailed:             return "Initialization failed: memory mapping failed";
        case Error::InitInvalidMagic:           return "Initialization failed: invalid magic number";
        case Error::Internal:                   return "Internal error";
        default:                                return "Unknown error";
    }
}

const char* OrderApi::version() {
    return ACCT_API_VERSION;
}

}  // namespace acct
