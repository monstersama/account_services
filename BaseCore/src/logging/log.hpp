#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
 
#include "shm/shm_generic.hpp"

namespace base_core_log {

// 日志级别（与现有 LogLevel 兼容）
enum class LogLevel : uint8_t {
    debug = 0,
    info,
    warn,
    error,
    fatal,
};

// 日志类型 ID：编译期注册，用于变长 payload 解码
enum class LogTypeId : uint8_t {
    MsgFormat = 6,     // format_func_id + payload（LogFormatFunc）
    MsgString = 7,     // payload 中直接存字符串
};

// payload 最大字节数
constexpr std::size_t kLogMaxPayload = 32;
constexpr std::size_t kLogMaxVarPayload = 1024;

// 二进制日志条目（64 字节 cache line 对齐）
// ts_tsc: TSC 时间戳，logReader 转换为可读格式
struct alignas(64) LogEntry {
    uint64_t ts_tsc = 0;   // TSC 值（RDTSC），非纳秒
    uint8_t type_id = 0;   // LogTypeId
    uint8_t level = 0;     // LogLevel
    uint8_t msg_id = 0;    // logDemo Module 消息 ID
    uint8_t module_id = 0;  // Module 枚举
    uint16_t payload_size = 0;
    uint16_t pad1 = 0;
    uint32_t pad2 = 0;
    uint8_t payload[kLogMaxPayload]{};
};

static_assert(sizeof(LogEntry) == 64, "LogEntry must be 64 bytes");

// ============ TSC 读取（热路径用，避免 clock_gettime 系统调用） ============
inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
#endif
}

// ============ 变长参数日志写入（LOG_FMT 用） ============
namespace detail {

// 将参数打包到 payload（const char* 拷贝字符串内容）
inline void pack_payload(uint8_t* /*dst*/) {}

template <typename T, typename... Rest>
void pack_payload(uint8_t* dst, const T& first, const Rest&... rest) {
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, const char*> ||
                  std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, char*>) {
        const char* str = first;
        if (str) {
            const std::size_t len = std::strlen(str);
            const std::size_t copy_len = (len < kLogMaxVarPayload) ? len : kLogMaxVarPayload;
            std::memcpy(dst, str, copy_len);
            dst[copy_len] = '\0';
            pack_payload(dst + copy_len + 1, rest...);
        } else {
            pack_payload(dst, rest...);
        }
    } else {
        std::memcpy(dst, &first, sizeof(T));
        pack_payload(dst + sizeof(T), rest...);
    }
}

// 计算 payload 总大小（const char* 按 strlen+1）
inline std::size_t payload_size() { return 0; }

inline std::size_t payload_size(const char* str) {
    return str ? (std::strlen(str) + 1) : 0;
}

template <typename T, typename... Rest>
std::size_t payload_size(const T& first, const Rest&... rest) {
    if constexpr (std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, const char*> ||
                  std::is_same_v<std::remove_cv_t<std::remove_reference_t<T>>, char*>) {
        return payload_size(first) + payload_size(rest...);
    } else {
        return sizeof(T) + payload_size(rest...);
    }
}

}  // namespace detail

// 写日志失败时调用（内部使用 shm::log_error）
void log_write_failed(const char* reason) noexcept;

// LOG_FMT 格式函数 / LOG_STR 字符串
#define LOG_FMT(writer, level, module_id, format_name, ...) \
    base_core_log::log_write_fmt((writer), (level), base_core_log::rdtsc(), \
        static_cast<uint8_t>(module_id), format_name##_id, format_name##_payload_size, ##__VA_ARGS__)

#define LOG_STR(writer, level, module_id, str) \
    base_core_log::log_write_str((writer), (level), base_core_log::rdtsc(), \
        static_cast<uint8_t>(module_id), (str), std::char_traits<char>::length(str))

// ============ 共享内存日志配置 ============
struct ShmLogConfig {
    std::string shm_name = "/tmp/log_shm";
    std::size_t buffer_capacity = 4096;
    bool per_thread = true;  // 每线程一文件：shm_name + "_0", "_1", ...
    std::string output_path = "./log_output.txt";  // LogReader 输出文件路径
};

// 变长区 header（32 字节，与 LogEntry 前 32 字节布局兼容）
struct LogVarEntryHeader {
    uint64_t ts_tsc = 0;
    uint8_t type_id = 0;
    uint8_t level = 0;
    uint8_t msg_id = 0;
    uint8_t module_id = 0;
    uint16_t payload_size = 0;
    uint16_t pad = 0;
    uint32_t pad2 = 0;
    uint8_t reserved[12]{};
};

static_assert(sizeof(LogVarEntryHeader) == 32, "LogVarEntryHeader must be 32 bytes");

// ============ 模块名映射函数类型 ===========
// 用于将 module_id 映射为可读的模块名称字符串
using ModuleNameMapper = const char* (*)(uint8_t module_id);

// ============ 共享内存布局头部（TSC 校准参数 + write_index） ============
struct alignas(64) LogShmHeader {
    uint32_t magic = 0x4C4F4753;  // "LOGS"
    uint32_t version = 3;         // v3: 双区布局（固定 + 变长）
    uint64_t ref_tsc = 0;         // 校准参考点 TSC
    uint64_t ref_ns = 0;          // 校准参考点纳秒（Unix Epoch）
    double tsc_freq_ghz = 0;      // TSC 频率 GHz，用于 TSC -> ns 转换
    uint8_t reserved[8]{};
    std::atomic<uint32_t> write_index{0};      // 固定区 slot 下标
    std::atomic<uint32_t> write_index_var{0};  // 变长区字节偏移
    uint32_t buffer_var_size = 0;              // 变长区总字节数
};

static_assert(sizeof(LogShmHeader) == 64, "LogShmHeader must be 64 bytes");

// ============ 直接写 buffer 的 writer（无 tail 检查，满时覆盖） ============
class LogBufferWriter {
public:
    LogBufferWriter() = default;

    void attach(LogShmHeader* header, LogEntry* buffer, std::size_t capacity,
               uint8_t* buffer_var, std::size_t buffer_var_size) noexcept;

    // 热路径：直接写 slot + 一次 release store，无 tail load（payload <= 32）
    bool try_push(const LogEntry& e) noexcept;

    // 变长路径：payload > 32，先记录长度再按长度拷贝
    bool try_push_variable(uint64_t ts_tsc, uint8_t type_id, uint8_t level, uint8_t msg_id,
                           uint8_t module_id, const void* payload, uint16_t payload_size) noexcept;
    // 字符串特化：拷贝 len+1 字节（含 '\0'）
    bool try_push_variable_str(uint64_t ts_tsc, uint8_t level, uint8_t module_id,
                               const char* str, std::size_t len) noexcept;

    bool is_attached() const noexcept { return buffer_ != nullptr; }

private:
    LogShmHeader* header_ = nullptr;
    LogEntry* buffer_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t write_index_ = 0;  // 线程局部缓存，无需原子 load
    uint8_t* buffer_var_ = nullptr;
    uint32_t buffer_var_size_ = 0;
    uint32_t write_index_var_ = 0;  // 变长区字节偏移
};

// ============ 共享内存日志器 ============
class ShmLogger {
public:
    static constexpr std::size_t kBufferCapacity = 4096;
    static constexpr std::size_t kBufferVarSize = 262144;  // 256KB 变长区

    ShmLogger() = default;
    ~ShmLogger() noexcept;

    ShmLogger(const ShmLogger&) = delete;
    ShmLogger& operator=(const ShmLogger&) = delete;

    // 创建或打开日志共享内存（写端）
    bool init(const ShmLogConfig& config);

    // 关闭
    void close() noexcept;

    // 获取 writer（用于 LOG_TEXT / LOG_FMT / LOG_STR）
    LogBufferWriter* writer() noexcept;
    const LogBufferWriter* writer() const noexcept;

    bool is_open() const noexcept;

    LogShmHeader* header() noexcept;
    const LogShmHeader* header() const noexcept;

    // 本实例的线程 ID（per_thread 时有效）
    std::size_t thread_id() const noexcept { return thread_id_; }

    static constexpr std::size_t kLayoutSize =
        sizeof(LogShmHeader) + kBufferCapacity * sizeof(LogEntry) + kBufferVarSize;

private:
    shm::ShmGenericWriter writer_;
    void* layout_ptr_ = nullptr;
    std::size_t layout_size_ = 0;
    std::size_t thread_id_ = 0;
    LogBufferWriter writer_impl_;
};

// 解码：将 LogEntry 格式化为字符串，ts_tsc 转为可读时间
// header 提供 TSC 校准参数，module_mapper 用于将 module_id 映射为模块名
void decode_log_entry(const LogEntry& e, const LogShmHeader* header, char* buf, std::size_t buf_size,
                      ModuleNameMapper module_mapper);

// 解码变长 entry（LogVarEntryHeader + payload）
void decode_log_entry_var(const LogVarEntryHeader& hdr, const uint8_t* payload,
                          const LogShmHeader* header, char* buf, std::size_t buf_size,
                          ModuleNameMapper module_mapper);

const char* to_string(LogLevel level) noexcept;

/// 初始化共享内存日志器，成功返回 true，失败返回 false
bool init_shm_logger(const ShmLogConfig& config, ShmLogger& logger);

// ============ 共享内存日志读取器 ============
class LogReader {
public:
    LogReader(std::string shm_name, std::string output_path, ModuleNameMapper module_mapper = nullptr);
    ~LogReader();

    // 初始化：打开共享内存和输出文件，返回 true 成功
    bool init();

    // 读取新数据（只读取上次位置之后的新日志），返回读取的条目数
    int read_new();

    // 一次性读取所有数据（兼容旧用法），返回 0 成功，1 失败
    int run();

    // 关闭资源
    void close();

    // 是否已初始化
    bool is_open() const noexcept;

private:
    std::string shm_name_;
    std::string output_path_;
    ModuleNameMapper module_mapper_;

    // 持续监控模式的状态
    shm::ShmGenericWriter writer_;
    std::ofstream out_;
    void* ptr_ = nullptr;
    const LogShmHeader* header_ = nullptr;
    const LogEntry* buffer_ = nullptr;
    const uint8_t* buffer_var_ = nullptr;
    uint32_t last_read_index_ = 0;
    uint32_t last_read_index_var_ = 0;
    bool initialized_ = false;
};

template <typename Writer, typename... Args>
bool log_write_fmt(Writer& w, LogLevel level, uint64_t ts, uint8_t module_id,
                  uint8_t format_func_id, uint16_t /*payload_size*/, const Args&... args) {
    const std::size_t actual_size = detail::payload_size(args...);
    const uint16_t size_to_use = static_cast<uint16_t>(
        (actual_size <= kLogMaxVarPayload) ? actual_size : kLogMaxVarPayload);

    if (size_to_use <= kLogMaxPayload) {
        LogEntry e{};
        e.ts_tsc = ts;
        e.type_id = static_cast<uint8_t>(LogTypeId::MsgFormat);
        e.level = static_cast<uint8_t>(level);
        e.msg_id = format_func_id;
        e.module_id = module_id;
        e.payload_size = size_to_use;
        detail::pack_payload(e.payload, args...);
        if (!w.try_push(e)) {
            log_write_failed("log_write_fmt: try_push failed");
            return false;
        }
        return true;
    }
    alignas(64) uint8_t buf[kLogMaxVarPayload];
    detail::pack_payload(buf, args...);
    if (!w.try_push_variable(ts, static_cast<uint8_t>(LogTypeId::MsgFormat),
                             static_cast<uint8_t>(level), format_func_id, module_id,
                             buf, size_to_use)) {
        log_write_failed("log_write_fmt: try_push_variable failed");
        return false;
    }
    return true;
}

// 记录字符串：payload 中直接存字符串内容
// len <= 32: 固定 64B slot；len > 32: 变长区
template <typename Writer>
bool log_write_str(Writer& w, LogLevel level, uint64_t ts, uint8_t module_id,
                  const char* str, std::size_t len) {
    if (!str) return false;
    const std::size_t payload_len = (len < kLogMaxVarPayload) ? len : kLogMaxVarPayload;
    const std::size_t payload_size = payload_len + 1;

    if (payload_size <= kLogMaxPayload) {
        LogEntry e{};
        e.ts_tsc = ts;
        e.type_id = static_cast<uint8_t>(LogTypeId::MsgString);
        e.level = static_cast<uint8_t>(level);
        e.msg_id = 0;
        e.module_id = module_id;
        std::memcpy(e.payload, str, payload_len);
        e.payload[payload_len] = '\0';
        e.payload_size = static_cast<uint16_t>(payload_size);
        if (!w.try_push(e)) {
            log_write_failed("log_write_str: try_push failed");
            return false;
        }
        return true;
    }
    if (!w.try_push_variable_str(ts, static_cast<uint8_t>(level), module_id, str, payload_len)) {
        log_write_failed("log_write_str: try_push_variable_str failed");
        return false;
    }
    return true;
}

template <typename Writer>
bool log_write_str(Writer& w, LogLevel level, uint64_t ts, uint8_t module_id,
                  std::string_view sv) {
    return log_write_str(w, level, ts, module_id, sv.data(), sv.size());
}

}  // namespace base_core_log

// 兼容性别名（逐步迁移后可移除）
namespace BaseCoreLog = base_core_log;
