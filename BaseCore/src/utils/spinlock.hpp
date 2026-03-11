#pragma once

#include <atomic>

namespace base_core {

// 高性能自旋锁（64 字节对齐避免 false sharing）
// TTAS + 指数退避
class alignas(64) SpinLock {
public:
    SpinLock() noexcept = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() noexcept;
    bool try_lock() noexcept;
    void unlock() noexcept;
    bool is_locked() const noexcept;

private:
    std::atomic<bool> flag_{false};
    char padding_[63];
};

// RAII 锁守卫
template <typename Lock>
class LockGuard {
public:
    explicit LockGuard(Lock& lock);
    ~LockGuard();
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& lock_;
};

// 可选 RAII 锁守卫（支持 try_lock）
template <typename Lock>
class UniqueLock {
public:
    explicit UniqueLock(Lock& lock, bool try_lock = false);
    ~UniqueLock();
    bool owns_lock() const noexcept;
    void unlock();
    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;

private:
    Lock* lock_;
    bool owns_lock_;
};

}  // namespace base_core

// 兼容性别名（逐步迁移后可移除）
namespace basecore = base_core;
