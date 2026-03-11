#pragma once

/**
 * 日志定义：LogText 文本 ID + LogFormatFunc 格式函数 + LogFmt 宏
 * 新增日志只需在此处添加，LogFmt 自动生成 id、payload_size、decode、注册
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "logging/log_fmt_macros.hpp"
#include "logging/log_format_registry.hpp"

namespace base_core_log {
 

// ============ LogFormatFunc：格式函数 + LogFmt ============
class LogFormatFunc {
public:
    static std::string format1(const int a, const int b) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "a: %d, b: %d", a, b);
        return std::string(buf);
    }
    LogFmt(format1, int, int)

    static std::string format2(const int a) {
        return "abc" + std::to_string(a);
    }
    LogFmt(format2, int)
};

}  // namespace base_core_log
