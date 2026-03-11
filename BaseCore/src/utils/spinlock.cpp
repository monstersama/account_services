#include "utils/spinlock.hpp"

// x86 PAUSE 指令支持
#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
#define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#define CPU_PAUSE() ((void)0)
#endif

namespace base_core {

void SpinLock::lock() noexcept {
    bool expected = false;

    if (flag_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
    }

    int backoff = 1;
    const int max_backoff = 1024;

    while (true) {
        while (flag_.load(std::memory_order_relaxed)) {
            for (int i = 0; i < backoff; ++i) {
                CPU_PAUSE();
            }
            backoff = (backoff < max_backoff) ? (backoff << 1) : max_backoff;
        }

        expected = false;
        if (flag_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }
    }
}

bool SpinLock::try_lock() noexcept {
    bool expected = false;
    return flag_.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed);
}

void SpinLock::unlock() noexcept {
    flag_.store(false, std::memory_order_release);
}

bool SpinLock::is_locked() const noexcept {
    return flag_.load(std::memory_order_relaxed);
}

template <typename Lock>
LockGuard<Lock>::LockGuard(Lock& lock) : lock_(lock) {
    lock_.lock();
}

template <typename Lock>
LockGuard<Lock>::~LockGuard() {
    lock_.unlock();
}

template <typename Lock>
UniqueLock<Lock>::UniqueLock(Lock& lock, bool try_lock) : lock_(&lock), owns_lock_(false) {
    if (try_lock) {
        owns_lock_ = lock_->try_lock();
    } else {
        lock_->lock();
        owns_lock_ = true;
    }
}

template <typename Lock>
UniqueLock<Lock>::~UniqueLock() {
    if (owns_lock_) {
        lock_->unlock();
    }
}

template <typename Lock>
bool UniqueLock<Lock>::owns_lock() const noexcept {
    return owns_lock_;
}

template <typename Lock>
void UniqueLock<Lock>::unlock() {
    if (owns_lock_) {
        lock_->unlock();
        owns_lock_ = false;
    }
}

template class LockGuard<SpinLock>;
template class UniqueLock<SpinLock>;

}  // namespace base_core
