#include "common/log.hpp"

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "common/basecore_log_format.hpp"
#include "common/basecore_log_modules.hpp"
#include "core/config_manager.hpp"
#include "logging/log.hpp"
#include "shm/shm_generic.hpp"

namespace acct_service {

namespace {

constexpr std::string_view kDefaultLoggerInstanceName = "account_service";
constexpr std::size_t kFixedBufferCapacity = base_core_log::ShmLogger::kBufferCapacity;
constexpr std::size_t kVariableBufferCapacity = base_core_log::ShmLogger::kBufferVarSize;

// Parses the configured minimum log level into the internal enum.
LogLevel parse_level(const std::string& level) {
    if (level == "debug") {
        return LogLevel::debug;
    }
    if (level == "info") {
        return LogLevel::info;
    }
    if (level == "warn") {
        return LogLevel::warn;
    }
    if (level == "error") {
        return LogLevel::error;
    }
    if (level == "fatal") {
        return LogLevel::fatal;
    }
    return LogLevel::info;
}

// Converts account_services log levels to the BaseCore writer enum.
base_core_log::LogLevel to_basecore_level(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug:
            return base_core_log::LogLevel::debug;
        case LogLevel::info:
            return base_core_log::LogLevel::info;
        case LogLevel::warn:
            return base_core_log::LogLevel::warn;
        case LogLevel::error:
            return base_core_log::LogLevel::error;
        case LogLevel::fatal:
            return base_core_log::LogLevel::fatal;
    }
    return base_core_log::LogLevel::info;
}

// Chooses whether stderr fallback is still required when shared-memory logging fails.
bool should_sync_fallback(LogLevel level) { return level == LogLevel::error || level == LogLevel::fatal; }

// Emits a best-effort stderr line when the structured logger is unavailable.
void write_fallback_stderr(const LogRecord& record) {
    std::fprintf(stderr, "[%llu][%s][%s][%s:%u] severity=%s code=%s domain=%s errno=%d msg=%s\n",
                 static_cast<unsigned long long>(record.ts_ns), to_string(record.level), record.module.c_str(),
                 record.file.c_str(), record.line, to_string(record.severity), to_string(record.code),
                 to_string(record.domain), record.sys_errno, record.message.c_str());
}

// Normalizes the logical logger name so shm names and filenames remain predictable.
std::string normalize_instance_name(std::string_view instance_name) {
    if (instance_name.empty()) {
        instance_name = kDefaultLoggerInstanceName;
    }

    std::string normalized;
    normalized.reserve(instance_name.size());
    for (char ch : instance_name) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            normalized.push_back(ch);
            continue;
        }
        if (ch >= 'A' && ch <= 'Z') {
            normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
            continue;
        }
        normalized.push_back('_');
    }

    if (normalized.empty()) {
        normalized.assign(kDefaultLoggerInstanceName);
    }
    return normalized;
}

// Builds a process-unique shm name so independent binaries never share one writer ring.
std::string build_shm_name(AccountId account_id, std::string_view instance_name) {
    return "/acct_service_log_" + std::string(instance_name) + "_" + std::to_string(account_id) + "_" +
           std::to_string(static_cast<unsigned long long>(::getpid()));
}

// Preserves the historical account log filename for the main service while isolating other binaries.
std::string build_output_path(const LogConfig& config, AccountId account_id, std::string_view instance_name) {
    if (instance_name == kDefaultLoggerInstanceName) {
        return config.log_dir + "/account_" + std::to_string(account_id) + ".log";
    }
    return config.log_dir + "/" + std::string(instance_name) + "_" + std::to_string(account_id) + ".log";
}

// Copies text into a fixed-size payload field used by BaseCore format decoding.
template <typename Field>
Field make_field(std::string_view text) {
    Field field{};
    const std::size_t copy_size = (text.size() < (field.size() - 1)) ? text.size() : (field.size() - 1);
    if (copy_size > 0) {
        text.copy(field.data(), copy_size);
    }
    field[copy_size] = '\0';
    return field;
}

// Tracks unread payload volume so overwrite-driven loss remains visible via dropped_count().
void account_pending_payload(std::size_t payload_size, std::size_t& pending_fixed_entries,
                             std::size_t& pending_variable_bytes, std::atomic<uint64_t>& dropped) {
    if (payload_size <= base_core_log::kLogMaxPayload) {
        ++pending_fixed_entries;
        if (pending_fixed_entries > kFixedBufferCapacity) {
            dropped.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    pending_variable_bytes += sizeof(base_core_log::LogVarEntryHeader) + payload_size;
    if (pending_variable_bytes > kVariableBufferCapacity) {
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace

struct AsyncLogger::Impl {
    base_core_log::ShmLogger logger{};
    base_core_log::ShmLogConfig shm_config{};
    std::string instance_name{};
    LogLevel min_level = LogLevel::info;
    bool async_enabled = true;
    std::atomic<bool> healthy{false};
    std::atomic<uint64_t> dropped{0};
    std::size_t pending_fixed_entries_ = 0;
    std::size_t pending_variable_bytes_ = 0;
    SpinLock io_lock;

    // Opens a fresh shared-memory ring for subsequent writes after init or drain.
    bool reopen_locked() {
        logger.close();
        if (!base_core_log::init_shm_logger(shm_config, logger)) {
            healthy.store(false, std::memory_order_release);
            return false;
        }

        pending_fixed_entries_ = 0;
        pending_variable_bytes_ = 0;
        healthy.store(true, std::memory_order_release);
        return true;
    }

    // Drains unread log entries to disk and optionally reopens a fresh shm ring.
    bool drain_locked(bool reopen_after_drain) {
        if (!logger.is_open()) {
            healthy.store(false, std::memory_order_release);
            return true;
        }

        // Read and append the current ring before tearing it down to avoid duplicate drains.
        base_core_log::LogReader reader(shm_config.shm_name, shm_config.output_path,
                                        &basecore_log_adapter::project_log_module_mapper);
        if (reader.run() != 0) {
            healthy.store(false, std::memory_order_release);
            return false;
        }

        logger.close();
        (void)shm::ShmGenericWriter::unlink(shm_config.shm_name);
        pending_fixed_entries_ = 0;
        pending_variable_bytes_ = 0;

        if (!reopen_after_drain) {
            healthy.store(false, std::memory_order_release);
            return true;
        }
        return reopen_locked();
    }

    // Writes one fully structured record into BaseCore shared memory.
    bool write_record_locked(const LogRecord& record) {
        auto* writer = logger.writer();
        if (!writer || !writer->is_attached()) {
            healthy.store(false, std::memory_order_release);
            return false;
        }

        const std::string_view file = record.file.empty() ? std::string_view("<unknown>") : record.file.view();
        const auto file_field = make_field<basecore_log_adapter::FileField>(file);
        const auto severity_field = make_field<basecore_log_adapter::SeverityField>(to_string(record.severity));
        const auto code_field = make_field<basecore_log_adapter::CodeField>(to_string(record.code));
        const auto domain_field = make_field<basecore_log_adapter::DomainField>(to_string(record.domain));
        const auto message_field = make_field<basecore_log_adapter::MessageField>(
            record.message.empty() ? std::string_view("") : record.message.view());
        const auto module = basecore_log_adapter::project_log_module_from_name(record.module.view());

        // Always use the structured format entry so reader output remains stable across all call sites.
        const bool ok = base_core_log::log_write_fmt(
            *writer, to_basecore_level(record.level), base_core_log::rdtsc(), static_cast<uint8_t>(module),
            basecore_log_adapter::ProjectLogFormat::kRecordLogId,
            basecore_log_adapter::ProjectLogFormat::kRecordLogPayloadSize, file_field, record.line, severity_field,
            code_field, domain_field, record.sys_errno, message_field);
        if (!ok) {
            healthy.store(false, std::memory_order_release);
            return false;
        }

        account_pending_payload(basecore_log_adapter::ProjectLogFormat::kRecordLogPayloadSize, pending_fixed_entries_,
                                pending_variable_bytes_, dropped);
        return true;
    }
};

AsyncLogger::~AsyncLogger() noexcept { shutdown(); }

// Preserves the historical service logger entrypoint with the default instance name.
bool AsyncLogger::init(const LogConfig& config, AccountId account_id) {
    return init(config, account_id, kDefaultLoggerInstanceName);
}

// Configures the shared-memory logger for one logical process instance.
bool AsyncLogger::init(const LogConfig& config, AccountId account_id, std::string_view instance_name) {
    shutdown();

    auto state = std::unique_ptr<Impl>(new (std::nothrow) Impl());
    if (!state) {
        return false;
    }

    state->min_level = parse_level(config.log_level);
    state->async_enabled = config.async_logging;
    state->instance_name = normalize_instance_name(instance_name);

    std::error_code ec;
    std::filesystem::create_directories(config.log_dir, ec);
    if (ec) {
        return false;
    }

    state->shm_config.shm_name = build_shm_name(account_id, state->instance_name);
    state->shm_config.per_thread = false;
    state->shm_config.output_path = build_output_path(config, account_id, state->instance_name);

    {
        LockGuard<SpinLock> guard(state->io_lock);

        // Force the format object into the final link so LogReader always has a decoder.
        if (!basecore_log_adapter::ProjectLogFormat::register_formats()) {
            return false;
        }

        // Remove any leftover object from an unclean previous exit before creating a new ring.
        (void)shm::ShmGenericWriter::unlink(state->shm_config.shm_name);
        if (!state->reopen_locked()) {
            return false;
        }
    }

    impl_ = std::move(state);
    return true;
}

void AsyncLogger::shutdown() noexcept {
    std::unique_ptr<Impl> state = std::move(impl_);
    if (!state) {
        return;
    }

    {
        LockGuard<SpinLock> guard(state->io_lock);
        (void)state->drain_locked(false);
        state->logger.close();
        if (!state->shm_config.shm_name.empty()) {
            (void)shm::ShmGenericWriter::unlink(state->shm_config.shm_name);
        }
        state->healthy.store(false, std::memory_order_release);
    }
}

// Forces the current shared-memory ring to be decoded into the configured log file.
bool AsyncLogger::flush(uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!impl_) {
        return true;
    }

    LockGuard<SpinLock> guard(impl_->io_lock);
    return impl_->drain_locked(true);
}

// Converts one LogRecord into the shared-memory format used by BaseCore.
bool AsyncLogger::log(const LogRecord& record) {
    if (!impl_) {
        if (should_sync_fallback(record.level)) {
            write_fallback_stderr(record);
        }
        return false;
    }

    if (static_cast<uint8_t>(record.level) < static_cast<uint8_t>(impl_->min_level)) {
        return true;
    }

    LockGuard<SpinLock> guard(impl_->io_lock);
    if (!impl_->write_record_locked(record)) {
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
        if (should_sync_fallback(record.level)) {
            write_fallback_stderr(record);
        }
        return false;
    }

    if (!impl_->async_enabled && !impl_->drain_locked(true)) {
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
        if (should_sync_fallback(record.level)) {
            write_fallback_stderr(record);
        }
        return false;
    }

    return true;
}

uint64_t AsyncLogger::dropped_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    return impl_->dropped.load(std::memory_order_relaxed);
}

bool AsyncLogger::healthy() const noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->healthy.load(std::memory_order_relaxed);
}

namespace {

AsyncLogger& global_logger() {
    static AsyncLogger logger;
    return logger;
}

}  // namespace

bool init_logger(const LogConfig& config, AccountId account_id) { return global_logger().init(config, account_id); }

// Exposes per-process logger naming to binaries such as gateway that share the common logger API.
bool init_logger(const LogConfig& config, AccountId account_id, std::string_view instance_name) {
    return global_logger().init(config, account_id, instance_name);
}

void shutdown_logger() { global_logger().shutdown(); }

bool flush_logger(uint32_t timeout_ms) { return global_logger().flush(timeout_ms); }

bool logger_healthy() noexcept { return global_logger().healthy(); }

uint64_t logger_dropped_count() noexcept { return global_logger().dropped_count(); }

void log_message(LogLevel level, std::string_view module, std::string_view file, uint32_t line,
                 std::string_view message, const ErrorStatus* status, int sys_errno) {
    LogRecord record;
    record.ts_ns = now_ns();
    record.level = level;
    record.sys_errno = sys_errno;
    record.line = line;
    record.module.assign(module);
    record.file.assign(file);
    record.message.assign(message);

    if (status) {
        record.domain = status->domain;
        record.code = status->code;
        record.severity = classify(status->domain, status->code).severity;
        if (record.sys_errno == 0) {
            record.sys_errno = status->sys_errno;
        }
    } else {
        const ErrorStatus& last = last_error();
        if (!last.ok()) {
            record.domain = last.domain;
            record.code = last.code;
            record.severity = classify(last.domain, last.code).severity;
            if (record.sys_errno == 0) {
                record.sys_errno = last.sys_errno;
            }
        }
    }

    (void)global_logger().log(record);
}

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug:
            return "DEBUG";
        case LogLevel::info:
            return "INFO";
        case LogLevel::warn:
            return "WARN";
        case LogLevel::error:
            return "ERROR";
        case LogLevel::fatal:
            return "FATAL";
    }
    return "INFO";
}

}  // namespace acct_service
