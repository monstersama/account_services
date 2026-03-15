#include "common/error.hpp"

namespace acct_service {

namespace {

thread_local ErrorStatus g_thread_last_error{};
ErrorStatus g_latest_error{};
SpinLock g_latest_error_lock;

std::atomic<int> g_shutdown_reason{-1};

const ErrorPolicy kRecoverablePolicy{ErrorSeverity::Recoverable, false, false};
const ErrorPolicy kCriticalPolicy{ErrorSeverity::Critical, true, true};
const ErrorPolicy kFatalPolicy{ErrorSeverity::Fatal, true, true};

// API 域保留严重级别，但不替调用方做进程级停服/退出。
const ErrorPolicy kApiCriticalPolicy{ErrorSeverity::Critical, false, false};
const ErrorPolicy kApiFatalPolicy{ErrorSeverity::Fatal, false, false};

}  // namespace

bool ErrorStatus::ok() const noexcept { return code == ErrorCode::Ok; }

const char* to_string(ErrorDomain domain) noexcept {
    switch (domain) {
        case ErrorDomain::none:
            return "none";
        case ErrorDomain::config:
            return "config";
        case ErrorDomain::shm:
            return "shm";
        case ErrorDomain::core:
            return "core";
        case ErrorDomain::order:
            return "order";
        case ErrorDomain::risk:
            return "risk";
        case ErrorDomain::portfolio:
            return "portfolio";
        case ErrorDomain::api:
            return "api";
    }
    return "unknown";
}

const char* to_string(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
            return "Ok";
        // Critical: 服务关键路径异常，继续运行可能扩大影响，需要停服并退出主进程。
        case ErrorCode::InvalidConfig:
            return "InvalidConfig";
        case ErrorCode::InvalidParam:
            return "InvalidParam";
        case ErrorCode::ConfigParseFailed:
            return "ConfigParseFailed";
        case ErrorCode::ConfigValidateFailed:
            return "ConfigValidateFailed";
        case ErrorCode::InvalidState:
            return "InvalidState";
        case ErrorCode::InvalidOrderId:
            return "InvalidOrderId";
        case ErrorCode::DuplicateOrder:
            return "DuplicateOrder";
        case ErrorCode::OrderBookFull:
            return "OrderBookFull";
        case ErrorCode::OrderNotFound:
            return "OrderNotFound";
        case ErrorCode::QueueFull:
            return "QueueFull";
        case ErrorCode::QueuePushFailed:
            return "QueuePushFailed";
        case ErrorCode::QueuePopFailed:
            return "QueuePopFailed";
        case ErrorCode::RouteFailed:
            return "RouteFailed";
        case ErrorCode::SplitFailed:
            return "SplitFailed";
        // Fatal: 一致性/状态已不可信，必须最高优先级停服并退出。
        case ErrorCode::PositionUpdateFailed:
            return "PositionUpdateFailed";
        case ErrorCode::OrderInvariantBroken:
            return "OrderInvariantBroken";
        case ErrorCode::OrderPoolFull:
            return "OrderPoolFull";
        case ErrorCode::ComponentUnavailable:
            return "ComponentUnavailable";
        case ErrorCode::ShmOpenFailed:
            return "ShmOpenFailed";
        case ErrorCode::ShmFstatFailed:
            return "ShmFstatFailed";
        case ErrorCode::ShmResizeFailed:
            return "ShmResizeFailed";
        case ErrorCode::ShmMmapFailed:
            return "ShmMmapFailed";
        case ErrorCode::ShmHeaderInvalid:
            return "ShmHeaderInvalid";
        case ErrorCode::ShmHeaderCorrupted:
            return "ShmHeaderCorrupted";
        case ErrorCode::HealthCheckFailed:
            return "HealthCheckFailed";
        case ErrorCode::LoggerInitFailed:
            return "LoggerInitFailed";
        case ErrorCode::LoggerThreadFailed:
            return "LoggerThreadFailed";
        case ErrorCode::LoggerQueueFull:
            return "LoggerQueueFull";
        case ErrorCode::InternalError:
            return "InternalError";
    }
    return "Unknown";
}

const char* to_string(ErrorSeverity severity) noexcept {
    switch (severity) {
        case ErrorSeverity::Recoverable:
            return "Recoverable";
        case ErrorSeverity::Critical:
            return "Critical";
        case ErrorSeverity::Fatal:
            return "Fatal";
    }
    return "Recoverable";
}

namespace {

constexpr bool code_in_range(ErrorCode code, uint16_t begin, uint16_t end) noexcept {
    const uint16_t value = static_cast<uint16_t>(code);
    return value >= begin && value <= end;
}

const ErrorPolicy& classify_by_code(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Ok:
        case ErrorCode::InvalidOrderId:
        case ErrorCode::DuplicateOrder:
        case ErrorCode::InvalidParam:
        case ErrorCode::OrderBookFull:
        case ErrorCode::OrderNotFound:
        case ErrorCode::QueueFull:
        case ErrorCode::QueuePushFailed:
        case ErrorCode::QueuePopFailed:
        case ErrorCode::RouteFailed:
        case ErrorCode::SplitFailed:
        case ErrorCode::OrderPoolFull:
        case ErrorCode::HealthCheckFailed:
        case ErrorCode::LoggerQueueFull:
            return kRecoverablePolicy;

        case ErrorCode::InvalidConfig:
        case ErrorCode::ConfigParseFailed:
        case ErrorCode::ConfigValidateFailed:
        case ErrorCode::InvalidState:
        case ErrorCode::ComponentUnavailable:
        case ErrorCode::ShmOpenFailed:
        case ErrorCode::ShmFstatFailed:
        case ErrorCode::ShmResizeFailed:
        case ErrorCode::ShmMmapFailed:
        case ErrorCode::ShmHeaderInvalid:
        case ErrorCode::LoggerInitFailed:
        case ErrorCode::LoggerThreadFailed:
            return kCriticalPolicy;

        case ErrorCode::PositionUpdateFailed:
        case ErrorCode::OrderInvariantBroken:
        case ErrorCode::ShmHeaderCorrupted:
        case ErrorCode::InternalError:
            return kFatalPolicy;
    }
    return kRecoverablePolicy;
}

const ErrorPolicy& classify_for_api(ErrorCode code) noexcept {
    const ErrorSeverity severity = classify_by_code(code).severity;
    switch (severity) {
        case ErrorSeverity::Recoverable:
            return kRecoverablePolicy;
        case ErrorSeverity::Critical:
            return kApiCriticalPolicy;
        case ErrorSeverity::Fatal:
            return kApiFatalPolicy;
    }
    return kRecoverablePolicy;
}

}  // namespace

const ErrorPolicy& classify(ErrorDomain domain, ErrorCode code) noexcept {
    switch (domain) {
        case ErrorDomain::api:
            // C API 场景下不替调用方进程做停服/退出决策：
            // 保留严重级别用于可观测性，但 stop/exit 始终为 false。
            return classify_for_api(code);

        case ErrorDomain::config:
            if (code_in_range(code, 2000, 2099)) {
                // 配置链路错误一旦触发，基础假设已失效，按 Critical 处理。
                return kCriticalPolicy;
            }
            break;

        case ErrorDomain::shm:
            if (code == ErrorCode::ShmHeaderCorrupted) {
                return kFatalPolicy;
            }
            if (code_in_range(code, 3000, 3099)) {
                return kCriticalPolicy;
            }
            break;

        case ErrorDomain::order:
            if (code == ErrorCode::OrderInvariantBroken) {
                return kFatalPolicy;
            }
            if (code_in_range(code, 4000, 4099)) {
                return kRecoverablePolicy;
            }
            break;

        case ErrorDomain::portfolio:
            if (code == ErrorCode::PositionUpdateFailed || code == ErrorCode::ShmHeaderCorrupted) {
                return kFatalPolicy;
            }
            if (code == ErrorCode::ComponentUnavailable || code == ErrorCode::ShmHeaderInvalid) {
                return kCriticalPolicy;
            }
            break;

        case ErrorDomain::none:
        case ErrorDomain::core:
        case ErrorDomain::risk:
            break;
    }

    // 默认按全局错误码策略兜底。
    return classify_by_code(code);
}

ErrorStatus make_error_status(ErrorDomain domain, ErrorCode code, std::string_view module, std::string_view file,
    uint32_t line, std::string_view message, int sys_errno) {
    ErrorStatus status;
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

void ErrorRegistry::record(const ErrorStatus& status) {
    LockGuard<SpinLock> guard(lock_);

    ++counters_[status.code];
    history_[history_pos_] = status;
    history_pos_ = (history_pos_ + 1) % kHistoryCapacity;
    if (history_size_ < kHistoryCapacity) {
        ++history_size_;
    }
}

uint64_t ErrorRegistry::count(ErrorCode code) const {
    LockGuard<SpinLock> guard(lock_);
    const auto it = counters_.find(code);
    if (it == counters_.end()) {
        return 0;
    }
    return it->second;
}

std::vector<ErrorStatus> ErrorRegistry::recent_errors() const {
    LockGuard<SpinLock> guard(lock_);

    std::vector<ErrorStatus> errors;
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

void ErrorRegistry::reset() {
    LockGuard<SpinLock> guard(lock_);
    counters_.clear();
    history_pos_ = 0;
    history_size_ = 0;
}

ErrorRegistry& global_error_registry() {
    static ErrorRegistry registry;
    return registry;
}

void record_error(const ErrorStatus& status) {
    g_thread_last_error = status;

    {
        LockGuard<SpinLock> guard(g_latest_error_lock);
        g_latest_error = status;
    }

    if (!status.ok()) {
        global_error_registry().record(status);
        const ErrorPolicy& policy = classify(status.domain, status.code);
        if (policy.stop_service || policy.exit_process) {
            // 统一在这里触发 shutdown 闩锁：
            // 仅当 policy.stop_service / policy.exit_process 为 true 时才停服退出。
            // 例如 api 域即便是 Critical/Fatal，也不会替调用方进程做退出决策。
            request_shutdown(policy.severity);
        }
    }
}

const ErrorStatus& last_error() noexcept { return g_thread_last_error; }

const ErrorStatus& latest_error() noexcept {
    static thread_local ErrorStatus snapshot{};
    LockGuard<SpinLock> guard(g_latest_error_lock);
    snapshot = g_latest_error;
    return snapshot;
}

void clear_last_error() noexcept { g_thread_last_error = ErrorStatus{}; }

void request_shutdown(ErrorSeverity severity) noexcept {
    int expected = g_shutdown_reason.load(std::memory_order_acquire);
    const int desired = static_cast<int>(severity);

    while (expected < desired) {
        if (g_shutdown_reason.compare_exchange_weak(
                expected, desired, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
}

ErrorSeverity shutdown_reason() noexcept {
    const int value = g_shutdown_reason.load(std::memory_order_acquire);
    if (value <= static_cast<int>(ErrorSeverity::Recoverable)) {
        return ErrorSeverity::Recoverable;
    }
    if (value >= static_cast<int>(ErrorSeverity::Fatal)) {
        return ErrorSeverity::Fatal;
    }
    return ErrorSeverity::Critical;
}

void clear_shutdown_reason() noexcept { g_shutdown_reason.store(-1, std::memory_order_release); }

bool should_stop_service() noexcept {
    return g_shutdown_reason.load(std::memory_order_acquire) >= static_cast<int>(ErrorSeverity::Critical);
}

bool should_exit_process() noexcept {
    return g_shutdown_reason.load(std::memory_order_acquire) >= static_cast<int>(ErrorSeverity::Critical);
}

}  // namespace acct_service
