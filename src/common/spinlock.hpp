#pragma once

#include <atomic>

namespace acct_service {

// 高性能自旋锁（64字节对齐避免false sharing）
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

// RAII锁守卫
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

// 可选的RAII锁守卫（支持try_lock）
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

}  // namespace acct_service
