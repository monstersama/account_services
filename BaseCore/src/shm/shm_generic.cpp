#include "shm/shm_generic.hpp"
#include "logging/default_single_log.hpp"
#include "shm/shm_common.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>

namespace shm {

namespace {

// 保留 use_file_backend，因为 unlink 需要它
inline bool use_file_backend() {
    return internal::use_file_backend();
}

}  // namespace

void log_error(const std::string& msg) noexcept {
    auto& logger = base_core_log::DefaultSingleLog::instance();
    if (!logger.is_initialized()) {
        (void)logger.init();
    }
    (void)logger.log_str(base_core_log::LogLevel::error, /*module_id=*/0, msg);
}

bool create_if_not_exists(const std::string& name, std::size_t size) {
    return internal::create_shm_impl(name, size, false);
}

bool create_only(const std::string& name, std::size_t size) {
    return internal::create_shm_impl(name, size, true);
}

// ============ ShmGenericWriter ============

ShmGenericWriter::ShmGenericWriter() = default;

ShmGenericWriter::~ShmGenericWriter() noexcept {
    close();
}

ShmGenericWriter::ShmGenericWriter(ShmGenericWriter&& other) noexcept
    : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_), fd_(other.fd_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

ShmGenericWriter& ShmGenericWriter::operator=(ShmGenericWriter&& other) noexcept {
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

void* ShmGenericWriter::open(const std::string& name, std::size_t size) {
    if (is_open()) {
        close();
    }

    const std::string& name_str = name;
    const bool use_file = use_file_backend();

    // 使用公共模块的底层函数获取 fd
    int fd = use_file ? ::open(name_str.c_str(), O_RDWR | O_CLOEXEC)
                      : shm_open(name_str.c_str(), O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "ShmGenericWriter::open failed name=" << name_str << " errno=" << errno
            << " (" << std::strerror(errno) << ")";
        log_error(oss.str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("ShmGenericWriter::open fstat failed name=" + name_str + " errno=" + std::to_string(errno));
        ::close(fd);
        return nullptr;
    }

    if (static_cast<std::size_t>(st.st_size) != size) {
        std::ostringstream oss;
        oss << "ShmGenericWriter::open size mismatch name=" << name_str << " expected=" << size
            << " actual=" << st.st_size;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        log_error("ShmGenericWriter::open mmap failed name=" + name_str + " size=" + std::to_string(size) +
                  " errno=" + std::to_string(errno));
        ::close(fd);
        return nullptr;
    }

    name_ = name_str;
    ptr_ = ptr;
    size_ = size;
    fd_ = fd;
    return ptr;
}

void* ShmGenericWriter::open(const std::string& account, const std::string& name, std::size_t size) {
    std::string name_str = name + "_" + account;
    return open(name_str, size);
}

bool ShmGenericWriter::write_at(std::size_t offset, const void* data, std::size_t len) noexcept {
    if (!ptr_ || ptr_ == MAP_FAILED || !data) {
        log_error("ShmGenericWriter::write_at failed: ptr invalid or data null name=" + name_);
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (offset > size_ || len > size_ - offset) {
        std::ostringstream oss;
        oss << "ShmWriter::write_at failed: offset=" << offset << " len=" << len
            << " size=" << size_ << " name=" << name_;
        log_error(oss.str());
        return false;
    }
    std::memcpy(static_cast<char*>(ptr_) + offset, data, len);
    return true;
}

void ShmGenericWriter::close() noexcept {
    if (ptr_ && ptr_ != MAP_FAILED) {
        munmap(ptr_, size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    name_.clear();
    ptr_ = nullptr;
    size_ = 0;
    fd_ = -1;
}

bool ShmGenericWriter::unlink(std::string_view name) {
    const std::string name_str(name);
    bool ok = false;
    if (use_file_backend()) {
        ok = (::unlink(name_str.c_str()) == 0);
    } else {
        ok = (shm_unlink(name_str.c_str()) == 0);
    }
    if (!ok && errno != ENOENT) {
        log_error("ShmGenericWriter::unlink failed name=" + name_str + " errno=" + std::to_string(errno));
    }
    return ok;
}

bool ShmGenericWriter::is_open() const noexcept {
    return ptr_ != nullptr && ptr_ != MAP_FAILED;
}

const std::string& ShmGenericWriter::name() const noexcept {
    return name_;
}

std::size_t ShmGenericWriter::size() const noexcept {
    return size_;
}

void* ShmGenericWriter::data() noexcept {
    return ptr_;
}

const void* ShmGenericWriter::data() const noexcept {
    return ptr_;
}

// ============ ShmGenericReader ============

ShmGenericReader::ShmGenericReader() = default;

ShmGenericReader::~ShmGenericReader() noexcept {
    close();
}

ShmGenericReader::ShmGenericReader(ShmGenericReader&& other) noexcept
    : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_), fd_(other.fd_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

ShmGenericReader& ShmGenericReader::operator=(ShmGenericReader&& other) noexcept {
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

const void* ShmGenericReader::open(const std::string& name, std::size_t size) {
    if (is_open()) {
        close();
    }

    const std::string& name_str = name;
    const bool use_file = use_file_backend();

    // 使用公共模块的底层函数获取 fd
    int fd = use_file ? ::open(name_str.c_str(), O_RDONLY | O_CLOEXEC)
                      : shm_open(name_str.c_str(), O_RDONLY | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "ShmGenericReader::open failed name=" << name_str << " errno=" << errno
            << " (" << std::strerror(errno) << ")";
        log_error(oss.str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("ShmGenericReader::open fstat failed name=" + name_str + " errno=" + std::to_string(errno));
        ::close(fd);
        return nullptr;
    }

    if (static_cast<std::size_t>(st.st_size) != size) {
        std::ostringstream oss;
        oss << "ShmGenericReader::open size mismatch name=" << name_str << " expected=" << size
            << " actual=" << st.st_size;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        log_error("ShmGenericReader::open mmap failed name=" + name_str + " size=" + std::to_string(size) +
                  " errno=" + std::to_string(errno));
        ::close(fd);
        return nullptr;
    }

    name_ = name_str;
    ptr_ = ptr;
    size_ = size;
    fd_ = fd;
    return ptr;
}

const void* ShmGenericReader::open(const std::string& account, const std::string& name, std::size_t size) {
    std::string name_str = name + "_" + account;
    return open(name_str, size);
}

bool ShmGenericReader::read_at(std::size_t offset, void* buf, std::size_t len) const noexcept {
    if (!ptr_ || ptr_ == MAP_FAILED || !buf) {
        log_error("ShmGenericReader::read_at failed: ptr invalid or buf null name=" + name_);
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (offset > size_ || len > size_ - offset) {
        std::ostringstream oss;
        oss << "ShmReader::read_at failed: offset=" << offset << " len=" << len
            << " size=" << size_ << " name=" << name_;
        log_error(oss.str());
        return false;
    }
    std::memcpy(buf, static_cast<const char*>(ptr_) + offset, len);
    return true;
}

void ShmGenericReader::close() noexcept {
    if (ptr_ && ptr_ != MAP_FAILED) {
        munmap(const_cast<void*>(ptr_), size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
    name_.clear();
    ptr_ = nullptr;
    size_ = 0;
    fd_ = -1;
}

bool ShmGenericReader::is_open() const noexcept {
    return ptr_ != nullptr && ptr_ != MAP_FAILED;
}

const std::string& ShmGenericReader::name() const noexcept {
    return name_;
}

std::size_t ShmGenericReader::size() const noexcept {
    return size_;
}

const void* ShmGenericReader::data() const noexcept {
    return ptr_;
}

}  // namespace shm
