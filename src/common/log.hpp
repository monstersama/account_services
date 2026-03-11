#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "common/error.hpp"
#include "common/fixed_string.hpp"
#include "common/spinlock.hpp"
#include "common/types.hpp"

namespace acct_service {

struct LogConfig;

enum class LogLevel : uint8_t {
    debug = 0,
    info,
    warn,
    error,
    fatal,
};

struct LogRecord {
    TimestampNs ts_ns = 0;
    LogLevel level = LogLevel::info;
    ErrorSeverity severity = ErrorSeverity::Recoverable;
    int sys_errno = 0;
    uint32_t line = 0;
    FixedString<24> module{};
    FixedString<96> file{};
    FixedString<256> message{};
    ErrorDomain domain = ErrorDomain::none;
    ErrorCode code = ErrorCode::Ok;
};

class AsyncLogger {
public:
    AsyncLogger() = default;
    ~AsyncLogger() noexcept;

    bool init(const LogConfig& config, AccountId account_id);
    bool init(const LogConfig& config, AccountId account_id, std::string_view instance_name);
    void shutdown() noexcept;
    bool flush(uint32_t timeout_ms);
    bool log(const LogRecord& record);

    uint64_t dropped_count() const noexcept;
    bool healthy() const noexcept;

private:
    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    struct Impl;
    std::unique_ptr<Impl> impl_{};
};

bool init_logger(const LogConfig& config, AccountId account_id);
bool init_logger(const LogConfig& config, AccountId account_id, std::string_view instance_name);
void shutdown_logger();
bool flush_logger(uint32_t timeout_ms);
bool logger_healthy() noexcept;
uint64_t logger_dropped_count() noexcept;

void log_message(LogLevel level, std::string_view module, std::string_view file, uint32_t line,
                 std::string_view message, const ErrorStatus* status = nullptr, int sys_errno = 0);

const char* to_string(LogLevel level) noexcept;

}  // namespace acct_service

#define ACCT_LOG_DEBUG(module, message)                                                                               \
    ::acct_service::log_message(::acct_service::LogLevel::debug, (module), __FILE__, static_cast<uint32_t>(__LINE__), \
                                (message))

#define ACCT_LOG_INFO(module, message)                                                                               \
    ::acct_service::log_message(::acct_service::LogLevel::info, (module), __FILE__, static_cast<uint32_t>(__LINE__), \
                                (message))

#define ACCT_LOG_WARN(module, message)                                                                               \
    ::acct_service::log_message(::acct_service::LogLevel::warn, (module), __FILE__, static_cast<uint32_t>(__LINE__), \
                                (message))

#define ACCT_LOG_ERROR(module, message)                                                                               \
    ::acct_service::log_message(::acct_service::LogLevel::error, (module), __FILE__, static_cast<uint32_t>(__LINE__), \
                                (message))

#define ACCT_LOG_FATAL(module, message)                                                                               \
    ::acct_service::log_message(::acct_service::LogLevel::fatal, (module), __FILE__, static_cast<uint32_t>(__LINE__), \
                                (message))

#define ACCT_LOG_ERROR_STATUS(status)                                                                \
    ::acct_service::log_message(::acct_service::LogLevel::error, (status).module.view(), __FILE__,   \
                                static_cast<uint32_t>(__LINE__), (status).message.view(), &(status), \
                                (status).sys_errno)
