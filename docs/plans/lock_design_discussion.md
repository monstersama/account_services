# 锁设计讨论记录

## 讨论时间
2026-02-04

## 参与人员
开发团队

---

## 1. 当前锁的使用位置

### 1.1 订单簿 (order_book)

**位置**: `src/order/order_book.hpp:85`

```cpp
mutable spinlock lock_;  // 全局自旋锁
```

**保护的数据**:
- `std::array<order_entry, kMaxActiveOrders> orders_` - 订单数组
- `std::unordered_map<internal_order_id_t, std::size_t> id_to_index_` - ID到索引映射
- `std::unordered_map<uint64_t, internal_order_id_t> broker_id_map_` - 券商ID映射
- `std::unordered_map<internal_security_id_t, std::vector<internal_order_id_t>> security_orders_` - 证券订单列表
- `std::vector<std::size_t> free_slots_` - 空闲槽位
- `std::size_t active_count_` - 活跃订单计数

**使用场景**:
- 添加新订单 (`add_order`)
- 查找订单 (`find_order`, `find_by_broker_id`)
- 更新订单状态 (`update_status`)
- 更新成交信息 (`update_trade`)
- 归档订单 (`archive_order`)

**问题**: 全局锁在高并发场景下会成为瓶颈

### 1.2 持仓管理器 (position_manager)

**位置**: `src/portfolio/position_manager.hpp:82`

```cpp
mutable spinlock fund_lock_;  // 资金专用锁
```

**保护的数据**:
- `fund_info fund_cache_` - 资金缓存

**使用场景**:
- 冻结/解冻资金
- 扣除/增加资金
- 查询资金

### 1.3 持仓数据行级锁

**位置**: `src/portfolio/positions.h`

```cpp
struct position {
    std::atomic<uint8_t> locked{0};  // 每行一个锁
    // ...
};
```

**特点**: 按证券分散，竞争较低

---

## 2. 无锁化可行性分析

### 2.1 现有原子设施

**order_request 已有原子字段**:
```cpp
// cache line 2
std::atomic<order_status_t> order_status{order_status_t::NotSet};
```

**position 已有行级锁**:
```cpp
struct position {
    std::atomic<uint8_t> locked{0};  // 单行锁
    // ...
};
```

### 2.2 无锁化设计方案

#### 方案A: 订单状态机 - 原子 CAS 操作

状态转换使用 CAS 实现无锁更新:

```cpp
// 状态转换示例
order_status_t expected = order_status_t::RiskControllerPending;
order_status.compare_exchange_strong(
    expected,
    order_status_t::RiskControllerAccepted,
    std::memory_order_acq_rel
);
```

**状态转换流程**:
```
StrategySubmitted -> RiskControllerPending -> RiskControllerAccepted -> TraderPending -> ...
                                            -> RiskControllerRejected (终止)
```

#### 方案B: 订单簿简化 - 只读索引 + 原子状态

```cpp
class order_book {
    // 订单数组 - 预分配，只分配/释放索引
    std::array<order_entry, kMaxActiveOrders> orders_;

    // 使用原子指针管理索引
    alignas(64) std::atomic<size_t> free_list_head_{0};
};
```

#### 方案C: 成交更新无锁流程

```
成交回报处理:
1. 从 response 读取 internal_order_id
2. 查找到订单指针（无锁读取索引）
3. 原子更新订单状态
4. 原子更新成交数量/金额
5. 触发持仓更新（使用 position 行级锁）
```

---

## 3. 关键问题与解决方案

### 3.1 多字段原子更新问题

**问题**: 一次成交需同时更新多个字段
- `volume_traded`
- `dvalue_traded`
- `order_status`

**解决方案**:
- **方案A**: 使用 64bit CAS 打包多个字段（限制较多）
- **方案B**: 定义明确状态，状态变更时其他字段已确定（推荐）

### 3.2 ABA 问题

**问题**: 状态 A->B->A，CAS 误判

**解决方案**:
- 添加版本号（version counter）
- 确保不存在循环状态转换

### 3.3 订单查找并发安全

**问题**: `id_to_index_` 映射表在添加/删除时需要保护

**解决方案**:
- 使用读写锁或分片锁
- 考虑无锁哈希表（如 Folly AtomicHashMap）

---

## 4. 优化后的自旋锁实现

对于必须使用锁的场景，使用 PAUSE 指令优化:

```cpp
#include <emmintrin.h>  // _mm_pause

void spinlock::lock() noexcept {
    // 先忙等待几次（超低延迟路径）
    for (int i = 0; i < 4; ++i) {
        if (!flag_.load(std::memory_order_relaxed)) {
            if (!flag_.exchange(true, std::memory_order_acquire)) {
                return;
            }
        }
        _mm_pause();
    }
    // 指数退避
    for (int i = 0; i < 16; ++i) {
        _mm_pause();
    }
    // 最后才 yield
    while (flag_.exchange(true, std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}
```

**优化点**:
1. `_mm_pause()` 减少功耗和总线流量
2. 指数退避避免过度自旋
3. 延迟 yield 减少上下文切换开销

---

## 5. 实现策略建议

### 阶段1: 基础实现（当前）
- 使用优化的自旋锁（PAUSE 指令）
- 确保功能正确性

### 阶段2: 无锁优化（后续）
- 订单状态使用原子 CAS
- 订单簿索引用读写锁或分片锁
- 资金使用原子操作

### 阶段3: 极致优化（可选）
- 完全无锁设计
- 每 CPU 核心一个订单簿分片
- RCU 模式读取

---

## 6. 决策点

需要决策的问题:

1. **是否立即采用无锁设计？**
   - [ ] 是，直接实现无锁状态机
   - [ ] 否，先使用优化锁，后续再优化

2. **多字段更新一致性方案？**
   - [ ] 64bit CAS 打包
   - [ ] 状态机驱动，状态确定字段值

3. **订单簿索引并发策略？**
   - [ ] 全局锁（简单）
   - [ ] 分片锁（平衡）
   - [ ] 无锁结构（复杂）

---

## 7. 参考资料

- [order_request.hpp](../src/order/order_request.hpp) - 订单请求结构
- [order_book.hpp](../src/order/order_book.hpp) - 订单簿设计
- [position_manager.hpp](../src/portfolio/position_manager.hpp) - 持仓管理
- [positions.h](../src/portfolio/positions.h) - 持仓数据结构
- [spinlock.hpp](../src/common/spinlock.hpp) - 自旋锁接口

---

*最后更新: 2026-02-04*
