#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace acct_service {
namespace observer_time {

constexpr std::size_t kSecondTextSize = 32;
constexpr std::size_t kTimestampTextSize = 40;

// 将 Unix Epoch 纳秒时间戳格式化为本地时间字符串，便于终端和 CSV 直接阅读。
inline std::string format_unix_time_ns(uint64_t unix_time_ns) {
    const std::time_t seconds = static_cast<std::time_t>(unix_time_ns / 1'000'000'000ULL);
    const uint32_t millis = static_cast<uint32_t>((unix_time_ns % 1'000'000'000ULL) / 1'000'000ULL);

    std::tm local_tm{};
    if (::localtime_r(&seconds, &local_tm) == nullptr) {
        return std::to_string(static_cast<unsigned long long>(unix_time_ns));
    }

    char second_text[kSecondTextSize]{};
    if (std::strftime(second_text, sizeof(second_text), "%Y-%m-%d %H:%M:%S", &local_tm) == 0) {
        return std::to_string(static_cast<unsigned long long>(unix_time_ns));
    }

    char timestamp_text[kTimestampTextSize]{};
    std::snprintf(timestamp_text, sizeof(timestamp_text), "%s.%03u", second_text, millis);
    return timestamp_text;
}

}  // namespace observer_time
}  // namespace acct_service
