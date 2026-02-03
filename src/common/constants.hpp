#pragma once

#include <cstddef>
#include <cstdint>

namespace acct {

// 队列容量定义（必须是2的幂）
inline constexpr std::size_t kStrategyOrderQueueCapacity = 65536;
inline constexpr std::size_t kDownstreamQueueCapacity = 65536;
inline constexpr std::size_t kResponseQueueCapacity = 131072;
inline constexpr std::size_t kMaxPositions = 8192;
inline constexpr std::size_t kMaxActiveOrders = 1048576;  // 2^20

// 默认共享内存名称
inline constexpr const char* kStrategyOrderShmName = "/strategy_order_shm";

// 共享内存魔数和版本
inline constexpr uint32_t kShmMagic = 0x41435354;  // "ACST"
inline constexpr uint32_t kShmVersion = 1;

// 字符串长度常量
inline constexpr std::size_t kBrokerOrderIdSize = 32;
inline constexpr std::size_t kSecurityIdSize = 16;
inline constexpr std::size_t kAccountNameSize = 32;
inline constexpr std::size_t kBrokerCodeSize = 16;

// Cache line size
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace acct
