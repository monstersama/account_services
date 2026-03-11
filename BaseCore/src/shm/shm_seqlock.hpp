#pragma once

#include "shm/shm_common.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// mmap 相关头文件
#if defined(__APPLE__)
#include <sys/mman.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif

namespace shm {

// ============ 共享内存布局 ============
// [Header: 64字节] + [Padding: 64字节] + [Data: 对齐到64字节]
//
// Header 结构：
// - seq: atomic<uint64_t>     // 序列号（奇数=写入中，偶数=写入完成）
// - data_size: uint32_t       // 数据大小
// - version: uint32_t         // 版本号（当前为1）
// - reserved: [48字节]       // 保留，对齐到64字节

constexpr uint32_t kShmLatestVersion = 1;
constexpr uint32_t kShmLatestHeaderSize = 64;
constexpr uint32_t kShmLatestPaddingSize = 64;  // Cache line分离

struct alignas(64) ShmLatestHeader {
    std::atomic<uint64_t> seq{0};        // 序列号（单调递增）
    uint32_t data_size = 0;              // 数据大小
    uint32_t version = kShmLatestVersion; // 版本号
    uint8_t reserved[48]{};              // 对齐填充

    // 检查是否正在写入
    bool is_writing() const noexcept {
        return (seq.load(std::memory_order_acquire) & 1ULL) != 0;
    }

    // 获取当前序列号（用于读取验证）
    uint64_t load_seq() const noexcept {
        return seq.load(std::memory_order_acquire);
    }
};

static_assert(sizeof(ShmLatestHeader) == 64, "ShmLatestHeader must be 64 bytes");

// ============ 写者：单写者 ============
// 职责：打开共享内存，无阻塞覆盖写入最新数据
// 线程安全：单线程写入（不检查多写者）
template <typename T>
class ShmSeqLockWriter {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    ShmSeqLockWriter();
    ~ShmSeqLockWriter() noexcept;

    ShmSeqLockWriter(const ShmSeqLockWriter&) = delete;
    ShmSeqLockWriter& operator=(const ShmSeqLockWriter&) = delete;
    ShmSeqLockWriter(ShmSeqLockWriter&& other) noexcept;
    ShmSeqLockWriter& operator=(ShmSeqLockWriter&& other) noexcept;

    // 打开共享内存（不存在则创建）
    // 返回 true 表示成功
    bool open(const std::string& name);
    bool open(const std::string& account, const std::string& name);

    // 写入数据（无阻塞覆盖）
    // 返回 true 表示成功
    bool write(const T& data) noexcept;

    // 关闭
    void close() noexcept;

    // 是否已打开
    bool is_open() const noexcept;

    // 获取共享内存名称
    const std::string& name() const noexcept;

    // 获取数据指针（用于直接操作，谨慎使用）
    T* data() noexcept;
    const T* data() const noexcept;

    // 获取Header指针
    ShmLatestHeader* header() noexcept;
    const ShmLatestHeader* header() const noexcept;

private:
    std::string name_;
    void* ptr_ = nullptr;
    std::size_t total_size_ = 0;
    ShmLatestHeader* header_ = nullptr;
    T* data_ = nullptr;

    static constexpr std::size_t kTotalSize = kShmLatestHeaderSize + kShmLatestPaddingSize + sizeof(T);
};

// ============ 读者：多读者 ============
// 职责：打开共享内存，读取最新数据
// 线程安全：多线程安全，各读者独立
template <typename T>
class ShmSeqLockReader {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    ShmSeqLockReader();
    ~ShmSeqLockReader() noexcept;

    ShmSeqLockReader(const ShmSeqLockReader&) = delete;
    ShmSeqLockReader& operator=(const ShmSeqLockReader&) = delete;
    ShmSeqLockReader(ShmSeqLockReader&& other) noexcept;
    ShmSeqLockReader& operator=(ShmSeqLockReader&& other) noexcept;

    // 打开共享内存（只读）
    // 返回 true 表示成功
    bool open(const std::string& name);
    bool open(const std::string& account, const std::string& name);

    // 读取最新数据（SeqLock模式）
    // 返回 true 表示成功读到完整数据
    // 返回 false 表示读取过程中被覆盖（建议重试）
    bool read_latest(T& out) const noexcept;

    // 读取最新数据（带重试）
    // max_retries: 最大重试次数
    // 返回 true 表示成功，false 表示超过重试次数仍失败
    bool read_latest_with_retry(T& out, int max_retries = 3) const noexcept;

    // 尝试读取（不保证完整性，最快）
    // 适用于可以接受偶尔读到旧数据的场景
    void read_fast(T& out) const noexcept;

    // 关闭
    void close() noexcept;

    // 是否已打开
    bool is_open() const noexcept;

    // 获取共享内存名称
    const std::string& name() const noexcept;

    // 获取当前序列号（用于检测是否有新数据）
    uint64_t current_seq() const noexcept;

private:
    std::string name_;
    const void* ptr_ = nullptr;
    std::size_t total_size_ = 0;
    const ShmLatestHeader* header_ = nullptr;
    const T* data_ = nullptr;
};

// ============ 实现 ============

// 使用 shm_common.hpp 中的 internal 函数

// ShmSeqLockWriter 实现
template <typename T>
ShmSeqLockWriter<T>::ShmSeqLockWriter() = default;

template <typename T>
ShmSeqLockWriter<T>::~ShmSeqLockWriter() noexcept {
    close();
}

template <typename T>
ShmSeqLockWriter<T>::ShmSeqLockWriter(ShmSeqLockWriter&& other) noexcept
    : name_(std::move(other.name_)),
      ptr_(other.ptr_),
      total_size_(other.total_size_),
      header_(other.header_),
      data_(other.data_) {
    other.ptr_ = nullptr;
    other.total_size_ = 0;
    other.header_ = nullptr;
    other.data_ = nullptr;
}

template <typename T>
ShmSeqLockWriter<T>& ShmSeqLockWriter<T>::operator=(ShmSeqLockWriter&& other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        ptr_ = other.ptr_;
        total_size_ = other.total_size_;
        header_ = other.header_;
        data_ = other.data_;
        other.ptr_ = nullptr;
        other.total_size_ = 0;
        other.header_ = nullptr;
        other.data_ = nullptr;
    }
    return *this;
}

template <typename T>
bool ShmSeqLockWriter<T>::open(const std::string& name) {
    if (is_open()) {
        close();
    }

    if (!internal::create_shm_impl(name, kTotalSize, false)) {
        return false;
    }

    void* ptr = internal::open_shm_write_impl(name, kTotalSize);
    if (!ptr) {
        return false;
    }

    auto* hdr = static_cast<ShmLatestHeader*>(ptr);
    auto* data_ptr = reinterpret_cast<T*>(static_cast<char*>(ptr) + kShmLatestHeaderSize + kShmLatestPaddingSize);

    // 初始化header（如果尚未初始化）
    if (hdr->version != kShmLatestVersion) {
        hdr->seq.store(0, std::memory_order_relaxed);
        hdr->data_size = sizeof(T);
        hdr->version = kShmLatestVersion;
    }

    name_ = name;
    ptr_ = ptr;
    total_size_ = kTotalSize;
    header_ = hdr;
    data_ = data_ptr;

    return true;
}

template <typename T>
bool ShmSeqLockWriter<T>::open(const std::string& account, const std::string& name) {
    std::string name_str = name + "_" + account;
    return open(name_str);
}

template <typename T>
bool ShmSeqLockWriter<T>::write(const T& data) noexcept {
    if (!is_open() || !header_ || !data_) {
        return false;
    }

    // SeqLock 写入协议：
    // 1. seq++（变奇数，标记写入开始）
    // 2. 写入数据
    // 3. seq++（变偶数，标记写入完成）

    uint64_t seq = header_->seq.load(std::memory_order_relaxed);
    header_->seq.store(seq + 1, std::memory_order_release);

    // 内存屏障确保seq++在memcpy之前完成
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 拷贝数据
    std::memcpy(data_, &data, sizeof(T));

    // 内存屏障确保memcpy在seq++之前完成
    std::atomic_thread_fence(std::memory_order_seq_cst);

    header_->seq.store(seq + 2, std::memory_order_release);

    return true;
}

template <typename T>
void ShmSeqLockWriter<T>::close() noexcept {
    if (ptr_ && ptr_ != MAP_FAILED) {
        munmap(ptr_, total_size_);
    }
    name_.clear();
    ptr_ = nullptr;
    total_size_ = 0;
    header_ = nullptr;
    data_ = nullptr;
}

template <typename T>
bool ShmSeqLockWriter<T>::is_open() const noexcept {
    return ptr_ != nullptr && ptr_ != MAP_FAILED;
}

template <typename T>
const std::string& ShmSeqLockWriter<T>::name() const noexcept {
    return name_;
}

template <typename T>
T* ShmSeqLockWriter<T>::data() noexcept {
    return data_;
}

template <typename T>
const T* ShmSeqLockWriter<T>::data() const noexcept {
    return data_;
}

template <typename T>
ShmLatestHeader* ShmSeqLockWriter<T>::header() noexcept {
    return header_;
}

template <typename T>
const ShmLatestHeader* ShmSeqLockWriter<T>::header() const noexcept {
    return header_;
}

// ShmSeqLockReader 实现
template <typename T>
ShmSeqLockReader<T>::ShmSeqLockReader() = default;

template <typename T>
ShmSeqLockReader<T>::~ShmSeqLockReader() noexcept {
    close();
}

template <typename T>
ShmSeqLockReader<T>::ShmSeqLockReader(ShmSeqLockReader&& other) noexcept
    : name_(std::move(other.name_)),
      ptr_(other.ptr_),
      total_size_(other.total_size_),
      header_(other.header_),
      data_(other.data_) {
    other.ptr_ = nullptr;
    other.total_size_ = 0;
    other.header_ = nullptr;
    other.data_ = nullptr;
}

template <typename T>
ShmSeqLockReader<T>& ShmSeqLockReader<T>::operator=(ShmSeqLockReader&& other) noexcept {
    if (this != &other) {
        close();
        name_ = std::move(other.name_);
        ptr_ = other.ptr_;
        total_size_ = other.total_size_;
        header_ = other.header_;
        data_ = other.data_;
        other.ptr_ = nullptr;
        other.total_size_ = 0;
        other.header_ = nullptr;
        other.data_ = nullptr;
    }
    return *this;
}

template <typename T>
bool ShmSeqLockReader<T>::open(const std::string& name) {
    if (is_open()) {
        close();
    }

    const void* ptr = internal::open_shm_read_impl(name, kShmLatestHeaderSize + kShmLatestPaddingSize + sizeof(T));
    if (!ptr) {
        return false;
    }

    auto* hdr = static_cast<const ShmLatestHeader*>(ptr);
    auto* data_ptr = reinterpret_cast<const T*>(static_cast<const char*>(ptr) + kShmLatestHeaderSize + kShmLatestPaddingSize);

    // 验证版本
    if (hdr->version != kShmLatestVersion) {
        munmap(const_cast<void*>(ptr), kShmLatestHeaderSize + kShmLatestPaddingSize + sizeof(T));
        return false;
    }

    name_ = name;
    ptr_ = ptr;
    total_size_ = kShmLatestHeaderSize + kShmLatestPaddingSize + sizeof(T);
    header_ = hdr;
    data_ = data_ptr;

    return true;
}

template <typename T>
bool ShmSeqLockReader<T>::open(const std::string& account, const std::string& name) {
    std::string name_str = name + "_" + account;
    return open(name_str);
}

template <typename T>
bool ShmSeqLockReader<T>::read_latest(T& out) const noexcept {
    if (!is_open() || !header_ || !data_) {
        return false;
    }

    // SeqLock 读取协议：
    // 1. 读取seq
    // 2. 如果seq是奇数，说明正在写入，返回失败
    // 3. 拷贝数据到本地
    // 4. 再次读取seq
    // 5. 如果seq变化，说明读取过程中被覆盖，返回失败

    uint64_t seq1 = header_->seq.load(std::memory_order_acquire);

    // 检查是否正在写入
    if ((seq1 & 1ULL) != 0) {
        return false;
    }

    // 拷贝数据
    std::memcpy(&out, data_, sizeof(T));

    // 内存屏障
    std::atomic_thread_fence(std::memory_order_seq_cst);

    uint64_t seq2 = header_->seq.load(std::memory_order_acquire);

    // 验证序列号是否一致
    return seq1 == seq2;
} 

//快速读取，不保证数据的完整性。节约几十纳秒的延迟
template <typename T>
void ShmSeqLockReader<T>::read_fast(T& out) const noexcept {
    if (is_open() && data_) {
        std::memcpy(&out, data_, sizeof(T));
    }
}

template <typename T>
void ShmSeqLockReader<T>::close() noexcept {
    if (ptr_ && ptr_ != MAP_FAILED) {
        munmap(const_cast<void*>(ptr_), total_size_);
    }
    name_.clear();
    ptr_ = nullptr;
    total_size_ = 0;
    header_ = nullptr;
    data_ = nullptr;
}

template <typename T>
bool ShmSeqLockReader<T>::is_open() const noexcept {
    return ptr_ != nullptr && ptr_ != MAP_FAILED;
}

template <typename T>
const std::string& ShmSeqLockReader<T>::name() const noexcept {
    return name_;
}

template <typename T>
uint64_t ShmSeqLockReader<T>::current_seq() const noexcept {
    if (header_) {
        return header_->load_seq();
    }
    return 0;
}

}  // namespace shm
