#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

#include "shm/shm_generic.hpp"

namespace acct_service::basecore_shm_bridge {

// account_services 当前仅支持 POSIX shm 数据面，避免静默切到文件后端。
inline bool is_file_backend_requested() noexcept {
    const char* value = std::getenv("SHM_USE_FILE");
    return value != nullptr && value[0] == '1';
}

inline bool create_region(std::string_view name, std::size_t size) {
    return shm::create_only(std::string(name), size);
}

inline bool open_writer(shm::ShmGenericWriter& writer, std::string_view name, std::size_t size) {
    return writer.open(std::string(name), size) != nullptr;
}

inline bool open_reader(shm::ShmGenericReader& reader, std::string_view name, std::size_t size) {
    return reader.open(std::string(name), size) != nullptr;
}

}  // namespace acct_service::basecore_shm_bridge
