#pragma once

#include <cstdint>
#include <ctime>

#include "common/types.hpp"

namespace acct_service {

// 获取当前纳秒时间戳（CLOCK_REALTIME）
inline timestamp_ns_t now_realtime_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<timestamp_ns_t>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

// 将 md_time_t (HHMMSSMMM) 转换为可读字符串
inline void md_time_to_str(md_time_t t, char* buf, std::size_t buf_size);

// 获取当前行情时间 (HHMMSSMMM)
inline md_time_t now_md_time();

// 纳秒转微秒
inline uint64_t ns_to_us(timestamp_ns_t ns) { return ns / 1000; }

// 纳秒转毫秒
inline uint64_t ns_to_ms(timestamp_ns_t ns) { return ns / 1'000'000; }

}  // namespace acct_service
