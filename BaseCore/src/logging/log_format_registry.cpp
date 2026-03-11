#include "logging/log_format_registry.hpp"

#include <cerrno>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace base_core_log {

namespace {

uint8_t g_next_id = 64;  // 64-255 留给 format 函数，1-63 留给 text

}  // namespace

uint8_t LogFormatRegistry::next_id() noexcept {
    if (g_next_id >= 254) return 255;
    return g_next_id++;
}

std::unordered_map<uint8_t, LogFormatRegistry::Entry>& LogFormatRegistry::registry() {
    static std::unordered_map<uint8_t, Entry> r;
    return r;
}

std::mutex& LogFormatRegistry::mutex() {
    static std::mutex m;
    return m;
}

bool LogFormatRegistry::register_func(uint8_t id, DecodeFunc decode, uint16_t expected_payload_size) {
    std::lock_guard<std::mutex> lock(mutex());
    registry()[id] = {decode, expected_payload_size};
    return true;
}

void LogFormatRegistry::on_error(uint8_t id, uint16_t actual_size) noexcept {
    std::fprintf(stderr, "[LogFormatRegistry] payload size mismatch: format_id=%u actual=%u\n",
        static_cast<unsigned>(id), static_cast<unsigned>(actual_size));
}

bool LogFormatRegistry::decode(uint8_t id, const uint8_t* payload, uint16_t size, char* buf, std::size_t buf_size) {
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mutex());
        auto it = registry().find(id);
        if (it == registry().end()) {
            std::fprintf(stderr, "[LogFormatRegistry] unknown format_id=%u\n", static_cast<unsigned>(id));
            return false;
        }
        entry = it->second;
    }
    if (size != entry.expected_payload_size) {
        on_error(id, size);
        return false;
    }
    if (entry.decode) {
        entry.decode(payload, size, buf, buf_size);
    }
    return true;
}

}  // namespace base_core_log
