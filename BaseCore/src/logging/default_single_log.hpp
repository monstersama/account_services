#pragma once

#include <string>

#include "logging/log.hpp"

namespace base_core_log {

// 默认单实例日志器，供其他模块方便获取 writer 或直接写字符串日志。
class DefaultSingleLog {
public:
    DefaultSingleLog(const DefaultSingleLog&) = delete;
    DefaultSingleLog& operator=(const DefaultSingleLog&) = delete;

    // 获取全局单例
    static DefaultSingleLog& instance() noexcept;

    // 使用默认配置初始化
    bool init();

    // 使用自定义配置初始化
    bool init(const ShmLogConfig& config);

    // 关闭并释放资源
    void shutdown() noexcept;

    bool is_initialized() const noexcept;

    // 获取当前使用的配置（用于 LogReader 等）
    const ShmLogConfig& config() const noexcept { return config_; }

    // 获取底层 writer（用于 LOG_FMT / LOG_STR）
    LogBufferWriter* writer() noexcept;

    // 便捷接口：写入字符串日志
    bool log_str(LogLevel level, uint8_t module_id, const char* msg, std::size_t len);
    bool log_str(LogLevel level, uint8_t module_id, std::string_view msg);

    // 便捷接口：写入格式化日志（对应 LOG_FMT）
    // 说明：format_func_id/payload_size 通常来自 format_name##_id / format_name##_payload_size
    template <typename... Args>
    bool log_fmt(LogLevel level, uint8_t module_id, uint8_t format_func_id, uint16_t payload_size,
                 const Args&... args) {
        if (!is_initialized()) {
            if (!init()) return false;
        }
        auto* w = writer();
        if (!w) return false;
        return log_write_fmt(*w, level, rdtsc(), module_id, format_func_id, payload_size, args...);
    }

private:
    DefaultSingleLog() = default;
    ~DefaultSingleLog() noexcept;

    ShmLogConfig config_;
    ShmLogger logger_;
};

// 使用 DefaultSingleLog 写格式化日志的便捷宏（对应 LOG_FMT）
// 示例：
//   LOG_FMT_DEFAULT(LogLevel::info, Module::module1, LogFormatFunc::format1, 3, 2);
#define LOG_FMT_DEFAULT(level, module_enum, format_name, ...)                            \
    base_core_log::DefaultSingleLog::instance().log_fmt(                                \
        (level),                                                                        \
        static_cast<uint8_t>(module_enum),                                              \
        format_name##_id,                                                               \
        format_name##_payload_size,                                                     \
        ##__VA_ARGS__)

// 使用 DefaultSingleLog 写字符串日志的便捷宏（对应 LOG_STR）
// 示例：
//   LOG_STR_DEFAULT(LogLevel::info, Module::module1, "hello");
#define LOG_STR_DEFAULT(level, module_enum, cstr)                                        \
    base_core_log::DefaultSingleLog::instance().log_str(                                \
        (level),                                                                        \
        static_cast<uint8_t>(module_enum),                                              \
        (cstr))

}  // namespace base_core_log

