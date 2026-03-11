#include "shm/shm_interface.hpp"
#include "logging/default_single_log.hpp"

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

constexpr mode_t kShmCreateMode = 0777;

inline bool use_file_backend() {
    const char* v = getenv("SHM_USE_FILE");
    return v && v[0] == '1';
}

// 创建：O_CREAT|O_EXCL，成功则 ftruncate 并 close
bool do_create_file(const std::string& path, std::size_t size, bool fail_if_exists) {
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) return !fail_if_exists;  // 已存在：幂等返回 true，create_only 返回 false
        return false;
    }
    bool ok = (ftruncate(fd, static_cast<off_t>(size)) == 0);
    ::close(fd);
    if (!ok) ::unlink(path.c_str());
    return ok;
}

bool do_create_shm(const std::string& name_str, std::size_t size, bool fail_if_exists) {
    int fd = shm_open(name_str.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, kShmCreateMode);
    if (fd < 0) {
        if (errno == EEXIST) return !fail_if_exists;
        return false;
    }
    if (fchmod(fd, kShmCreateMode) < 0) {
        ::close(fd);
        shm_unlink(name_str.c_str());
        return false;
    }
    bool ok = (ftruncate(fd, static_cast<off_t>(size)) == 0);
    ::close(fd);
    if (!ok) shm_unlink(name_str.c_str());
    return ok;
}

// 打开已存在：写端 O_RDWR
int open_file_write(const std::string& path) {
    return ::open(path.c_str(), O_RDWR | O_CLOEXEC);
}

int open_shm_write(const std::string& name_str) {
    return shm_open(name_str.c_str(), O_RDWR | O_CLOEXEC, 0644);
}

// 打开已存在：读端 O_RDONLY
int open_file_read(const std::string& path) {
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
}

int open_shm_read(const std::string& name_str) {
    return shm_open(name_str.c_str(), O_RDONLY | O_CLOEXEC, 0644);
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
    if (use_file_backend()) {
        return do_create_file(name, size, false);
    }
    return do_create_shm(name, size, false);
}

bool create_only(const std::string& name, std::size_t size) {
    if (use_file_backend()) {
        return do_create_file(name, size, true);
    }
    return do_create_shm(name, size, true);
}

// ============ ShmWriter ============

ShmWriter::ShmWriter() = default;

ShmWriter::~ShmWriter() noexcept {
    close();
}

ShmWriter::ShmWriter(ShmWriter&& other) noexcept
    : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_), fd_(other.fd_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

ShmWriter& ShmWriter::operator=(ShmWriter&& other) noexcept {
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

void* ShmWriter::open(const std::string& name, std::size_t size) {
    if (is_open()) {
        close();
    }

    const std::string& name_str = name;
    const bool use_file = use_file_backend();

    int fd = use_file ? open_file_write(name_str) : open_shm_write(name_str);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "ShmWriter::open failed name=" << name_str << " errno=" << errno
            << " (" << std::strerror(errno) << ")";
        log_error(oss.str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("ShmWriter::open fstat failed name=" + name_str + " errno=" + std::to_string(errno));
        ::close(fd);
        return nullptr;
    }

    if (static_cast<std::size_t>(st.st_size) != size) {
        std::ostringstream oss;
        oss << "ShmWriter::open size mismatch name=" << name_str << " expected=" << size
            << " actual=" << st.st_size;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        log_error("ShmWriter::open mmap failed name=" + name_str + " size=" + std::to_string(size) +
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

void* ShmWriter::open(const std::string& account, const std::string& name, std::size_t size) {
    std::string name_str = name + "_" + account;
    return open(name_str, size);
}

bool ShmWriter::write_at(std::size_t offset, const void* data, std::size_t len) noexcept {
    if (!ptr_ || ptr_ == MAP_FAILED || !data) {
        log_error("ShmWriter::write_at failed: ptr invalid or data null name=" + name_);
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

void ShmWriter::close() noexcept {
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

bool ShmWriter::unlink(std::string_view name) {
    const std::string name_str(name);
    bool ok = false;
    if (use_file_backend()) {
        ok = (::unlink(name_str.c_str()) == 0);
    } else {
        ok = (shm_unlink(name_str.c_str()) == 0);
    }
    if (!ok && errno != ENOENT) {
        log_error("ShmWriter::unlink failed name=" + name_str + " errno=" + std::to_string(errno));
    }
    return ok;
}

bool ShmWriter::is_open() const noexcept {
    return ptr_ != nullptr && ptr_ != MAP_FAILED;
}

const std::string& ShmWriter::name() const noexcept {
    return name_;
}

std::size_t ShmWriter::size() const noexcept {
    return size_;
}

void* ShmWriter::data() noexcept {
    return ptr_;
}

const void* ShmWriter::data() const noexcept {
    return ptr_;
}

// ============ ShmReader ============

ShmReader::ShmReader() = default;

ShmReader::~ShmReader() noexcept {
    close();
}

ShmReader::ShmReader(ShmReader&& other) noexcept
    : name_(std::move(other.name_)), ptr_(other.ptr_), size_(other.size_), fd_(other.fd_) {
    other.ptr_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
}

ShmReader& ShmReader::operator=(ShmReader&& other) noexcept {
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

const void* ShmReader::open(const std::string& name, std::size_t size) {
    if (is_open()) {
        close();
    }

    const std::string& name_str = name;
    const bool use_file = use_file_backend();

    int fd = use_file ? open_file_read(name_str) : open_shm_read(name_str);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "ShmReader::open failed name=" << name_str << " errno=" << errno
            << " (" << std::strerror(errno) << ")";
        log_error(oss.str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        log_error("ShmReader::open fstat failed name=" + name_str + " errno=" + std::to_string(errno));
        ::close(fd);
        return nullptr;
    }

    if (static_cast<std::size_t>(st.st_size) != size) {
        std::ostringstream oss;
        oss << "ShmReader::open size mismatch name=" << name_str << " expected=" << size
            << " actual=" << st.st_size;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        log_error("ShmReader::open mmap failed name=" + name_str + " size=" + std::to_string(size) +
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

const void* ShmReader::open(const std::string& account, const std::string& name, std::size_t size) {
    std::string name_str = name + "_" + account;
    return open(name_str, size);
}

bool ShmReader::read_at(std::size_t offset, void* buf, std::size_t len) const noexcept {
    if (!ptr_ || ptr_ == MAP_FAILED || !buf) {
        log_error("ShmReader::read_at failed: ptr invalid or buf null name=" + name_);
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

void ShmReader::close() noexcept {
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

bool ShmReader::is_open() const noexcept {
    return ptr_ != nullptr && ptr_ != MAP_FAILED;
}

const std::string& ShmReader::name() const noexcept {
    return name_;
}

std::size_t ShmReader::size() const noexcept {
    return size_;
}

const void* ShmReader::data() const noexcept {
    return ptr_;
}

}  // namespace shm
