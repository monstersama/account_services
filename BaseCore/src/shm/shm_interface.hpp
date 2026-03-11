#pragma once

#include <cstddef>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>

namespace shm {

// 失败时记录到默认日志（DefaultSingleLog）
void log_error(const std::string& msg) noexcept;

// 创建共享内存（不存在则创建并 ftruncate，已存在则无操作）
// 返回 true 表示可安全 open，false 表示创建失败
bool create_if_not_exists(const std::string& name, std::size_t size);

// 仅创建新的共享内存，已存在则失败
// 返回 true 表示创建成功，false 表示已存在或创建失败
bool create_only(const std::string& name, std::size_t size);

// ============ 写端：一写 ============
// 职责：打开已存在的可写共享内存，提供 write_at 写入接口
class ShmWriter {
public:
    ShmWriter();
    ~ShmWriter() noexcept;

    ShmWriter(const ShmWriter&) = delete;
    ShmWriter& operator=(const ShmWriter&) = delete;
    ShmWriter(ShmWriter&& other) noexcept;
    ShmWriter& operator=(ShmWriter&& other) noexcept;

    // 打开已存在的可写共享内存，失败返回 nullptr
    void* open(const std::string& name, std::size_t size);
    void* open(const std::string& account, const std::string& name, std::size_t size);

    // 在 offset 处写入 data，长度为 size 字节
    bool write_at(std::size_t offset, const void* data, std::size_t size) noexcept;

    void close() noexcept;
    static bool unlink(std::string_view name);
    bool is_open() const noexcept;
    const std::string& name() const noexcept;
    std::size_t size() const noexcept;

    void* data() noexcept;
    const void* data() const noexcept;

private:
    std::string name_;
    void* ptr_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

// ============ 读端：多读 ============
// 职责：打开已存在的只读共享内存，提供 read_at 读取接口
class ShmReader {
public:
    ShmReader();
    ~ShmReader() noexcept;

    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;
    ShmReader(ShmReader&& other) noexcept;
    ShmReader& operator=(ShmReader&& other) noexcept;

    // 打开已存在的只读共享内存，失败返回 nullptr
    const void* open(const std::string& name, std::size_t size);
    const void* open(const std::string& account, const std::string& name, std::size_t size);

    bool read_at(std::size_t offset, void* buf, std::size_t size) const noexcept;

    void close() noexcept;
    bool is_open() const noexcept;
    const std::string& name() const noexcept;
    std::size_t size() const noexcept;

    const void* data() const noexcept;

private:
    std::string name_;
    const void* ptr_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

}  // namespace shm
