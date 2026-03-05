#pragma once

#include <cstdint>
#include <ctime>

#include "common/types.hpp"

namespace acct_service {

// 获取当前纳秒时间戳（CLOCK_REALTIME）
inline TimestampNs now_realtime_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<TimestampNs>(ts.tv_sec) * 1'000'000'000ULL + ts.tv_nsec;
}

// 将 MdTime (HHMMSSMMM) 转换为可读字符串
inline void md_time_to_str(MdTime t, char* buf, std::size_t buf_size);

// 获取当前行情时间 (HHMMSSMMM)
inline MdTime now_md_time();

// 纳秒转微秒
inline uint64_t ns_to_us(TimestampNs ns) { return ns / 1000; }

// 纳秒转毫秒
inline uint64_t ns_to_ms(TimestampNs ns) { return ns / 1'000'000; }

}  // namespace acct_service
