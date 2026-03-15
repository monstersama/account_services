# `src/order` 模块设计

## 1. 模块定位

`src/order` 负责账户服务内部的订单登记、索引维护、撤单路由、内部子单登记和重启恢复。它不再承担“一次性拆单运行时”的职责；执行算法推进已经迁到 `src/execution`。

源码范围：

- [`src/order/order_request.hpp`](../src/order/order_request.hpp)
- [`src/order/order_book.hpp`](../src/order/order_book.hpp)
- [`src/order/order_book.cpp`](../src/order/order_book.cpp)
- [`src/order/order_router.hpp`](../src/order/order_router.hpp)
- [`src/order/order_router.cpp`](../src/order/order_router.cpp)
- [`src/order/order_recovery.hpp`](../src/order/order_recovery.hpp)
- [`src/order/order_recovery.cpp`](../src/order/order_recovery.cpp)
- [`src/order/passive_execution.hpp`](../src/order/passive_execution.hpp)

## 2. 核心职责

- 为进入系统的订单建立本地簿内状态。
- 维护内部订单 ID、券商订单号、证券维度和父子单维度的索引。
- 为普通订单和内部子单分配 `orders_shm` 槽位并推送到下游。
- 对父单撤单请求执行“对子单扇出撤单”。
- 为执行引擎托管父单提供镜像同步接口。
- 在服务重启时从 `orders_shm` 恢复已在途但未终态的订单。

## 3. 核心模型

### 3.1 `OrderEntry`

`OrderEntry` 是订单簿内的扩展态对象，除了 `OrderRequest` 本身，还保存：

- `submit_time_ns`
- `last_update_ns`
- `strategy_id`
- `risk_result`
- `retry_count`
- `is_split_child`
- `parent_order_id`
- `shm_order_index`
- `fund_frozen`

其中：

- `request` 记录订单业务状态和监控镜像字段
- `shm_order_index` 把簿内对象和 `orders_shm` 槽位绑定起来
- `fund_frozen` 供 `EventLoop` 在成交或终态时释放剩余冻结资金

### 3.2 `OrderBook`

`OrderBook` 是活跃订单的本地索引中心。

核心实现特点：

- 固定容量数组 `orders_`
- 空闲槽位栈 `free_slots_`
- `internal_order_id -> array index`
- `broker_order_id -> internal_order_id`
- `security -> order_ids`
- `parent -> children`
- `child -> parent`
- `managed_parent_ids_`
- `SpinLock` 串行保护内部状态

当前有两类父单语义：

- 普通父单：仍允许用子单聚合刷新父单镜像
- 执行引擎托管父单：通过 `mark_managed_parent()` 和 `sync_managed_parent_view(...)` 由 `ExecutionEngine` 显式回写镜像，不再走旧的 `volume_remain` 聚合

### 3.3 `order_router`

`order_router` 现在只负责路由，不再在运行时做一次性拆单。

当前职责：

- 普通新单：直接把已有槽位索引推入 `downstream_shm->order_queue`
- 撤单：为原单或其子单生成内部撤单请求，再推下游
- 内部子单：接收 `ExecutionEngine::submit_child()` 生成的子单并统一登记/下发
- 恢复：从 `orders_shm` 重建“已下游但未终态”的订单

### 3.4 `order_recovery`

`order_recovery` 用于重启后恢复本地簿状态，避免交易进程回报到达时找不到对应订单。

恢复范围：

- 只恢复 `DownstreamQueued / DownstreamDequeued`
- 只恢复非终态订单
- 不恢复仍停留在上游阶段的订单

## 4. 关键运行流程

### 4.1 新单进入本地簿

1. `EventLoop` 从 `orders_shm` 读取稳定快照。
2. 构造 `OrderEntry` 并调用 `OrderBook::add_order()`。
3. `OrderBook` 分配固定槽位并建立各类索引。
4. 订单状态推进为 `RiskControllerPending`。
5. 风控通过后：
   - 普通订单进入 `order_router::route_order()`
   - 执行算法订单进入 `ExecutionEngine`

### 4.2 普通新单下游路由

1. `order_router::route_order()` 判断 `order_type`。
2. 新单路径直接检查 `shm_order_index` 是否有效。
3. 把索引推入 `downstream_shm->order_queue`。
4. 更新订单状态为 `TraderSubmitted`，并把槽位阶段推进到 `DownstreamQueued`。

### 4.3 执行引擎内部子单提交

1. `ExecutionEngine` 生成内部子单 `OrderEntry`。
2. 调用 `order_router::submit_internal_order(...)`。
3. `order_router` 为子单分配槽位并加入 `OrderBook`。
4. 子单以 `is_split_child=true` 和 `parent_order_id!=0` 的形式登记父子关系。
5. 子单被推入下游队列，等待 gateway / broker 链路处理。

### 4.4 撤单路由

撤单分两种情况：

- 原单没有子单
  - 生成一个撤单请求并推下游
- 原单已有子单
  - 为每个活跃子单生成一笔内部撤单请求
  - 第一个子撤单复用传入 `cancel_id`
  - 后续子撤单向 `OrderBook` 重新发号

### 4.5 重启恢复

`recover_downstream_active_orders_from_shm()` 的流程：

1. 扫描 `orders_shm->slots[0..next_index)`。
2. 只筛选已进入下游阶段的槽位。
3. 读取稳定快照，必要时重试以规避 seqlock 写入窗口。
4. 按 `internal_order_id` 去重，保留最新快照。
5. 重建 `OrderEntry` 并写回 `OrderBook`。
6. 用最大恢复订单 ID 与 `upstream_shm->header.next_order_id` 合并，抬升 `OrderBook` 的本地发号器。

## 5. 两套状态不要混淆

### 业务订单状态

来自 `OrderRequest::order_state`，例如：

- `RiskControllerPending`
- `RiskControllerAccepted`
- `TraderSubmitted`
- `BrokerAccepted`
- `MarketAccepted`
- `Finished`

### 订单池槽位阶段

来自 `OrderSlotState`，例如：

- `UpstreamQueued`
- `UpstreamDequeued`
- `RiskRejected`
- `DownstreamQueued`
- `Terminal`

区别：

- 业务状态描述订单语义
- 槽位阶段描述订单镜像在跨进程链路中的位置

## 6. 依赖与边界

### 依赖其他模块

- `src/common`
  - 错误码、日志、基础类型、并发工具
- `src/shm`
  - 订单池槽位协议、快照读写、下游队列投递
- `src/core`
  - `EventLoop` 是主要调用者
- `src/execution`
  - 为受管父单回写执行态镜像

### 不负责的内容

- 不直接决定风控是否通过
- 不直接修改资金和持仓
- 不管理共享内存对象的创建/映射生命周期
- 不再负责一次性拆单算法推进

## 7. 相关专题文档

- [execution 模块设计](src_execution_module.md)
- [core 模块设计](src_core_module.md)
- [src 模块总览](src_modules_overview.md)
