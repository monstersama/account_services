#include "common/time_utils.hpp"

#include <cstdio>
#include <ctime>

namespace acct {

void md_time_to_str(md_time_t t, char* buf, std::size_t buf_size) {
    if (buf_size < 13) {  // HH:MM:SS.MMM 需要12字符 + null
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return;
    }

    // md_time_t 格式: HHMMSSMMM (小时、分钟、秒、毫秒)
    uint32_t hours = t / 10000000;
    uint32_t minutes = (t / 100000) % 100;
    uint32_t seconds = (t / 1000) % 100;
    uint32_t millis = t % 1000;

    std::snprintf(buf, buf_size, "%02u:%02u:%02u.%03u",
                  hours, minutes, seconds, millis);
}

md_time_t now_md_time() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm local_tm;
    localtime_r(&ts.tv_sec, &local_tm);

    // 转换为 HHMMSSMMM 格式
    uint32_t hours = static_cast<uint32_t>(local_tm.tm_hour);
    uint32_t minutes = static_cast<uint32_t>(local_tm.tm_min);
    uint32_t seconds = static_cast<uint32_t>(local_tm.tm_sec);
    uint32_t millis = static_cast<uint32_t>(ts.tv_nsec / 1'000'000);

    return hours * 10000000 + minutes * 100000 + seconds * 1000 + millis;
}

}  // namespace acct
