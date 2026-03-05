#include "common/log.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "core/config_manager.hpp"

namespace acct_service {

namespace {

class LoggerQueue {
public:
    explicit LoggerQueue(std::size_t capacity)
        : capacity_(capacity), mask_((capacity > 0) ? (capacity - 1) : 0), buffer_(capacity), ready_(capacity) {
        for (std::size_t i = 0; i < ready_.size(); ++i) {
            ready_[i].store(0, std::memory_order_relaxed);
        }
    }

    bool try_push(const LogRecord& record) {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        for (;;) {
            const uint64_t head = head_.load(std::memory_order_acquire);
            if ((tail - head) >= capacity_) {
                return false;
            }
            if (tail_.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                buffer_[tail & mask_] = record;
                ready_[tail & mask_].store(1, std::memory_order_release);
                return true;
            }
        }
    }

    bool try_pop(LogRecord& out) {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        if (head >= tail_.load(std::memory_order_acquire)) {
            return false;
        }
        const std::size_t index = head & mask_;
        if (ready_[index].load(std::memory_order_acquire) == 0) {
            return false;
        }
        out = buffer_[index];
        ready_[index].store(0, std::memory_order_release);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) >= tail_.load(std::memory_order_acquire);
    }

private:
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::vector<LogRecord> buffer_;
    std::vector<std::atomic<uint8_t>> ready_;
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> tail_{0};
};

std::size_t normalize_capacity(std::size_t requested) {
    if (requested < 1024) {
        requested = 1024;
    }
    std::size_t cap = 1;
    while (cap < requested) {
        cap <<= 1;
    }
    return cap;
}

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

bool should_sync_fallback(LogLevel level) {
    return level == LogLevel::error || level == LogLevel::fatal;
}

void write_fallback_stderr(const LogRecord& record) {
    std::fprintf(stderr,
        "[%llu][%s][%s][%s:%u] code=%s domain=%s errno=%d msg=%s\n",
        static_cast<unsigned long long>(record.ts_ns), to_string(record.level), record.module.c_str(), record.file.c_str(),
        record.line, to_string(record.code), to_string(record.domain), record.sys_errno, record.message.c_str());
}

}  // namespace

struct AsyncLogger::Impl {
    std::unique_ptr<LoggerQueue> queue{};
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> healthy{false};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> enqueued{0};
    std::atomic<uint64_t> written{0};
    std::unique_ptr<FILE, int (*)(FILE*)> output{nullptr, std::fclose};
    LogLevel min_level = LogLevel::info;
    bool async_enabled = true;
    SpinLock io_lock;

    void consume_loop() {
        constexpr std::size_t kBatch = 256;
        uint64_t processed = 0;
        while (running.load(std::memory_order_acquire) || (queue && !queue->empty())) {
            LogRecord rec;
            bool got = false;
            for (std::size_t i = 0; i < kBatch; ++i) {
                if (!queue || !queue->try_pop(rec)) {
                    break;
                }
                got = true;
                if (output) {
                    std::fprintf(output.get(),
                        "[%llu][%s][%s][%s:%u] severity=%s code=%s domain=%s errno=%d msg=%s\n",
                        static_cast<unsigned long long>(rec.ts_ns), to_string(rec.level), rec.module.c_str(),
                        rec.file.c_str(), rec.line, to_string(rec.severity), to_string(rec.code), to_string(rec.domain),
                        rec.sys_errno, rec.message.c_str());
                }
                ++processed;
            }

            if (got) {
                written.store(processed, std::memory_order_release);
                if (output) {
                    std::fflush(output.get());
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (output) {
            std::fflush(output.get());
        }
        written.store(processed, std::memory_order_release);
    }
};

AsyncLogger::~AsyncLogger() noexcept { shutdown(); }

bool AsyncLogger::init(const LogConfig& config, AccountId account_id) {
    shutdown();

    auto state = std::unique_ptr<Impl>(new (std::nothrow) Impl());
    if (!state) {
        return false;
    }

    const std::size_t queue_size = normalize_capacity(config.async_queue_size);
    state->queue.reset(new (std::nothrow) LoggerQueue(queue_size));
    if (!state->queue) {
        return false;
    }

    state->min_level = parse_level(config.log_level);
    state->async_enabled = config.async_logging;

    std::error_code ec;
    std::filesystem::create_directories(config.log_dir, ec);
    const std::string path = config.log_dir + "/account_" + std::to_string(account_id) + ".log";
    state->output.reset(std::fopen(path.c_str(), "a"));
    if (!state->output) {
        return false;
    }

    state->running.store(true, std::memory_order_release);
    state->healthy.store(true, std::memory_order_release);

    try {
        state->worker = std::thread([raw = state.get()]() { raw->consume_loop(); });
    } catch (...) {
        state->healthy.store(false, std::memory_order_release);
        return false;
    }

    impl_ = std::move(state);
    return true;
}

void AsyncLogger::shutdown() noexcept {
    if (!impl_) {
        return;
    }

    impl_->running.store(false, std::memory_order_release);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    if (impl_->output) {
        std::fflush(impl_->output.get());
    }
    impl_.reset();
}

bool AsyncLogger::flush(uint32_t timeout_ms) {
    if (!impl_) {
        return true;
    }

    const uint64_t target = impl_->enqueued.load(std::memory_order_acquire);
    const TimestampNs start = now_monotonic_ns();
    const TimestampNs timeout_ns = static_cast<TimestampNs>(timeout_ms) * 1000000ULL;

    while (impl_->written.load(std::memory_order_acquire) < target) {
        if (now_monotonic_ns() - start > timeout_ns) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (impl_->output) {
        std::fflush(impl_->output.get());
    }
    return true;
}

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

    if (!impl_->async_enabled) {
        LockGuard<SpinLock> guard(impl_->io_lock);
        if (impl_->output) {
            std::fprintf(impl_->output.get(),
                "[%llu][%s][%s][%s:%u] severity=%s code=%s domain=%s errno=%d msg=%s\n",
                static_cast<unsigned long long>(record.ts_ns), to_string(record.level), record.module.c_str(),
                record.file.c_str(), record.line, to_string(record.severity), to_string(record.code),
                to_string(record.domain), record.sys_errno, record.message.c_str());
            std::fflush(impl_->output.get());
            return true;
        }
        write_fallback_stderr(record);
        return false;
    }

    if (!impl_->queue->try_push(record)) {
        impl_->dropped.fetch_add(1, std::memory_order_relaxed);
        if (should_sync_fallback(record.level)) {
            write_fallback_stderr(record);
        }
        return false;
    }
    impl_->enqueued.fetch_add(1, std::memory_order_relaxed);
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

bool init_logger(const LogConfig& config, AccountId account_id) {
    return global_logger().init(config, account_id);
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
