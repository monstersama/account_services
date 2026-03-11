#include "shm/shm_common.hpp"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>

namespace shm {
namespace internal {

namespace {

constexpr mode_t kShmCreateMode = 0777;

// 错误日志回调，默认输出到stderr
LogErrorFunc g_log_error_func = nullptr;

void default_log_error(const std::string& msg) noexcept {
    std::cerr << "[shm] " << msg << "\n";
}

// 创建文件后端
bool do_create_file(const std::string& path, std::size_t size, bool fail_if_exists) {
    int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) {
        if (errno == EEXIST) {
            // 已存在
            if (!fail_if_exists) {
                // 幂等模式：验证大小
                struct stat st;
                if (::stat(path.c_str(), &st) == 0) {
                    return static_cast<std::size_t>(st.st_size) == size;
                }
            }
            return false;
        }
        return false;
    }
    bool ok = (ftruncate(fd, static_cast<off_t>(size)) == 0);
    ::close(fd);
    if (!ok) {
        ::unlink(path.c_str());
    }
    return ok;
}

// 创建POSIX共享内存
bool do_create_shm(const std::string& name_str, std::size_t size, bool fail_if_exists) {
    int fd = shm_open(name_str.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, kShmCreateMode);
    if (fd < 0) {
        if (errno == EEXIST) {
            // 已存在
            if (!fail_if_exists) {
                // 幂等模式：验证大小
                struct stat st;
                if (::stat(name_str.c_str(), &st) == 0) {
                    return static_cast<std::size_t>(st.st_size) == size;
                }
            }
            return false;
        }
        return false;
    }
    if (fchmod(fd, kShmCreateMode) < 0) {
        ::close(fd);
        shm_unlink(name_str.c_str());
        return false;
    }
    bool ok = (ftruncate(fd, static_cast<off_t>(size)) == 0);
    ::close(fd);
    if (!ok) {
        shm_unlink(name_str.c_str());
    }
    return ok;
}

// 打开文件写端
int open_file_write(const std::string& path) {
    return ::open(path.c_str(), O_RDWR | O_CLOEXEC);
}

// 打开文件读端
int open_file_read(const std::string& path) {
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
}

// 打开POSIX共享内存写端
int open_shm_write_fd(const std::string& name_str) {
    return shm_open(name_str.c_str(), O_RDWR | O_CLOEXEC, 0644);
}

// 打开POSIX共享内存读端
int open_shm_read_fd(const std::string& name_str) {
    return shm_open(name_str.c_str(), O_RDONLY | O_CLOEXEC, 0644);
}

}  // namespace

// ============ 公共接口实现 ============

bool use_file_backend() {
    const char* v = getenv("SHM_USE_FILE");
    return v && v[0] == '1';
}

bool create_shm_impl(const std::string& name, std::size_t size, bool fail_if_exists) {
    if (use_file_backend()) {
        return do_create_file(name, size, fail_if_exists);
    }
    return do_create_shm(name, size, fail_if_exists);
}

void* open_shm_write_impl(const std::string& name, std::size_t size) {
    const bool use_file = use_file_backend();

    int fd = use_file ? open_file_write(name) : open_shm_write_fd(name);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "open_shm_write_impl failed name=" << name << " errno=" << errno
            << " (" << std::strerror(errno) << ")";
        log_error(oss.str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        std::ostringstream oss;
        oss << "open_shm_write_impl fstat failed name=" << name << " errno=" << errno;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    if (static_cast<std::size_t>(st.st_size) != size) {
        std::ostringstream oss;
        oss << "open_shm_write_impl size mismatch name=" << name << " expected=" << size
            << " actual=" << st.st_size;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        std::ostringstream oss;
        oss << "open_shm_write_impl mmap failed name=" << name << " size=" << size
            << " errno=" << errno;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    // 关闭 fd，mmap 仍然有效
    ::close(fd);

    return ptr;
}

const void* open_shm_read_impl(const std::string& name, std::size_t size) {
    const bool use_file = use_file_backend();

    int fd = use_file ? open_file_read(name) : open_shm_read_fd(name);
    if (fd < 0) {
        std::ostringstream oss;
        oss << "open_shm_read_impl failed name=" << name << " errno=" << errno
            << " (" << std::strerror(errno) << ")";
        log_error(oss.str());
        return nullptr;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        std::ostringstream oss;
        oss << "open_shm_read_impl fstat failed name=" << name << " errno=" << errno;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    if (static_cast<std::size_t>(st.st_size) != size) {
        std::ostringstream oss;
        oss << "open_shm_read_impl size mismatch name=" << name << " expected=" << size
            << " actual=" << st.st_size;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    void* ptr = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        std::ostringstream oss;
        oss << "open_shm_read_impl mmap failed name=" << name << " size=" << size
            << " errno=" << errno;
        log_error(oss.str());
        ::close(fd);
        return nullptr;
    }

    // 关闭 fd，mmap 仍然有效
    ::close(fd);

    return ptr;
}

void set_log_error_func(LogErrorFunc func) {
    g_log_error_func = func;
}

void log_error(const std::string& msg) noexcept {
    if (g_log_error_func) {
        g_log_error_func(msg);
    } else {
        default_log_error(msg);
    }
}

}  // namespace internal
}  // namespace shm
