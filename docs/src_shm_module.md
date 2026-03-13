# `src/shm` 模块设计

## 1. 模块定位

`src/shm` 定义跨进程共享内存协议、共享内存布局和访问辅助函数，是策略进程、账户服务、交易进程以及监控侧之间共享状态的通信底座。

源码范围：

- [`src/shm/shm_layout.hpp`](../src/shm/shm_layout.hpp)
- [`src/shm/orders_shm.hpp`](../src/shm/orders_shm.hpp)
- [`src/shm/shm_manager.hpp`](../src/shm/shm_manager.hpp)
- [`src/shm/shm_manager.cpp`](../src/shm/shm_manager.cpp)
- [`src/shm/spsc_queue.hpp`](../src/shm/spsc_queue.hpp)

## 2. 核心职责

- 定义上游、下游、成交回报、订单池、持仓池五类 SHM 布局。
- 定义订单池槽位协议和稳定快照读取方式。
- 负责 `shm_open / ftruncate / mmap / munmap / shm_unlink` 的 RAII 风格封装。
- 校验 SHM 头部、尺寸、版本、交易日等关键元数据。

## 3. 五类共享内存对象

### 3.1 队列型 SHM

#### `upstream_shm_layout`

用途：

- 策略 / SDK -> 账户服务

载荷：

- `spsc_queue<OrderIndex, kUpstreamOrderQueueCapacity>`

#### `downstream_shm_layout`

用途：

- 账户服务 -> 交易进程

载荷：

- `spsc_queue<OrderIndex, kDownstreamQueueCapacity>`

#### `trades_shm_layout`

用途：

- 交易进程 -> 账户服务

载荷：

- `spsc_queue<TradeResponse, kResponseQueueCapacity>`

### 3.2 镜像型 SHM

#### `orders_shm_layout`

用途：

- 保存完整 `OrderRequest` 镜像
- 供监控只读
- 供重启恢复扫描

组成：

- `OrdersHeader`
- `OrderSlot slots[kDailyOrderPoolCapacity]`

#### `positions_shm_layout`

用途：

- 保存 FUND 行和证券持仓行
- 供监控只读
- 供服务重启恢复状态索引

组成：

- `PositionsHeader`
- `position_count`
- `position positions[kMaxPositions]`

## 4. 头部结构与元数据

### 4.1 `SHMHeader`

服务于三类队列型 SHM，包含：

- `magic`
- `version`
- `create_time`
- `last_update`
- `next_order_id`

### 4.2 `OrdersHeader`

订单池专用头部，除了版本信息，还额外包含：

- `header_size`
- `total_size`
- `capacity`
- `init_state`
- `next_index`
- `full_reject_count`
- `trading_day`

需要注意：

- 订单池按“交易日命名 + 交易日校验”工作
- 名称中的交易日后缀必须与头部中的 `trading_day` 匹配

### 4.3 `PositionsHeader`

持仓池专用头部，包含：

- `header_size`
- `total_size`
- `capacity`
- `init_state`
- `id`

其中：

- `init_state == 1` 表示可读
- `id` 由 `PositionManager` 维护，用作下一个证券编号

## 5. 订单池协议

### 5.1 `OrderSlot`

每个订单槽位包含：

- `seq`
- `last_update_ns`
- `stage`
- `source`
- `request`

其中 `seq` 采用 seqlock 风格协议：

- 奇数：写入中
- 偶数：稳定可读

### 5.2 `OrderSlotState`

槽位阶段描述的是“跨进程链路位置”，不是业务订单状态。典型取值：

- `UpstreamQueued`
- `UpstreamDequeued`
- `RiskRejected`
- `DownstreamQueued`
- `DownstreamDequeued`
- `Terminal`
- `QueuePushFailed`

### 5.3 `orders_shm.hpp` 提供的关键辅助函数

- `make_orders_shm_name(...)`
- `extract_trading_day_from_name(...)`
- `orders_shm_try_allocate(...)`
- `orders_shm_write_order(...)`
- `orders_shm_sync_order(...)`
- `orders_shm_update_stage(...)`
- `orders_shm_append(...)`
- `orders_shm_read_snapshot(...)`

写侧基本模式：

1. 申请槽位
2. 写入 `OrderRequest`
3. 设置阶段和来源
4. 把 `OrderIndex` 推入对应队列

读侧基本模式：

1. 检查索引是否可见
2. 读取 `seq0`
3. 拷贝快照
4. 再读 `seq1`
5. 只有 `seq0 == seq1` 且为偶数时才返回成功

## 6. `SHMManager` 生命周期

`SHMManager` 是 SHM 对象的访问器和资源拥有者。

关键能力：

- `open_upstream(...)`
- `open_downstream(...)`
- `open_trades(...)`
- `open_orders(...)`
- `open_positions(...)`
- `close()`
- `unlink(...)`

### 6.1 打开流程

`open_impl(...)` 根据 `shm_mode` 执行：

- `Create`
- `Open`
- `OpenOrCreate`

随后执行：

1. `shm_open`
2. 必要时 `fchmod`
3. `fstat`
4. 新建对象时 `ftruncate`
5. `mmap`
6. 记录 `name_ / ptr_ / size_ / fd_`

### 6.2 新建对象与既有对象的差异

- 新建时：
  - 写入头部默认值
  - 记录 `last_open_is_new_ = true`
- 打开既有对象时：
  - 做版本、尺寸、容量、交易日等校验
  - 校验失败会记录错误并关闭映射

### 6.3 关闭与删除

- `close()`
  - `munmap + close(fd)`
  - 清空内部状态
- `unlink(name)`
  - 删除 SHM 对象名
  - 仅删除名字，不负责关闭其他进程已有映射

## 7. `spsc_queue` 的角色

`spsc_queue<T, Capacity>` 是队列型 SHM 的核心基础件，特点：

- 单生产者单消费者
- 固定容量环形缓冲区
- `Capacity` 必须是 2 的幂
- 对外暴露：
  - `try_push`
  - `try_pop`
  - `try_peek`
  - `size`
  - `empty`

这里的约束决定了：

- 队列语义简单明确
- 上游/下游/回报通道都假设“单写单读”的进程拓扑

## 8. 依赖与边界

### 依赖其他模块

- `src/common`
  - 常量、类型、错误与日志
- `src/order`
  - `OrderRequest` 被嵌入 `OrderSlot`
- `src/portfolio`
  - `position` 被嵌入 `positions_shm_layout`

### 被哪些模块使用

- `src/core`
  - 打开所有 SHM，并驱动订单/回报链路
- `src/api`
  - 下单 SDK 写入 `orders_shm`，监控 SDK 只读打开 SHM
- 交易进程 / gateway
  - 消费或产出队列数据

## 9. 相关专题文档

- [订单处理流程图](order_flowchart.md)
- [SHM manager 方案记录](plans/implemented/shm_manager_plan.md)

## 10. 维护提示

- 修改 `OrderSlot`、`OrdersHeader`、`PositionsHeader` 布局时，必须同步检查：
  - 监控 SDK 读取逻辑
  - 重启恢复逻辑
  - 头部版本号和校验逻辑
- `OrderSlotState` 与业务订单状态不是一回事，新增阶段时要同步检查监控文档与订单文档。
- 若改变订单池命名规则或交易日策略，必须同时更新：
  - `orders_shm.hpp`
  - `SHMManager::open_orders()`
  - 订单监控 SDK
