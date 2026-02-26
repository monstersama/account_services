#include "shm/shm_manager.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>

#include "common/error.hpp"
#include "common/log.hpp"
#include "shm/orders_shm.hpp"

namespace acct_service {

namespace {

bool report_shm_error(error_code code, std::string_view name, std::string_view detail, int err = 0) {
    error_status status = ACCT_MAKE_ERROR(error_domain::shm, code, "shm_manager", detail, err);
    if (!name.empty()) {
        fixed_string<192> msg;
        std::string text = std::string(detail) + " [" + std::string(name) + "]";
        status.message.assign(text);
    }
    record_error(status);
    ACCT_LOG_ERROR_STATUS(status);
    return false;
}

}  // namespace

// 构造函数
SHMManager::SHMManager() = default;

// 析构函数
SHMManager::~SHMManager() noexcept { close(); }

// 移动构造函数
SHMManager::SHMManager(SHMManager &&other) noexcept
    : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_), fd_(other.fd_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

// 移动赋值运算符
SHMManager &SHMManager::operator=(SHMManager &&other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        ptr_ = other.ptr_;
        size_ = other.size_;
        fd_ = other.fd_;
        other.ptr_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

// 内部实现：打开或创建共享内存
void *SHMManager::open_impl(std::string_view name, std::size_t size, shm_mode mode) {
    // 如果已经打开了其他共享内存，先关闭
    if (is_open()) {
        close();
    }

    last_open_is_new_ = false;
    bool is_new = false;
    const std::string name_str(name);

    auto cleanup = [&](bool unlink_if_new) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        if (unlink_if_new) {
            shm_unlink(name_str.c_str());
        }
        name_.clear();
        ptr_ = nullptr;
        size_ = 0;
        fd_ = -1;
        last_open_is_new_ = false;
    };

    switch (mode) {
        case shm_mode::Create:
            fd_ = shm_open(name_str.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
            if (fd_ < 0) {
                (void)report_shm_error(error_code::ShmOpenFailed, name, "shm_open(create) failed", errno);
                return nullptr;
            }
            is_new = true;
            break;
        case shm_mode::Open:
            fd_ = shm_open(name_str.c_str(), O_RDWR | O_CLOEXEC, 0644);
            if (fd_ < 0) {
                (void)report_shm_error(error_code::ShmOpenFailed, name, "shm_open(open) failed", errno);
                return nullptr;
            }
            is_new = false;
            break;
        case shm_mode::OpenOrCreate:
            fd_ = shm_open(name_str.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
            if (fd_ < 0) {
                if (errno == EEXIST) {
                    fd_ = shm_open(name_str.c_str(), O_RDWR | O_CLOEXEC, 0644);
                    if (fd_ < 0) {
                        (void)report_shm_error(error_code::ShmOpenFailed, name, "shm_open(open after exist) failed", errno);
                        return nullptr;
                    }
                    is_new = false;
                } else {
                    (void)report_shm_error(error_code::ShmOpenFailed, name, "shm_open(open_or_create) failed", errno);
                    return nullptr;
                }
            } else {
                is_new = true;
            }
            break;
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
        (void)report_shm_error(error_code::ShmFstatFailed, name, "fstat failed", errno);
        cleanup(is_new);
        return nullptr;
    }

    if (!is_new && static_cast<std::size_t>(st.st_size) != size) {
        (void)report_shm_error(error_code::ShmResizeFailed, name, "shm size mismatch");
        cleanup(false);
        return nullptr;
    }

    // 如果是新创建的，设置大小
    if (is_new) {
        if (ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            (void)report_shm_error(error_code::ShmResizeFailed, name, "ftruncate failed", errno);
            cleanup(true);
            return nullptr;
        }
    }

    // 映射到进程地址空间
    ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
        (void)report_shm_error(error_code::ShmMmapFailed, name, "mmap failed", errno);
        cleanup(is_new);
        return nullptr;
    }

    size_ = size;
    name_ = name;
    last_open_is_new_ = is_new;

    return ptr_;
}

// 初始化共享内存头部
void SHMManager::init_header(shm_header *header, account_id_t account_id) {
    header->magic = shm_header::kMagic;
    header->version = shm_header::kVersion;
    header->create_time = now_ns();
    header->last_update = now_ns();
    header->next_order_id.store(1, std::memory_order_relaxed);
}

// 验证共享内存头部
bool SHMManager::validate_header(const shm_header *header) {
    if (header->magic != shm_header::kMagic) {
        (void)report_shm_error(error_code::ShmHeaderInvalid, name_, "invalid shm magic");
        return false;
    }
    if (header->version != shm_header::kVersion) {
        (void)report_shm_error(error_code::ShmHeaderInvalid, name_, "invalid shm version");
        return false;
    }
    return true;
}

// 创建/打开上游共享内存
upstream_shm_layout *SHMManager::open_upstream(std::string_view name, shm_mode mode, account_id_t account_id) {
    constexpr std::size_t size = sizeof(upstream_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<upstream_shm_layout *>(ptr);

    if (last_open_is_new_) {
        init_header(&layout->header, account_id);
    } else {
        if (!validate_header(&layout->header)) {
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开下游共享内存
downstream_shm_layout *SHMManager::open_downstream(std::string_view name, shm_mode mode, account_id_t account_id) {
    constexpr std::size_t size = sizeof(downstream_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<downstream_shm_layout *>(ptr);

    if (last_open_is_new_) {
        init_header(&layout->header, account_id);
    } else {
        if (!validate_header(&layout->header)) {
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开成交回报共享内存
trades_shm_layout *SHMManager::open_trades(std::string_view name, shm_mode mode, account_id_t account_id) {
    constexpr std::size_t size = sizeof(trades_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<trades_shm_layout *>(ptr);

    if (last_open_is_new_) {
        init_header(&layout->header, account_id);
    } else {
        if (!validate_header(&layout->header)) {
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开订单池共享内存
orders_shm_layout* SHMManager::open_orders(std::string_view name, shm_mode mode, account_id_t account_id) {
    (void)account_id;
    constexpr std::size_t size = sizeof(orders_shm_layout);
    void* ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto* layout = static_cast<orders_shm_layout*>(ptr);

    char expected_trading_day[9] = "00000000";
    if (!extract_trading_day_from_name(name, expected_trading_day)) {
        std::memcpy(expected_trading_day, "00000000", 9);
    }

    if (last_open_is_new_) {
        layout->header.magic = orders_header::kMagic;
        layout->header.version = orders_header::kVersion;
        layout->header.header_size = static_cast<uint32_t>(sizeof(orders_header));
        layout->header.total_size = static_cast<uint32_t>(sizeof(orders_shm_layout));
        layout->header.capacity = static_cast<uint32_t>(kDailyOrderPoolCapacity);
        layout->header.init_state = 0;
        layout->header.create_time = now_ns();
        layout->header.last_update = layout->header.create_time;
        layout->header.next_index.store(0, std::memory_order_relaxed);
        layout->header.full_reject_count.store(0, std::memory_order_relaxed);
        std::memcpy(layout->header.trading_day, expected_trading_day, 9);
        layout->header.init_state = 1;
    } else {
        if (layout->header.magic != orders_header::kMagic) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid orders shm magic");
            close();
            return nullptr;
        }
        if (layout->header.version != orders_header::kVersion) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid orders shm version");
            close();
            return nullptr;
        }
        if (layout->header.header_size != static_cast<uint32_t>(sizeof(orders_header))) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid orders shm header size");
            close();
            return nullptr;
        }
        if (layout->header.total_size != static_cast<uint32_t>(sizeof(orders_shm_layout))) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid orders shm total size");
            close();
            return nullptr;
        }
        if (layout->header.capacity != static_cast<uint32_t>(kDailyOrderPoolCapacity)) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid orders shm capacity");
            close();
            return nullptr;
        }
        if (layout->header.init_state != 1) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "orders shm not initialized");
            close();
            return nullptr;
        }
        if (std::memcmp(layout->header.trading_day, expected_trading_day, 8) != 0) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "orders shm trading day mismatch");
            close();
            return nullptr;
        }
    }

    return layout;
}

// 创建/打开持仓共享内存
positions_shm_layout *SHMManager::open_positions(std::string_view name, shm_mode mode, account_id_t account_id) {
    constexpr std::size_t size = sizeof(positions_shm_layout);
    void *ptr = open_impl(name, size, mode);
    if (!ptr) {
        return nullptr;
    }

    auto *layout = static_cast<positions_shm_layout *>(ptr);

    // 持仓共享内存使用 positions_header，需要单独处理
    if (last_open_is_new_) {
        layout->header.magic = positions_header::kMagic;
        layout->header.version = positions_header::kVersion;
        layout->header.header_size = static_cast<uint32_t>(sizeof(positions_header));
        layout->header.total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
        layout->header.capacity = static_cast<uint32_t>(kMaxPositions);
        layout->header.init_state = 0;
        layout->header.create_time = now_ns();
        layout->header.last_update = now_ns();
        layout->header.id.store(1, std::memory_order_relaxed);
        layout->position_count.store(0, std::memory_order_relaxed);
    } else {
        if (layout->header.magic != positions_header::kMagic) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid positions shm magic");
            close();
            return nullptr;
        }
        if (layout->header.version != positions_header::kVersion) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid positions shm version");
            close();
            return nullptr;
        }
        const uint32_t expected_header_size = static_cast<uint32_t>(sizeof(positions_header));
        if (layout->header.header_size != expected_header_size) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid positions header size");
            close();
            return nullptr;
        }
        const uint32_t expected_total_size = static_cast<uint32_t>(sizeof(positions_shm_layout));
        if (layout->header.total_size != expected_total_size) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid positions total size");
            close();
            return nullptr;
        }
        const uint32_t expected_capacity = static_cast<uint32_t>(kMaxPositions);
        if (layout->header.capacity != expected_capacity) {
            (void)report_shm_error(error_code::ShmHeaderInvalid, name, "invalid positions capacity");
            close();
            return nullptr;
        }
    }

    return layout;
}

// 关闭并解除映射
void SHMManager::close() noexcept {
    if (ptr_ && ptr_ != MAP_FAILED) {
        if (munmap(ptr_, size_) < 0) {
            // Keep close() noexcept: error reporting must not leak exceptions.
            try {
                (void)report_shm_error(error_code::ShmMmapFailed, name_, "munmap failed", errno);
            } catch (...) {
            }
        }
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }

    name_.clear();
    ptr_ = nullptr;
    size_ = 0;
    fd_ = -1;
    last_open_is_new_ = false;
}

// 删除共享内存对象
bool SHMManager::unlink(std::string_view name) {
    if (shm_unlink(std::string(name).c_str()) < 0) {
        (void)report_shm_error(error_code::ShmOpenFailed, name, "shm_unlink failed", errno);
        return false;
    }
    return true;
}

// 检查是否已打开
bool SHMManager::is_open() const noexcept { return ptr_ != nullptr && ptr_ != MAP_FAILED; }

// 获取共享内存名称
const std::string &SHMManager::name() const noexcept { return name_; }

}  // namespace acct_service
