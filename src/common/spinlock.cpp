#include "common/spinlock.hpp"

#include <thread>

// x86 PAUSE 指令支持
#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#define CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__)
// ARM 使用 YIELD 指令
#define CPU_PAUSE() asm volatile("yield" ::: "memory")
#else
#define CPU_PAUSE() ((void)0)
#endif

namespace acct {

void spinlock::lock() noexcept {
    // TTAS (Test-And-Test-And-Set) + 指数退避优化
    // 先读取检查，避免不必要的原子写操作，减少缓存一致性流量

    // 快速路径：尝试立即获取锁
    if (!flag_.load(std::memory_order_relaxed) && 
        !flag_.exchange(true, std::memory_order_acquire)) {
        return;
    }

    // 慢速路径：TTAS + 指数退避
    int backoff = 1;
    const int max_backoff = 1024;

    while (true) {
        // 第一步：Test - 检查锁是否被持有（只读，不触发缓存失效风暴）
        while (flag_.load(std::memory_order_relaxed)) {
            // 指数退避：PAUSE 次数逐渐增加
            for (int i = 0; i < backoff; ++i) {
                CPU_PAUSE();
            }
            // 增加退避时间，但不超过上限
            backoff = (backoff < max_backoff) ? (backoff << 1) : max_backoff;
        }

        // 第二步：Test-And-Set - 尝试获取锁
        if (!flag_.exchange(true, std::memory_order_acquire)) {
            return;
        }

        // 失败说明在读取和交换之间有其他线程获取了锁
        // 继续循环，指数退避已经增加
    }
}

bool spinlock::try_lock() noexcept {
    // TTAS 模式尝试获取锁
    if (flag_.load(std::memory_order_relaxed)) {
        return false;
    }
    return !flag_.exchange(true, std::memory_order_acquire);
}

void spinlock::unlock() noexcept {
    flag_.store(false, std::memory_order_release);
}

bool spinlock::is_locked() const noexcept {
    return flag_.load(std::memory_order_relaxed);
}

// lock_guard 实现
template <typename Lock>
lock_guard<Lock>::lock_guard(Lock& lock) : lock_(lock) {
    lock_.lock();
}

template <typename Lock>
lock_guard<Lock>::~lock_guard() {
    lock_.unlock();
}

// unique_lock 实现
template <typename Lock>
unique_lock<Lock>::unique_lock(Lock& lock, bool try_lock)
    : lock_(&lock), owns_lock_(false) {
    if (try_lock) {
        owns_lock_ = lock_->try_lock();
    } else {
        lock_->lock();
        owns_lock_ = true;
    }
}

template <typename Lock>
unique_lock<Lock>::~unique_lock() {
    if (owns_lock_) {
        lock_->unlock();
    }
}

template <typename Lock>
bool unique_lock<Lock>::owns_lock() const noexcept {
    return owns_lock_;
}

template <typename Lock>
void unique_lock<Lock>::unlock() {
    if (owns_lock_) {
        lock_->unlock();
        owns_lock_ = false;
    }
}

// 显式实例化
template class lock_guard<spinlock>;
template class unique_lock<spinlock>;

}  // namespace acct
