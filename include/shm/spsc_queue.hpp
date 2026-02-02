#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace acct {

// 单生产者单消费者无锁环形队列
// T 必须是 trivially copyable
// Capacity 必须是 2 的幂
template <typename T, std::size_t Capacity>
class alignas(64) spsc_queue {
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
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

}  // namespace acct
