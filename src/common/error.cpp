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

// API 域保留严重级别，但不替调用方做进程级停服/退出。
const error_policy kApiCriticalPolicy{error_severity::Critical, false, false};
const error_policy kApiFatalPolicy{error_severity::Fatal, false, false};

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
        // Critical: 服务关键路径异常，继续运行可能扩大影响，需要停服并退出主进程。
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
        // Fatal: 一致性/状态已不可信，必须最高优先级停服并退出。
        case error_code::PositionUpdateFailed:
            return "PositionUpdateFailed";
        case error_code::OrderInvariantBroken:
            return "OrderInvariantBroken";
        case error_code::OrderPoolFull:
            return "OrderPoolFull";
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

namespace {

constexpr bool code_in_range(error_code code, uint16_t begin, uint16_t end) noexcept {
    const uint16_t value = static_cast<uint16_t>(code);
    return value >= begin && value <= end;
}

const error_policy& classify_by_code(error_code code) noexcept {
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
        case error_code::OrderPoolFull:
        case error_code::HealthCheckFailed:
        case error_code::LoggerQueueFull:
            return kRecoverablePolicy;

        case error_code::InvalidConfig:
        case error_code::ConfigParseFailed:
        case error_code::ConfigValidateFailed:
        case error_code::InvalidState:
        case error_code::ComponentUnavailable:
        case error_code::ShmOpenFailed:
        case error_code::ShmFstatFailed:
        case error_code::ShmResizeFailed:
        case error_code::ShmMmapFailed:
        case error_code::ShmHeaderInvalid:
        case error_code::LoggerInitFailed:
        case error_code::LoggerThreadFailed:
            return kCriticalPolicy;

        case error_code::PositionUpdateFailed:
        case error_code::OrderInvariantBroken:
        case error_code::ShmHeaderCorrupted:
        case error_code::InternalError:
            return kFatalPolicy;
    }
    return kRecoverablePolicy;
}

const error_policy& classify_for_api(error_code code) noexcept {
    const error_severity severity = classify_by_code(code).severity;
    switch (severity) {
        case error_severity::Recoverable:
            return kRecoverablePolicy;
        case error_severity::Critical:
            return kApiCriticalPolicy;
        case error_severity::Fatal:
            return kApiFatalPolicy;
    }
    return kRecoverablePolicy;
}

}  // namespace

const error_policy& classify(error_domain domain, error_code code) noexcept {
    switch (domain) {
        case error_domain::api:
            // C API 场景下不替调用方进程做停服/退出决策：
            // 保留严重级别用于可观测性，但 stop/exit 始终为 false。
            return classify_for_api(code);

        case error_domain::config:
            if (code_in_range(code, 2000, 2099)) {
                // 配置链路错误一旦触发，基础假设已失效，按 Critical 处理。
                return kCriticalPolicy;
            }
            break;

        case error_domain::shm:
            if (code == error_code::ShmHeaderCorrupted) {
                return kFatalPolicy;
            }
            if (code_in_range(code, 3000, 3099)) {
                return kCriticalPolicy;
            }
            break;

        case error_domain::order:
            if (code == error_code::OrderInvariantBroken) {
                return kFatalPolicy;
            }
            if (code_in_range(code, 4000, 4099)) {
                return kRecoverablePolicy;
            }
            break;

        case error_domain::portfolio:
            if (code == error_code::PositionUpdateFailed || code == error_code::ShmHeaderCorrupted) {
                return kFatalPolicy;
            }
            if (code == error_code::ComponentUnavailable || code == error_code::ShmHeaderInvalid) {
                return kCriticalPolicy;
            }
            break;

        case error_domain::none:
        case error_domain::core:
        case error_domain::risk:
            break;
    }

    // 默认按全局错误码策略兜底。
    return classify_by_code(code);
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
            // 仅当 policy.stop_service / policy.exit_process 为 true 时才停服退出。
            // 例如 api 域即便是 Critical/Fatal，也不会替调用方进程做退出决策。
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
