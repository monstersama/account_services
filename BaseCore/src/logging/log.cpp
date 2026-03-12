#include "logging/log.hpp"
#include "logging/log_format_registry.hpp"
#include "shm/shm_generic.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>

namespace base_core_log {

namespace {

inline uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

// TSC 校准：测量 TSC 频率（GHz）
double calibrate_tsc_freq() {
    const uint64_t t0_tsc = rdtsc();
    const uint64_t t0_ns = now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t t1_tsc = rdtsc();
    const uint64_t t1_ns = now_ns();
    const uint64_t tsc_delta = t1_tsc - t0_tsc;
    const uint64_t ns_delta = t1_ns - t0_ns;
    if (ns_delta == 0) return 2.0;  // 默认假设 2 GHz
    return static_cast<double>(tsc_delta) / static_cast<double>(ns_delta);
}

}  // namespace

// 共享内存布局：[LogShmHeader][LogEntry buffer]
namespace {

}  // namespace

void log_write_failed(const char* reason) noexcept {
    if (reason) {
        shm::log_error(std::string(reason));
    }
}

void LogBufferWriter::attach(LogShmHeader* header, LogEntry* buffer, std::size_t capacity,
                            uint8_t* buffer_var, std::size_t buffer_var_size) noexcept {
    header_ = header;
    buffer_ = buffer;
    capacity_ = static_cast<uint32_t>(capacity);
    write_index_ = header_ ? header_->write_index.load(std::memory_order_relaxed) : 0;
    buffer_var_ = buffer_var;
    buffer_var_size_ = static_cast<uint32_t>(buffer_var_size);
    write_index_var_ = header_ ? header_->write_index_var.load(std::memory_order_relaxed) : 0;
}

bool LogBufferWriter::try_push(const LogEntry& e) noexcept {
    if (!buffer_ || capacity_ == 0) return false;
    const uint32_t idx = write_index_ % capacity_;
    buffer_[idx] = e;
    write_index_ = (idx + 1) % capacity_;
    header_->write_index.store(write_index_, std::memory_order_release);
    return true;
}

bool LogBufferWriter::try_push_variable(uint64_t ts_tsc, uint8_t type_id, uint8_t level,
                                        uint8_t msg_id, uint8_t module_id,
                                        const void* payload, uint16_t payload_size) noexcept {
    if (!buffer_var_ || buffer_var_size_ == 0 || !payload) return false;
    const std::size_t entry_size = sizeof(LogVarEntryHeader) + payload_size;
    if (entry_size > buffer_var_size_) return false;

    uint32_t idx = write_index_var_ % buffer_var_size_;
    if (idx + entry_size > buffer_var_size_) {
        idx = 0;
    }

    LogVarEntryHeader hdr{};
    hdr.ts_tsc = ts_tsc;
    hdr.type_id = type_id;
    hdr.level = level;
    hdr.msg_id = msg_id;
    hdr.module_id = module_id;
    hdr.payload_size = payload_size;

    std::memcpy(buffer_var_ + idx, &hdr, sizeof(LogVarEntryHeader));
    std::memcpy(buffer_var_ + idx + sizeof(LogVarEntryHeader), payload, payload_size);

    write_index_var_ = static_cast<uint32_t>((idx + entry_size) % buffer_var_size_);
    header_->write_index_var.store(write_index_var_, std::memory_order_release);
    return true;
}

bool LogBufferWriter::try_push_variable_str(uint64_t ts_tsc, uint8_t level, uint8_t module_id,
                                            const char* str, std::size_t len) noexcept {
    if (!str || !buffer_var_ || buffer_var_size_ == 0) return false;
    const uint16_t payload_size = static_cast<uint16_t>(len + 1);
    if (payload_size > buffer_var_size_ - sizeof(LogVarEntryHeader)) return false;

    uint32_t idx = write_index_var_ % buffer_var_size_;
    const std::size_t entry_size = sizeof(LogVarEntryHeader) + payload_size;
    if (idx + entry_size > buffer_var_size_) {
        idx = 0;
    }

    LogVarEntryHeader hdr{};
    hdr.ts_tsc = ts_tsc;
    hdr.type_id = static_cast<uint8_t>(LogTypeId::MsgString);
    hdr.level = level;
    hdr.msg_id = 0;
    hdr.module_id = module_id;
    hdr.payload_size = payload_size;

    std::memcpy(buffer_var_ + idx, &hdr, sizeof(LogVarEntryHeader));
    std::memcpy(buffer_var_ + idx + sizeof(LogVarEntryHeader), str, len);
    buffer_var_[idx + sizeof(LogVarEntryHeader) + len] = '\0';

    write_index_var_ = static_cast<uint32_t>((idx + entry_size) % buffer_var_size_);
    header_->write_index_var.store(write_index_var_, std::memory_order_release);
    return true;
}

ShmLogger::~ShmLogger() noexcept { close(); }

bool ShmLogger::init(const ShmLogConfig& config) {
    close();
    const std::string& shm_name = config.shm_name;
    const std::size_t layout_size = sizeof(LogShmHeader) +
        config.buffer_capacity * sizeof(LogEntry) + ShmLogger::kBufferVarSize;
    if (!shm::create_if_not_exists(shm_name, layout_size)) {
        return false;
    }
    void* ptr = writer_.open(shm_name, layout_size);
    if (!ptr) return false;
    layout_ptr_ = ptr;
    layout_size_ = layout_size;

    auto* hdr = static_cast<LogShmHeader*>(ptr);
    auto* base = static_cast<char*>(ptr) + sizeof(LogShmHeader);
    auto* buf = reinterpret_cast<LogEntry*>(base);
    auto* buf_var = reinterpret_cast<uint8_t*>(base + config.buffer_capacity * sizeof(LogEntry));

    if (hdr->magic != 0x4C4F4753) {
        hdr->magic = 0x4C4F4753;
        hdr->version = 3;
        hdr->ref_tsc = rdtsc();
        hdr->ref_ns = now_ns();
        hdr->tsc_freq_ghz = calibrate_tsc_freq();
        hdr->write_index.store(0, std::memory_order_relaxed);
        hdr->write_index_var.store(0, std::memory_order_relaxed);
        hdr->buffer_var_size = static_cast<uint32_t>(ShmLogger::kBufferVarSize);
    }
    uint8_t* var_buf = (hdr->version == 3 && layout_size >= ShmLogger::kLayoutSize)
                           ? buf_var
                           : nullptr;
    std::size_t var_size = var_buf ? ShmLogger::kBufferVarSize : 0;
    writer_impl_.attach(hdr, buf, config.buffer_capacity, var_buf, var_size);
    return true;
}

void ShmLogger::close() noexcept {
    writer_impl_.attach(nullptr, nullptr, 0, nullptr, 0);
    layout_ptr_ = nullptr;
    layout_size_ = 0;
    writer_.close();
}

LogBufferWriter* ShmLogger::writer() noexcept {
    return layout_ptr_ ? &writer_impl_ : nullptr;
}

const LogBufferWriter* ShmLogger::writer() const noexcept {
    return layout_ptr_ ? &writer_impl_ : nullptr;
}

LogShmHeader* ShmLogger::header() noexcept {
    return static_cast<LogShmHeader*>(layout_ptr_);
}

const LogShmHeader* ShmLogger::header() const noexcept {
    return static_cast<const LogShmHeader*>(layout_ptr_);
}

bool ShmLogger::is_open() const noexcept { return layout_ptr_ != nullptr; }

bool init_shm_logger(const ShmLogConfig& config, ShmLogger& logger) {
    if (!logger.init(config)) {
        shm::log_error("init_shm_logger: failed to init ShmLogger shm_name=" + config.shm_name);
        return false;
    }
    auto* w = logger.writer();
    if (!w || !w->is_attached()) {
        shm::log_error("init_shm_logger: writer is null or not attached shm_name=" + config.shm_name);
        logger.close();
        return false;
    }
    return true;
}

// TSC 转纳秒（Unix Epoch）
static uint64_t tsc_to_ns(uint64_t tsc, const LogShmHeader* header) {
    if (!header || header->tsc_freq_ghz <= 0 || header->tsc_freq_ghz > 10.0) {
        return 0;
    }
    const int64_t tsc_delta = static_cast<int64_t>(tsc) - static_cast<int64_t>(header->ref_tsc);
    const double ns_delta = static_cast<double>(tsc_delta) / header->tsc_freq_ghz;
    return static_cast<uint64_t>(static_cast<int64_t>(header->ref_ns) + static_cast<int64_t>(ns_delta));
}

// 纳秒转可读时间字符串（YYYY-MM-DD HH:MM:SS.NNNNNNNNN）
static void format_timestamp_ns(uint64_t ns, char* buf, std::size_t buf_size) {
    if (buf_size < 36) return;  // YYYY-MM-DD HH:MM:SS.NNNNNNNNN + null
    const time_t sec = static_cast<time_t>(ns / 1'000'000'000ULL);
    const uint32_t nsec = static_cast<uint32_t>(ns % 1'000'000'000ULL);
    struct tm tm;
    localtime_r(&sec, &tm);
    std::snprintf(buf, buf_size, "%04d-%02d-%02d %02d:%02d:%02d.%09u",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, nsec);
}

void decode_log_entry(const LogEntry& e, const LogShmHeader* header, char* buf, std::size_t buf_size,
                      ModuleNameMapper module_mapper) {
    if (buf_size < 64) return;

    char time_buf[36];
    const uint64_t ns = tsc_to_ns(e.ts_tsc, header);
    format_timestamp_ns(ns, time_buf, sizeof(time_buf));

    const char* mod = (e.module_id != 0 && module_mapper)
        ? module_mapper(e.module_id)
        : "unknown";
    int n = 0;

    switch (static_cast<LogTypeId>(e.type_id)) {
        case LogTypeId::MsgFormat: {
            char msg_buf[256];
            if (base_core_log::LogFormatRegistry::decode(
                    e.msg_id, e.payload, e.payload_size, msg_buf, sizeof(msg_buf))) {
                n = std::snprintf(buf, buf_size, "[%s][%s][%s] %s\n",
                    time_buf, to_string(static_cast<LogLevel>(e.level)), mod, msg_buf);
            } else {
                n = std::snprintf(buf, buf_size, "[%s][%s][%s] [decode error format_id=%u]\n",
                    time_buf, to_string(static_cast<LogLevel>(e.level)), mod, e.msg_id);
            }
            break;
        }
        case LogTypeId::MsgString: {
            char str_buf[kLogMaxPayload];
            const std::size_t copy_len = (static_cast<std::size_t>(e.payload_size) < kLogMaxPayload)
                ? static_cast<std::size_t>(e.payload_size) : (kLogMaxPayload - 1);
            std::memcpy(str_buf, e.payload, copy_len);
            str_buf[copy_len] = '\0';
            n = std::snprintf(buf, buf_size, "[%s][%s][%s] %s\n",
                time_buf, to_string(static_cast<LogLevel>(e.level)), mod, str_buf);
            break;
        }
        default:
            n = std::snprintf(buf, buf_size, "[%s][%s][%s] unknown type_id=%u\n",
                time_buf, to_string(static_cast<LogLevel>(e.level)), mod, e.type_id);
    }
    (void)n;
}

void decode_log_entry_var(const LogVarEntryHeader& hdr, const uint8_t* payload,
                          const LogShmHeader* header, char* buf, std::size_t buf_size,
                          ModuleNameMapper module_mapper) {
    if (!payload || buf_size < 64) return;

    char time_buf[36];
    const uint64_t ns = tsc_to_ns(hdr.ts_tsc, header);
    format_timestamp_ns(ns, time_buf, sizeof(time_buf));

    const char* mod = (hdr.module_id != 0 && module_mapper)
        ? module_mapper(hdr.module_id)
        : "unknown";
    int n = 0;

    switch (static_cast<LogTypeId>(hdr.type_id)) {
        case LogTypeId::MsgFormat: {
            char msg_buf[256];
            if (base_core_log::LogFormatRegistry::decode(
                    hdr.msg_id, payload, hdr.payload_size, msg_buf, sizeof(msg_buf))) {
                n = std::snprintf(buf, buf_size, "[%s][%s][%s] %s\n",
                    time_buf, to_string(static_cast<LogLevel>(hdr.level)), mod, msg_buf);
            } else {
                n = std::snprintf(buf, buf_size, "[%s][%s][%s] [decode error format_id=%u]\n",
                    time_buf, to_string(static_cast<LogLevel>(hdr.level)), mod, hdr.msg_id);
            }
            break;
        }
        case LogTypeId::MsgString: {
            char str_buf[kLogMaxVarPayload + 1];
            const std::size_t copy_len =
                (static_cast<std::size_t>(hdr.payload_size) <= kLogMaxVarPayload)
                    ? static_cast<std::size_t>(hdr.payload_size) : kLogMaxVarPayload;
            std::memcpy(str_buf, payload, copy_len);
            str_buf[copy_len] = '\0';
            n = std::snprintf(buf, buf_size, "[%s][%s][%s] %s\n",
                time_buf, to_string(static_cast<LogLevel>(hdr.level)), mod, str_buf);
            break;
        }
        default:
            n = std::snprintf(buf, buf_size, "[%s][%s][%s] unknown type_id=%u\n",
                time_buf, to_string(static_cast<LogLevel>(hdr.level)), mod, hdr.type_id);
    }
    (void)n;
}

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::debug: return "DEBUG";
        case LogLevel::info: return "INFO";
        case LogLevel::warn: return "WARN";
        case LogLevel::error: return "ERROR";
        case LogLevel::fatal: return "FATAL";
    }
    return "INFO";
}

// ============ LogReader ============
namespace {

constexpr std::size_t kDecodeBufSize = 512;

}  // namespace

LogReader::LogReader(std::string shm_name, std::string output_path, ModuleNameMapper module_mapper)
    : shm_name_(std::move(shm_name)), output_path_(std::move(output_path)),
      module_mapper_(module_mapper) {}

LogReader::~LogReader() {
    close();
}

bool LogReader::init() {
    if (initialized_) {
        return true;
    }

    // 尝试打开 v3 布局
    ptr_ = writer_.open(shm_name_, ShmLogger::kLayoutSize);
    if (!ptr_) {
        // 回退到 v2 布局
        constexpr std::size_t kV2LayoutSize =
            sizeof(LogShmHeader) + ShmLogger::kBufferCapacity * sizeof(LogEntry);
        ptr_ = writer_.open(shm_name_, kV2LayoutSize);
    }
    if (!ptr_) {
        shm::log_error("logReader: failed to open shm name=" + shm_name_);
        return false;
    }

    header_ = static_cast<const LogShmHeader*>(ptr_);
    if (header_->magic != 0x4C4F4753) {
        shm::log_error("logReader: invalid shm magic name=" + shm_name_);
        writer_.close();
        ptr_ = nullptr;
        return false;
    }
    if (header_->version != 2 && header_->version != 3) {
        shm::log_error("logReader: unsupported shm version name=" + shm_name_ +
                       " version=" + std::to_string(header_->version));
        writer_.close();
        ptr_ = nullptr;
        return false;
    }

    const auto* base = static_cast<const char*>(ptr_) + sizeof(LogShmHeader);
    buffer_ = reinterpret_cast<const LogEntry*>(base);

    // v3 变长区
    if (header_->version == 3 && header_->buffer_var_size > 0 &&
        writer_.size() >= sizeof(LogShmHeader) + ShmLogger::kBufferCapacity * sizeof(LogEntry) +
                            static_cast<std::size_t>(header_->buffer_var_size)) {
        buffer_var_ = reinterpret_cast<const uint8_t*>(base + ShmLogger::kBufferCapacity * sizeof(LogEntry));
    }

    // 打开输出文件
    out_.open(output_path_, std::ios::app);
    if (!out_) {
        shm::log_error("logReader: failed to open output path=" + output_path_);
        writer_.close();
        ptr_ = nullptr;
        header_ = nullptr;
        buffer_ = nullptr;
        buffer_var_ = nullptr;
        return false;
    }

    // 初始化读取位置
    last_read_index_ = 0;
    last_read_index_var_ = 0;
    initialized_ = true;

    return true;
}

int LogReader::read_new() {
    if (!initialized_ || !header_ || !buffer_) {
        return -1;
    }

    char buf[kDecodeBufSize];
    int count = 0;
    constexpr std::size_t kCapacity = ShmLogger::kBufferCapacity;

    // 读取固定区新数据
    const uint32_t write_index = header_->write_index.load(std::memory_order_acquire);

    // 处理环形缓冲区的环绕情况
    if (write_index < last_read_index_) {
        // 写者已经环绕了一圈，需要分段读取
        // 先读取 [last_read_index_, kCapacity)
        for (uint32_t i = last_read_index_; i < kCapacity; ++i) {
            decode_log_entry(buffer_[i], header_, buf, sizeof(buf), module_mapper_);
            out_ << buf;
            ++count;
        }
        // 再读取 [0, write_index)
        for (uint32_t i = 0; i < write_index; ++i) {
            decode_log_entry(buffer_[i], header_, buf, sizeof(buf), module_mapper_);
            out_ << buf;
            ++count;
        }
    } else {
        // 正常情况，顺序读取
        for (uint32_t i = last_read_index_; i != write_index;
             i = (i + 1) % static_cast<uint32_t>(kCapacity)) {
            decode_log_entry(buffer_[i], header_, buf, sizeof(buf), module_mapper_);
            out_ << buf;
            ++count;
        }
    }
    last_read_index_ = write_index;

    // 读取变长区新数据
    if (buffer_var_ && header_->version == 3) {
        const uint32_t write_index_var = header_->write_index_var.load(std::memory_order_acquire);

        // 变长区也处理环绕（简化处理：如果发生环绕，重置位置从头读取）
        if (write_index_var < last_read_index_var_) {
            last_read_index_var_ = 0;
        }

        uint32_t pos = last_read_index_var_;
        while (pos < write_index_var) {
            const auto* hdr = reinterpret_cast<const LogVarEntryHeader*>(buffer_var_ + pos);
            const uint8_t* payload = buffer_var_ + pos + sizeof(LogVarEntryHeader);
            const uint16_t payload_size = hdr->payload_size;

            if (pos + sizeof(LogVarEntryHeader) + payload_size > write_index_var) {
                break;
            }

            decode_log_entry_var(*hdr, payload, header_, buf, sizeof(buf), module_mapper_);
            out_ << buf;
            ++count;

            pos += sizeof(LogVarEntryHeader) + payload_size;
        }
        last_read_index_var_ = pos;
    }

    if (count > 0) {
        out_.flush();
    }

    return count;
}

int LogReader::run() {
    // 一次性读取模式：初始化、读取所有、关闭
    if (!init()) {
        return 1;
    }

    // 读取所有现有数据
    int total = 0;
    constexpr std::size_t kCapacity = ShmLogger::kBufferCapacity;

    // 设置读取位置为0，读取全部
    last_read_index_ = 0;
    last_read_index_var_ = 0;

    const uint32_t write_index = header_->write_index.load(std::memory_order_acquire);
    for (uint32_t read_index = 0; read_index != write_index;
         read_index = (read_index + 1) % static_cast<uint32_t>(kCapacity)) {
        char buf[kDecodeBufSize];
        decode_log_entry(buffer_[read_index], header_, buf, sizeof(buf), module_mapper_);
        out_ << buf;
        ++total;
    }

    if (buffer_var_ && header_->version == 3 && header_->buffer_var_size > 0) {
        const uint32_t write_index_var = header_->write_index_var.load(std::memory_order_acquire);
        uint32_t pos = 0;
        char buf[kDecodeBufSize];

        while (pos < write_index_var) {
            const auto* hdr = reinterpret_cast<const LogVarEntryHeader*>(buffer_var_ + pos);
            const uint8_t* payload = buffer_var_ + pos + sizeof(LogVarEntryHeader);
            const uint16_t payload_size = hdr->payload_size;

            if (pos + sizeof(LogVarEntryHeader) + payload_size > write_index_var) {
                break;
            }

            decode_log_entry_var(*hdr, payload, header_, buf, sizeof(buf), module_mapper_);
            out_ << buf;
            ++total;

            pos += sizeof(LogVarEntryHeader) + payload_size;
        }
    }

    out_.flush();
    close();

    return 0;
}

void LogReader::close() {
    if (out_.is_open()) {
        out_.close();
    }
    writer_.close();
    ptr_ = nullptr;
    header_ = nullptr;
    buffer_ = nullptr;
    buffer_var_ = nullptr;
    last_read_index_ = 0;
    last_read_index_var_ = 0;
    initialized_ = false;
}

bool LogReader::is_open() const noexcept {
    return initialized_ && writer_.is_open() && out_.is_open();
}

}  // namespace base_core_log
