#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "common/error.hpp"
#include "common/fixed_string.hpp"
#include "common/spinlock.hpp"
#include "common/types.hpp"

namespace acct_service {

struct log_config;

enum class log_level : uint8_t {
    debug = 0,
    info,
    warn,
    error,
    fatal,
};

struct log_record {
    timestamp_ns_t ts_ns = 0;
    log_level level = log_level::info;
    error_severity severity = error_severity::Recoverable;
    int sys_errno = 0;
    uint32_t line = 0;
    fixed_string<24> module{};
    fixed_string<96> file{};
    fixed_string<256> message{};
    error_domain domain = error_domain::none;
    error_code code = error_code::Ok;
};

class async_logger {
public:
    async_logger() = default;
    ~async_logger() noexcept;

    bool init(const log_config& config, account_id_t account_id);
    void shutdown() noexcept;
    bool flush(uint32_t timeout_ms);
    bool log(const log_record& record);

    uint64_t dropped_count() const noexcept;
    bool healthy() const noexcept;

private:
    async_logger(const async_logger&) = delete;
    async_logger& operator=(const async_logger&) = delete;

    struct impl;
    std::unique_ptr<impl> impl_{};
};

bool init_logger(const log_config& config, account_id_t account_id);
void shutdown_logger();
bool flush_logger(uint32_t timeout_ms);
bool logger_healthy() noexcept;
uint64_t logger_dropped_count() noexcept;

void log_message(log_level level, std::string_view module, std::string_view file, uint32_t line,
    std::string_view message, const error_status* status = nullptr, int sys_errno = 0);

const char* to_string(log_level level) noexcept;

}  // namespace acct_service

#define ACCT_LOG_DEBUG(module, message) \
    ::acct_service::log_message(        \
        ::acct_service::log_level::debug, (module), __FILE__, static_cast<uint32_t>(__LINE__), (message))

#define ACCT_LOG_INFO(module, message) \
    ::acct_service::log_message(       \
        ::acct_service::log_level::info, (module), __FILE__, static_cast<uint32_t>(__LINE__), (message))

#define ACCT_LOG_WARN(module, message) \
    ::acct_service::log_message(       \
        ::acct_service::log_level::warn, (module), __FILE__, static_cast<uint32_t>(__LINE__), (message))

#define ACCT_LOG_ERROR(module, message) \
    ::acct_service::log_message(        \
        ::acct_service::log_level::error, (module), __FILE__, static_cast<uint32_t>(__LINE__), (message))

#define ACCT_LOG_FATAL(module, message) \
    ::acct_service::log_message(        \
        ::acct_service::log_level::fatal, (module), __FILE__, static_cast<uint32_t>(__LINE__), (message))

#define ACCT_LOG_ERROR_STATUS(status)                                                               \
    ::acct_service::log_message(::acct_service::log_level::error, (status).module.view(), __FILE__, \
        static_cast<uint32_t>(__LINE__), (status).message.view(), &(status), (status).sys_errno)
