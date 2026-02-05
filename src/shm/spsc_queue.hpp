#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace acct_service {

// 单生产者单消费者无锁环形队列
// T 必须可拷贝赋值（支持含 std::atomic 成员的类型）
// Capacity 必须是 2 的幂
template <typename T, std::size_t Capacity>
class alignas(64) spsc_queue {
    static_assert(std::is_copy_assignable_v<T>, "T must be copy assignable");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");

public:
    // 共享内存中初始化
    void init() noexcept;

    // 生产者：尝试写入
    bool try_push(const T& item) noexcept;

    // 消费者：尝试读取
    bool try_pop(T& item) noexcept;

    // 消费者：只看不取
    bool try_peek(T& item) const noexcept;

    // 获取当前元素数量（近似值）
    std::size_t size() const noexcept;

    // 是否为空
    bool empty() const noexcept;

    // 容量
    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    alignas(64) T buffer_[Capacity];
};

// ============ 实现 ============

template <typename T, std::size_t Capacity>
void spsc_queue<T, Capacity>::init() noexcept {
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

template <typename T, std::size_t Capacity>
bool spsc_queue<T, Capacity>::try_push(const T& item) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);

    // 检查队列是否已满
    if (((head + 1) & kMask) == tail) {
        return false;
    }

    buffer_[head] = item;
    head_.store((head + 1) & kMask, std::memory_order_release);
    return true;
}

template <typename T, std::size_t Capacity>
bool spsc_queue<T, Capacity>::try_pop(T& item) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);

    // 检查队列是否为空
    if (tail == head) {
        return false;
    }

    item = buffer_[tail];
    tail_.store((tail + 1) & kMask, std::memory_order_release);
    return true;
}

template <typename T, std::size_t Capacity>
bool spsc_queue<T, Capacity>::try_peek(T& item) const noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);

    if (tail == head) {
        return false;
    }

    item = buffer_[tail];
    return true;
}

template <typename T, std::size_t Capacity>
std::size_t spsc_queue<T, Capacity>::size() const noexcept {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return (head - tail + Capacity) & kMask;
}

template <typename T, std::size_t Capacity>
bool spsc_queue<T, Capacity>::empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
}

}  // namespace acct
