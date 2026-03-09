# `src/order` 模块设计

## 1. 模块定位

`src/order` 负责账户服务内部的订单接纳、跟踪、拆单、撤单路由和重启恢复。它不负责配置加载、资金/仓位校验和共享内存对象生命周期，但会大量操作订单池镜像与下游队列。

源码范围：

- [`src/order/order_request.hpp`](../src/order/order_request.hpp)
- [`src/order/order_book.hpp`](../src/order/order_book.hpp)
- [`src/order/order_book.cpp`](../src/order/order_book.cpp)
- [`src/order/order_router.hpp`](../src/order/order_router.hpp)
- [`src/order/order_router.cpp`](../src/order/order_router.cpp)
- [`src/order/order_splitter.hpp`](../src/order/order_splitter.hpp)
- [`src/order/order_splitter.cpp`](../src/order/order_splitter.cpp)
- [`src/order/order_recovery.hpp`](../src/order/order_recovery.hpp)
- [`src/order/order_recovery.cpp`](../src/order/order_recovery.cpp)

## 2. 核心职责

- 为进入系统的订单建立本地簿内状态。
- 维护内部订单 ID、券商订单号、证券维度和父子单维度的索引。
- 对需要拆单的订单生成子单并维护母单聚合状态。
- 把新单或撤单请求写入订单池后推向下游交易进程。
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

- `request` 是订单业务状态
- `shm_order_index` 把簿内对象和 `orders_shm` 槽位关联起来
- `fund_frozen` 让 `EventLoop` 可以在成交或终态时回收买单剩余冻结资金

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
- `SpinLock` 串行保护内部状态

`OrderBook` 通过 `set_change_callback()` 把簿内变更通知给外部。当前实际回调注册方是 `EventLoop`，它会据此回写 `orders_shm`。

### 3.3 `order_router`

`order_router` 负责把订单变成“可下游消费的索引”：

- 普通新单：直接把已有槽位索引推入 `downstream_shm->order_queue`
- 拆单新单：为每个子单新建槽位，再逐个推下游
- 撤单：为原单或其子单生成内部撤单请求，再推下游

### 3.4 `order_splitter`

`order_splitter` 是纯拆分策略执行器。

当前实现支持：

- `FixedSize`
- `Iceberg`
- `TWAP`

需要注意：

- `Iceberg` 目前复用 `FixedSize` 实现。
- `VWAP` 枚举已存在，配置解析也接受 `vwap`，但当前源码没有实际 `VWAP` 拆单实现。

### 3.5 `order_recovery`

`order_recovery` 用于重启后恢复本地簿状态，避免交易进程回报到达时找不到对应订单。

恢复范围很明确：

- 只恢复 `DownstreamQueued / DownstreamDequeued`
- 只恢复非终态订单
- 不恢复仍停留在上游阶段的订单

## 4. 关键运行流程

### 4.1 新单进入本地簿

1. `EventLoop` 读取 `orders_shm` 中的订单快照。
2. 构造 `OrderEntry` 并调用 `OrderBook::add_order()`。
3. `OrderBook` 分配固定槽位并建立各类索引。
4. 订单状态推进为 `RiskControllerPending`。
5. 风控通过后，由 `order_router::route_order()` 继续处理。

### 4.2 普通新单下游路由

1. `order_router::route_order()` 判断 `order_type`。
2. 若不是撤单，再判断是否需要拆单。
3. 对不拆单订单：
   - 检查 `shm_order_index` 是否有效
   - 直接把索引推入 `downstream_shm->order_queue`
   - 更新订单状态为 `TraderSubmitted`
   - 把槽位阶段更新为 `DownstreamQueued`

### 4.3 母单拆分与子单聚合

1. `order_router::handle_split_order()` 调 `order_splitter::split()` 生成子单。
2. 每个子单：
   - 在 `orders_shm` 新分配槽位
   - 以 `is_split_child=true` 方式加入 `OrderBook`
   - 记录 `parent_order_id`
   - 推送到下游
3. `OrderBook::refresh_parent_from_children_nolock()` 在以下场景被触发：
   - 子单加入
   - 子单状态变化
   - 子单成交变化
4. 聚合逻辑会回写母单的：
   - `volume_traded`
   - `volume_remain`
   - `dvalue_traded`
   - `dfee_executed`
   - `dprice_traded`
   - `order_state`

状态推进规则：

- 若父单被锁存为错误，则保持 `TraderError`
- 若所有子单都已终态，则父单推进到 `Finished`
- 否则父单采用“当前最前进的子单状态”

### 4.4 撤单路由

撤单分两种情况：

- 原单没有子单
  - 生成一个撤单请求并推下游
- 原单已有子单
  - 为每个活跃子单生成一笔内部撤单请求
  - 第一个子撤单复用传入 `cancel_id`
  - 后续子撤单重新向 `OrderBook` 发号

### 4.5 重启恢复

`recover_downstream_active_orders_from_shm()` 的流程：

1. 扫描 `orders_shm->slots[0..next_index)`。
2. 只筛选已进入下游阶段的槽位。
3. 读取稳定快照，必要时重试以规避 seqlock 写入窗口。
4. 按 `internal_order_id` 去重，保留最新快照。
5. 重建 `OrderEntry` 并写回 `OrderBook`。
6. 用最大恢复订单 ID 与 `upstream_shm->header.next_order_id` 合并，抬升 `OrderBook` 的本地发号器。

## 5. 两套状态不要混淆

`src/order` 同时面对两种状态：

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

文档、代码和监控逻辑都应把这两套状态分开描述。

## 6. 依赖与边界

### 依赖其他模块

- `src/common`
  - 错误码、日志、基础类型、并发工具
- `src/shm`
  - 订单池槽位协议、快照读写、下游队列投递
- `src/risk`
  - 风控结果由 `EventLoop` 注入本模块流程
- `src/portfolio`
  - 资源冻结与成交结算发生在父流程，不在本模块内部
- `src/core`
  - `EventLoop` 是本模块的主要调用者

### 不负责的内容

- 不直接决定风控是否通过。
- 不直接修改资金和持仓。
- 不管理共享内存对象的创建/映射生命周期。

## 7. 相关专题文档

- [订单处理流程图](order_flowchart.md)
- [订单架构图](order_architecture.mmd)
- [订单错误流](order_error_flow.mmd)
- [订单状态图](order_state_diagram.mmd)
- [拆单父子单跟踪方案](plans/order_book_split_tracking_plan_zh.md)
- [陈旧成交回报与找单问题方案](plans/stale_trade_response_order_not_found_plan_zh.md)

## 8. 维护提示

- 若修改 `OrderBook` 索引关系或母子单聚合逻辑，优先同步这份文档和相关专题方案。
- 若修改 `orders_shm` 槽位写回规则，必须同时检查 `src/shm` 文档和监控 SDK 文档。
- 若新增拆单策略，除更新 `order_splitter` 外，还要同步说明：
  - 何时触发拆单
  - 子单发号方式
  - 母单状态如何聚合
