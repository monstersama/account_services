#include "shm/shm_manager.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#include <iostream>
// 前向声明，避免 <unistd.h> 中 acct() 与 namespace acct 冲突
// extern "C" {
// int close(int fd) noexcept;
// int ftruncate(int fd, off_t length) noexcept;
// int shm_unlink(const char* name) noexcept;
// }

namespace acct_service {

// 构造函数
shm_manager::shm_manager() = default;

// 析构函数
shm_manager::~shm_manager() { close(); }

// 移动构造函数
shm_manager::shm_manager(shm_manager &&other) noexcept
    : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_), fd_(other.fd_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

// 移动赋值运算符
shm_manager &shm_manager::operator=(shm_manager &&other) noexcept {
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
void *shm_manager::open_impl(std::string_view name, std::size_t size, shm_mode mode) {
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
                std::cerr << "shm_open failed for " << name << ": " << strerror(errno) << std::endl;
                return nullptr;
            }
            is_new = true;
            break;
        case shm_mode::Open:
            fd_ = shm_open(name_str.c_str(), O_RDWR | O_CLOEXEC, 0644);
            if (fd_ < 0) {
                std::cerr << "shm_open failed for " << name << ": " << strerror(errno) << std::endl;
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
                        std::cerr << "shm_open failed for " << name << ": " << strerror(errno) << std::endl;
                        return nullptr;
                    }
                    is_new = false;
                } else {
                    std::cerr << "shm_open failed for " << name << ": " << strerror(errno) << std::endl;
                    return nullptr;
                }
            } else {
                is_new = true;
            }
            break;
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
        std::cerr << "fstat failed for " << name << ": " << strerror(errno) << std::endl;
        cleanup(is_new);
        return nullptr;
    }

    if (!is_new && static_cast<std::size_t>(st.st_size) != size) {
        std::cerr << "shm size mismatch for " << name << ": expected " << size << ", got "
                  << static_cast<std::size_t>(st.st_size) << std::endl;
        cleanup(false);
        return nullptr;
    }

    // 如果是新创建的，设置大小
    if (is_new) {
        if (ftruncate(fd_, static_cast<off_t>(size)) < 0) {
            std::cerr << "ftruncate failed for " << name << ": " << strerror(errno) << std::endl;
            cleanup(true);
            return nullptr;
        }
    }

    // 映射到进程地址空间
    ptr_ = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (ptr_ == MAP_FAILED) {
        std::cerr << "mmap failed for " << name << ": " << strerror(errno) << std::endl;
        cleanup(is_new);
        return nullptr;
    }

    size_ = size;
    name_ = name;
    last_open_is_new_ = is_new;

    return ptr_;
}

// 初始化共享内存头部
void shm_manager::init_header(shm_header *header, account_id_t account_id) {
    header->magic = shm_header::kMagic;
    header->version = shm_header::kVersion;
    header->create_time = now_ns();
    header->last_update = now_ns();
    header->next_order_id.store(1, std::memory_order_relaxed);
}

// 验证共享内存头部
bool shm_manager::validate_header(const shm_header *header) {
    if (header->magic != shm_header::kMagic) {
        std::cerr << "Invalid shm magic: expected 0x" << std::hex << shm_header::kMagic << ", got 0x" << header->magic
                  << std::dec << std::endl;
        return false;
    }
    if (header->version != shm_header::kVersion) {
        std::cerr << "Invalid shm version: expected " << shm_header::kVersion << ", got " << header->version
                  << std::endl;
        return false;
    }
    return true;
}

// 创建/打开上游共享内存
upstream_shm_layout *shm_manager::open_upstream(std::string_view name, shm_mode mode, account_id_t account_id) {
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
downstream_shm_layout *shm_manager::open_downstream(std::string_view name, shm_mode mode, account_id_t account_id) {
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
trades_shm_layout *shm_manager::open_trades(std::string_view name, shm_mode mode, account_id_t account_id) {
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

// 创建/打开持仓共享内存
positions_shm_layout *shm_manager::open_positions(std::string_view name, shm_mode mode, account_id_t account_id) {
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
        layout->header.create_time = now_ns();
        layout->header.last_update = now_ns();
        layout->header.id.store(1, std::memory_order_relaxed);
        layout->position_count.store(0, std::memory_order_relaxed);
    } else {
        if (layout->header.magic != positions_header::kMagic) {
            std::cerr << "Invalid positions shm magic: expected 0x" << std::hex << positions_header::kMagic
                      << ", got 0x" << layout->header.magic << std::dec << std::endl;
            close();
            return nullptr;
        }
        if (layout->header.version != positions_header::kVersion) {
            std::cerr << "Invalid positions shm version: expected " << positions_header::kVersion << ", got "
                      << layout->header.version << std::endl;
            close();
            return nullptr;
        }
    }

    return layout;
}

// 关闭并解除映射
void shm_manager::close() {
    if (ptr_ && ptr_ != MAP_FAILED) {
        if (munmap(ptr_, size_) < 0) {
            std::cerr << "munmap failed: " << strerror(errno) << std::endl;
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
bool shm_manager::unlink(std::string_view name) {
    if (shm_unlink(std::string(name).c_str()) < 0) {
        std::cerr << "shm_unlink failed for " << name << ": " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// 检查是否已打开
bool shm_manager::is_open() const noexcept { return ptr_ != nullptr && ptr_ != MAP_FAILED; }

// 获取共享内存名称
const std::string &shm_manager::name() const noexcept { return name_; }

}  // namespace acct_service
