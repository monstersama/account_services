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

class logger_queue {
public:
    explicit logger_queue(std::size_t capacity)
        : capacity_(capacity), mask_((capacity > 0) ? (capacity - 1) : 0), buffer_(capacity), ready_(capacity) {
        for (std::size_t i = 0; i < ready_.size(); ++i) {
            ready_[i].store(0, std::memory_order_relaxed);
        }
    }

    bool try_push(const log_record& record) {
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

    bool try_pop(log_record& out) {
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
    std::vector<log_record> buffer_;
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

log_level parse_level(const std::string& level) {
    if (level == "debug") {
        return log_level::debug;
    }
    if (level == "info") {
        return log_level::info;
    }
    if (level == "warn") {
        return log_level::warn;
    }
    if (level == "error") {
        return log_level::error;
    }
    if (level == "fatal") {
        return log_level::fatal;
    }
    return log_level::info;
}

bool should_sync_fallback(log_level level) {
    return level == log_level::error || level == log_level::fatal;
}

void write_fallback_stderr(const log_record& record) {
    std::fprintf(stderr,
        "[%llu][%s][%s][%s:%u] code=%s domain=%s errno=%d msg=%s\n",
        static_cast<unsigned long long>(record.ts_ns), to_string(record.level), record.module.c_str(), record.file.c_str(),
        record.line, to_string(record.code), to_string(record.domain), record.sys_errno, record.message.c_str());
}

}  // namespace

struct async_logger::impl {
    logger_queue* queue = nullptr;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> healthy{false};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> enqueued{0};
    std::atomic<uint64_t> written{0};
    FILE* output = nullptr;
    log_level min_level = log_level::info;
    bool async_enabled = true;
    spinlock io_lock;

    void consume_loop() {
        constexpr std::size_t kBatch = 256;
        uint64_t processed = 0;
        while (running.load(std::memory_order_acquire) || (queue && !queue->empty())) {
            log_record rec;
            bool got = false;
            for (std::size_t i = 0; i < kBatch; ++i) {
                if (!queue || !queue->try_pop(rec)) {
                    break;
                }
                got = true;
                if (output) {
                    std::fprintf(output,
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
                    std::fflush(output);
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (output) {
            std::fflush(output);
        }
        written.store(processed, std::memory_order_release);
    }
};

async_logger::~async_logger() { shutdown(); }

bool async_logger::init(const log_config& config, account_id_t account_id) {
    shutdown();

    auto* state = new (std::nothrow) impl();
    if (!state) {
        return false;
    }

    const std::size_t queue_size = normalize_capacity(config.async_queue_size);
    state->queue = new (std::nothrow) logger_queue(queue_size);
    if (!state->queue) {
        delete state;
        return false;
    }

    state->min_level = parse_level(config.log_level);
    state->async_enabled = config.async_logging;

    std::error_code ec;
    std::filesystem::create_directories(config.log_dir, ec);
    const std::string path = config.log_dir + "/account_" + std::to_string(account_id) + ".log";
    state->output = std::fopen(path.c_str(), "a");
    if (!state->output) {
        delete state->queue;
        delete state;
        return false;
    }

    state->running.store(true, std::memory_order_release);
    state->healthy.store(true, std::memory_order_release);

    try {
        state->worker = std::thread([state]() { state->consume_loop(); });
    } catch (...) {
        state->healthy.store(false, std::memory_order_release);
        std::fclose(state->output);
        delete state->queue;
        delete state;
        return false;
    }

    impl_ = state;
    return true;
}

void async_logger::shutdown() {
    if (!impl_) {
        return;
    }

    impl_->running.store(false, std::memory_order_release);
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    if (impl_->output) {
        std::fflush(impl_->output);
        std::fclose(impl_->output);
    }
    delete impl_->queue;
    delete impl_;
    impl_ = nullptr;
}

bool async_logger::flush(uint32_t timeout_ms) {
    if (!impl_) {
        return true;
    }

    const uint64_t target = impl_->enqueued.load(std::memory_order_acquire);
    const timestamp_ns_t start = now_ns();
    const timestamp_ns_t timeout_ns = static_cast<timestamp_ns_t>(timeout_ms) * 1000000ULL;

    while (impl_->written.load(std::memory_order_acquire) < target) {
        if (now_ns() - start > timeout_ns) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (impl_->output) {
        std::fflush(impl_->output);
    }
    return true;
}

bool async_logger::log(const log_record& record) {
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
        lock_guard<spinlock> guard(impl_->io_lock);
        if (impl_->output) {
            std::fprintf(impl_->output,
                "[%llu][%s][%s][%s:%u] severity=%s code=%s domain=%s errno=%d msg=%s\n",
                static_cast<unsigned long long>(record.ts_ns), to_string(record.level), record.module.c_str(),
                record.file.c_str(), record.line, to_string(record.severity), to_string(record.code),
                to_string(record.domain), record.sys_errno, record.message.c_str());
            std::fflush(impl_->output);
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

uint64_t async_logger::dropped_count() const noexcept {
    if (!impl_) {
        return 0;
    }
    return impl_->dropped.load(std::memory_order_relaxed);
}

bool async_logger::healthy() const noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->healthy.load(std::memory_order_relaxed);
}

namespace {

async_logger& global_logger() {
    static async_logger logger;
    return logger;
}

}  // namespace

bool init_logger(const log_config& config, account_id_t account_id) {
    return global_logger().init(config, account_id);
}

void shutdown_logger() { global_logger().shutdown(); }

bool flush_logger(uint32_t timeout_ms) { return global_logger().flush(timeout_ms); }

bool logger_healthy() noexcept { return global_logger().healthy(); }

uint64_t logger_dropped_count() noexcept { return global_logger().dropped_count(); }

void log_message(log_level level, std::string_view module, std::string_view file, uint32_t line,
    std::string_view message, const error_status* status, int sys_errno) {
    log_record record;
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
        const error_status& last = last_error();
        if (!last.ok()) {
            record.domain = last.domain;
            record.code = last.code;
            record.severity = classify(last.domain, last.code).severity;
        }
    }

    (void)global_logger().log(record);
}

const char* to_string(log_level level) noexcept {
    switch (level) {
        case log_level::debug:
            return "DEBUG";
        case log_level::info:
            return "INFO";
        case log_level::warn:
            return "WARN";
        case log_level::error:
            return "ERROR";
        case log_level::fatal:
            return "FATAL";
    }
    return "INFO";
}

}  // namespace acct_service
