#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace base_core_log {

/**
 * 格式函数注册表：format_func_id -> (decode 函数, expected_payload_size)
 * 写端通过 LogFmt 宏注册，logReader 启动时静态初始化完成注册，decode 时查表调用
 */
class LogFormatRegistry {
public:
    using DecodeFunc = void (*)(const uint8_t* payload, uint16_t size, char* buf, std::size_t buf_size);

    static uint8_t next_id() noexcept;
    static bool register_func(uint8_t id, DecodeFunc decode, uint16_t expected_payload_size);
    static void on_error(uint8_t id, uint16_t actual_size) noexcept;
    static bool decode(uint8_t id, const uint8_t* payload, uint16_t size, char* buf, std::size_t buf_size);

private:
    struct Entry {
        DecodeFunc decode = nullptr;
        uint16_t expected_payload_size = 0;
    };
    static std::unordered_map<uint8_t, Entry>& registry();
    static std::mutex& mutex();
};

// 兼容性别名（逐步迁移后可移除）
using log_format_registry = LogFormatRegistry;

}  // namespace base_core_log
