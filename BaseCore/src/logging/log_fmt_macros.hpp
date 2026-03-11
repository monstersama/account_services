#pragma once

/**
 * LogFmt 宏定义：按参数个数分发，自动生成 id、payload_size、decode、注册
 * 由 log_demo_format_func.hpp 引用
 */

#include <cstdint>
#include <cstring>
#include <string>

#include "logging/log_format_registry.hpp"

// ============ 参数个数计数 ============
#define LOG_FMT_NARG(...) LOG_FMT_NARG_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define LOG_FMT_NARG_(...) LOG_FMT_NARG__(__VA_ARGS__)
#define LOG_FMT_NARG__(_1, _2, _3, _4, _5, _6, _7, _8, N, ...) N

#define LOG_FMT_CAT(a, b) LOG_FMT_CAT_(a, b)
#define LOG_FMT_CAT_(a, b) a##b

// ============ LogFmt 宏：按参数个数分发 ============
#ifdef LOG_DEMO_FORMAT_FUNC_IMPLEMENTATION
#define LOG_FMT_REGISTER(name) \
    static void __attribute__((constructor)) name##_register(void) { \
        LogFormatRegistry::register_func(name##_id, name##_decode, name##_payload_size); \
    }
#else
#define LOG_FMT_REGISTER(name)
#endif

#define LogFmt(name, ...) LOG_FMT_CAT(LOG_FMT_IMPL_, LOG_FMT_NARG(__VA_ARGS__))(name, __VA_ARGS__)

#define LOG_FMT_IMPL_1(name, T1) \
    static constexpr uint8_t name##_id = 64 + (__COUNTER__ % 192); \
    static constexpr uint16_t name##_payload_size = sizeof(T1); \
    static void name##_decode(const uint8_t* p, uint16_t sz, char* buf, std::size_t buf_sz) { \
        if (sz != sizeof(T1)) { LogFormatRegistry::on_error(name##_id, sz); return; } \
        T1 a1; std::memcpy(&a1, p, sizeof(T1)); \
        std::string s = name(a1); \
        std::strncpy(buf, s.c_str(), buf_sz); buf[buf_sz - 1] = '\0'; \
    } \
    LOG_FMT_REGISTER(name)

#define LOG_FMT_IMPL_2(name, T1, T2) \
    static constexpr uint8_t name##_id = 64 + (__COUNTER__ % 192); \
    static constexpr uint16_t name##_payload_size = sizeof(T1) + sizeof(T2); \
    static void name##_decode(const uint8_t* p, uint16_t sz, char* buf, std::size_t buf_sz) { \
        if (sz != sizeof(T1) + sizeof(T2)) { LogFormatRegistry::on_error(name##_id, sz); return; } \
        T1 a1; T2 a2; \
        std::memcpy(&a1, p, sizeof(T1)); std::memcpy(&a2, p + sizeof(T1), sizeof(T2)); \
        std::string s = name(a1, a2); \
        std::strncpy(buf, s.c_str(), buf_sz); buf[buf_sz - 1] = '\0'; \
    } \
    LOG_FMT_REGISTER(name)

#define LOG_FMT_IMPL_3(name, T1, T2, T3) \
    static constexpr uint8_t name##_id = 64 + (__COUNTER__ % 192); \
    static constexpr uint16_t name##_payload_size = sizeof(T1) + sizeof(T2) + sizeof(T3); \
    static void name##_decode(const uint8_t* p, uint16_t sz, char* buf, std::size_t buf_sz) { \
        if (sz != name##_payload_size) { LogFormatRegistry::on_error(name##_id, sz); return; } \
        T1 a1; T2 a2; T3 a3; \
        std::memcpy(&a1, p, sizeof(T1)); std::memcpy(&a2, p + sizeof(T1), sizeof(T2)); \
        std::memcpy(&a3, p + sizeof(T1) + sizeof(T2), sizeof(T3)); \
        std::string s = name(a1, a2, a3); \
        std::strncpy(buf, s.c_str(), buf_sz); buf[buf_sz - 1] = '\0'; \
    } \
    LOG_FMT_REGISTER(name)

#define LOG_FMT_IMPL_4(name, T1, T2, T3, T4) \
    static constexpr uint8_t name##_id = 64 + (__COUNTER__ % 192); \
    static constexpr uint16_t name##_payload_size = sizeof(T1) + sizeof(T2) + sizeof(T3) + sizeof(T4); \
    static void name##_decode(const uint8_t* p, uint16_t sz, char* buf, std::size_t buf_sz) { \
        if (sz != name##_payload_size) { LogFormatRegistry::on_error(name##_id, sz); return; } \
        T1 a1; T2 a2; T3 a3; T4 a4; \
        std::memcpy(&a1, p, sizeof(T1)); std::memcpy(&a2, p + sizeof(T1), sizeof(T2)); \
        std::memcpy(&a3, p + sizeof(T1) + sizeof(T2), sizeof(T3)); \
        std::memcpy(&a4, p + sizeof(T1) + sizeof(T2) + sizeof(T3), sizeof(T4)); \
        std::string s = name(a1, a2, a3, a4); \
        std::strncpy(buf, s.c_str(), buf_sz); buf[buf_sz - 1] = '\0'; \
    } \
    LOG_FMT_REGISTER(name)

#define LOG_FMT_IMPL_5(name, T1, T2, T3, T4, T5) \
    static constexpr uint8_t name##_id = 64 + (__COUNTER__ % 192); \
    static constexpr uint16_t name##_payload_size = sizeof(T1) + sizeof(T2) + sizeof(T3) + sizeof(T4) + sizeof(T5); \
    static void name##_decode(const uint8_t* p, uint16_t sz, char* buf, std::size_t buf_sz) { \
        if (sz != name##_payload_size) { LogFormatRegistry::on_error(name##_id, sz); return; } \
        T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; \
        std::memcpy(&a1, p, sizeof(T1)); std::memcpy(&a2, p + sizeof(T1), sizeof(T2)); \
        std::memcpy(&a3, p + sizeof(T1) + sizeof(T2), sizeof(T3)); \
        std::memcpy(&a4, p + sizeof(T1) + sizeof(T2) + sizeof(T3), sizeof(T4)); \
        std::memcpy(&a5, p + sizeof(T1) + sizeof(T2) + sizeof(T3) + sizeof(T4), sizeof(T5)); \
        std::string s = name(a1, a2, a3, a4, a5); \
        std::strncpy(buf, s.c_str(), buf_sz); buf[buf_sz - 1] = '\0'; \
    } \
    LOG_FMT_REGISTER(name)

#define LOG_FMT_IMPL_6(name, T1, T2, T3, T4, T5, T6) \
    static constexpr uint8_t name##_id = 64 + (__COUNTER__ % 192); \
    static constexpr uint16_t name##_payload_size = sizeof(T1) + sizeof(T2) + sizeof(T3) + sizeof(T4) + sizeof(T5) + sizeof(T6); \
    static void name##_decode(const uint8_t* p, uint16_t sz, char* buf, std::size_t buf_sz) { \
        if (sz != name##_payload_size) { LogFormatRegistry::on_error(name##_id, sz); return; } \
        T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; \
        std::memcpy(&a1, p, sizeof(T1)); std::memcpy(&a2, p + sizeof(T1), sizeof(T2)); \
        std::memcpy(&a3, p + sizeof(T1) + sizeof(T2), sizeof(T3)); \
        std::memcpy(&a4, p + sizeof(T1) + sizeof(T2) + sizeof(T3), sizeof(T4)); \
        std::memcpy(&a5, p + sizeof(T1) + sizeof(T2) + sizeof(T3) + sizeof(T4), sizeof(T5)); \
        std::memcpy(&a6, p + sizeof(T1) + sizeof(T2) + sizeof(T3) + sizeof(T4) + sizeof(T5), sizeof(T6)); \
        std::string s = name(a1, a2, a3, a4, a5, a6); \
        std::strncpy(buf, s.c_str(), buf_sz); buf[buf_sz - 1] = '\0'; \
    } \
    LOG_FMT_REGISTER(name)
