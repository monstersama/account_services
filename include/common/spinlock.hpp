#pragma once

#include <atomic>

namespace acct {

// 高性能自旋锁（64字节对齐避免false sharing）
class alignas(64) spinlock {
public:
    spinlock() noexcept = default;
    spinlock(const spinlock&) = delete;
    spinlock& operator=(const spinlock&) = delete;

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
class lock_guard {
public:
    explicit lock_guard(Lock& lock);
    ~lock_guard();
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

private:
    Lock& lock_;
};

// 可选的RAII锁守卫（支持try_lock）
template <typename Lock>
class unique_lock {
public:
    explicit unique_lock(Lock& lock, bool try_lock = false);
    ~unique_lock();
    bool owns_lock() const noexcept;
    void unlock();
    unique_lock(const unique_lock&) = delete;
    unique_lock& operator=(const unique_lock&) = delete;

private:
    Lock* lock_;
    bool owns_lock_;
};

}  // namespace acct
