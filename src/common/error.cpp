#include "common/error.hpp"

#include <algorithm>

namespace acct_service {

namespace {

thread_local error_status g_thread_last_error{};
error_status g_latest_error{};
spinlock g_latest_error_lock;

std::atomic<int> g_shutdown_reason{-1};

const error_policy kRecoverablePolicy{error_severity::Recoverable, false, false};
const error_policy kCriticalPolicy{error_severity::Critical, true, true};
const error_policy kFatalPolicy{error_severity::Fatal, true, true};

}  // namespace

bool error_status::ok() const noexcept { return code == error_code::Ok; }

const char* to_string(error_domain domain) noexcept {
    switch (domain) {
        case error_domain::none:
            return "none";
        case error_domain::config:
            return "config";
        case error_domain::shm:
            return "shm";
        case error_domain::core:
            return "core";
        case error_domain::order:
            return "order";
        case error_domain::risk:
            return "risk";
        case error_domain::portfolio:
            return "portfolio";
        case error_domain::api:
            return "api";
    }
    return "unknown";
}

const char* to_string(error_code code) noexcept {
    switch (code) {
        case error_code::Ok:
            return "Ok";
        case error_code::InvalidConfig:
            return "InvalidConfig";
        case error_code::InvalidParam:
            return "InvalidParam";
        case error_code::ConfigParseFailed:
            return "ConfigParseFailed";
        case error_code::ConfigValidateFailed:
            return "ConfigValidateFailed";
        case error_code::InvalidState:
            return "InvalidState";
        case error_code::InvalidOrderId:
            return "InvalidOrderId";
        case error_code::DuplicateOrder:
            return "DuplicateOrder";
        case error_code::OrderBookFull:
            return "OrderBookFull";
        case error_code::OrderNotFound:
            return "OrderNotFound";
        case error_code::QueueFull:
            return "QueueFull";
        case error_code::QueuePushFailed:
            return "QueuePushFailed";
        case error_code::QueuePopFailed:
            return "QueuePopFailed";
        case error_code::RouteFailed:
            return "RouteFailed";
        case error_code::SplitFailed:
            return "SplitFailed";
        case error_code::PositionUpdateFailed:
            return "PositionUpdateFailed";
        case error_code::OrderInvariantBroken:
            return "OrderInvariantBroken";
        case error_code::ComponentUnavailable:
            return "ComponentUnavailable";
        case error_code::ShmOpenFailed:
            return "ShmOpenFailed";
        case error_code::ShmFstatFailed:
            return "ShmFstatFailed";
        case error_code::ShmResizeFailed:
            return "ShmResizeFailed";
        case error_code::ShmMmapFailed:
            return "ShmMmapFailed";
        case error_code::ShmHeaderInvalid:
            return "ShmHeaderInvalid";
        case error_code::ShmHeaderCorrupted:
            return "ShmHeaderCorrupted";
        case error_code::HealthCheckFailed:
            return "HealthCheckFailed";
        case error_code::LoggerInitFailed:
            return "LoggerInitFailed";
        case error_code::LoggerThreadFailed:
            return "LoggerThreadFailed";
        case error_code::LoggerQueueFull:
            return "LoggerQueueFull";
        case error_code::InternalError:
            return "InternalError";
    }
    return "Unknown";
}

const char* to_string(error_severity severity) noexcept {
    switch (severity) {
        case error_severity::Recoverable:
            return "Recoverable";
        case error_severity::Critical:
            return "Critical";
        case error_severity::Fatal:
            return "Fatal";
    }
    return "Recoverable";
}

const error_policy& classify(error_domain domain, error_code code) noexcept {
    (void)domain;
    switch (code) {
        case error_code::Ok:
        case error_code::InvalidOrderId:
        case error_code::DuplicateOrder:
        case error_code::InvalidParam:
        case error_code::OrderBookFull:
        case error_code::OrderNotFound:
        case error_code::QueueFull:
        case error_code::QueuePushFailed:
        case error_code::QueuePopFailed:
        case error_code::RouteFailed:
        case error_code::SplitFailed:
        case error_code::HealthCheckFailed:
        case error_code::LoggerQueueFull:
            return kRecoverablePolicy;

        // Critical: 服务关键路径异常，继续运行可能扩大影响，需要停服并退出主进程。
        case error_code::InvalidConfig:
            // 启动配置不合法，服务基础假设不成立。
        case error_code::ConfigParseFailed:
            // 配置文件解析失败，无法保证后续行为正确。
        case error_code::ConfigValidateFailed:
            // 配置通过语法但不满足业务约束。
        case error_code::InvalidState:
            // 关键生命周期状态机异常（如在错误阶段继续 run）。
        case error_code::ComponentUnavailable:
            // 核心组件缺失，主流程不可用。
        case error_code::ShmOpenFailed:
            // 共享内存不可用，无法与上下游通信。
        case error_code::ShmFstatFailed:
            // 共享内存元数据检查失败，安全性不可确认。
        case error_code::ShmResizeFailed:
            // 共享内存尺寸异常，布局可能不兼容。
        case error_code::ShmMmapFailed:
            // 地址空间映射失败，核心数据平面不可用。
        case error_code::ShmHeaderInvalid:
            // 共享内存头非法（但未确认破坏程度），按关键错误处理。
        case error_code::LoggerInitFailed:
            // 日志不可用会导致“盲飞”，影响排障和风控审计。
        case error_code::LoggerThreadFailed:
            return kCriticalPolicy;

        // Fatal: 一致性/状态已不可信，必须最高优先级停服并退出。
        case error_code::PositionUpdateFailed:
            // 成交已落但资金/持仓未能同步，账务一致性破坏。
        case error_code::OrderInvariantBroken:
            // 订单簿内部不变量破坏（索引、父子关系等）。
        case error_code::ShmHeaderCorrupted:
            // 共享内存头损坏，数据面可信度丧失。
        case error_code::InternalError:
            // 未分类内部错误默认按最保守策略处理。
            return kFatalPolicy;
    }
    return kRecoverablePolicy;
}

error_status make_error_status(error_domain domain, error_code code, std::string_view module, std::string_view file,
    uint32_t line, std::string_view message, int sys_errno) {
    error_status status;
    status.domain = domain;
    status.code = code;
    status.sys_errno = sys_errno;
    status.ts_ns = now_ns();
    status.line = line;
    status.module.assign(module);
    status.file.assign(file);
    status.message.assign(message);
    return status;
}

void error_registry::record(const error_status& status) {
    lock_guard<spinlock> guard(lock_);

    ++counters_[status.code];
    history_[history_pos_] = status;
    history_pos_ = (history_pos_ + 1) % kHistoryCapacity;
    if (history_size_ < kHistoryCapacity) {
        ++history_size_;
    }
}

uint64_t error_registry::count(error_code code) const {
    lock_guard<spinlock> guard(lock_);
    const auto it = counters_.find(code);
    if (it == counters_.end()) {
        return 0;
    }
    return it->second;
}

std::vector<error_status> error_registry::recent_errors() const {
    lock_guard<spinlock> guard(lock_);

    std::vector<error_status> errors;
    errors.reserve(history_size_);

    std::size_t start = 0;
    if (history_size_ == kHistoryCapacity) {
        start = history_pos_;
    }

    for (std::size_t i = 0; i < history_size_; ++i) {
        const std::size_t index = (start + i) % kHistoryCapacity;
        errors.push_back(history_[index]);
    }
    return errors;
}

void error_registry::reset() {
    lock_guard<spinlock> guard(lock_);
    counters_.clear();
    history_pos_ = 0;
    history_size_ = 0;
}

error_registry& global_error_registry() {
    static error_registry registry;
    return registry;
}

void record_error(const error_status& status) {
    g_thread_last_error = status;

    {
        lock_guard<spinlock> guard(g_latest_error_lock);
        g_latest_error = status;
    }

    if (!status.ok()) {
        global_error_registry().record(status);
        const error_policy& policy = classify(status.domain, status.code);
        if (policy.stop_service || policy.exit_process) {
            // 统一在这里触发 shutdown 闩锁：
            // Critical/Fatal 都会让 event_loop 停止并由主进程非0退出。
            request_shutdown(policy.severity);
        }
    }
}

const error_status& last_error() noexcept { return g_thread_last_error; }

const error_status& latest_error() noexcept {
    static thread_local error_status snapshot{};
    lock_guard<spinlock> guard(g_latest_error_lock);
    snapshot = g_latest_error;
    return snapshot;
}

void clear_last_error() noexcept { g_thread_last_error = error_status{}; }

void request_shutdown(error_severity severity) noexcept {
    int expected = g_shutdown_reason.load(std::memory_order_acquire);
    const int desired = static_cast<int>(severity);

    while (expected < desired) {
        if (g_shutdown_reason.compare_exchange_weak(
                expected, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
}

error_severity shutdown_reason() noexcept {
    const int value = g_shutdown_reason.load(std::memory_order_acquire);
    if (value <= static_cast<int>(error_severity::Recoverable)) {
        return error_severity::Recoverable;
    }
    if (value >= static_cast<int>(error_severity::Fatal)) {
        return error_severity::Fatal;
    }
    return error_severity::Critical;
}

void clear_shutdown_reason() noexcept { g_shutdown_reason.store(-1, std::memory_order_release); }

bool should_stop_service() noexcept {
    return g_shutdown_reason.load(std::memory_order_acquire) >= static_cast<int>(error_severity::Critical);
}

bool should_exit_process() noexcept {
    return g_shutdown_reason.load(std::memory_order_acquire) >= static_cast<int>(error_severity::Critical);
}

}  // namespace acct_service
