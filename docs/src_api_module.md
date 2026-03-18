# `src/api` 模块设计

## 1. 模块定位

`src/api` 是面向外部进程的稳定 C ABI 适配层，分为三组接口：

- 下单 SDK
- 订单池只读监控 SDK
- 持仓池只读监控 SDK

它不参与账户服务主循环中的风控、路由和成交结算；它只负责“外部接入”和“只读观测”。

源码范围：

- [`src/api/order_api.cpp`](../src/api/order_api.cpp)
- [`src/api/order_monitor_api.cpp`](../src/api/order_monitor_api.cpp)
- [`src/api/position_monitor_api.cpp`](../src/api/position_monitor_api.cpp)
- [`include/api/order_api.h`](../include/api/order_api.h)
- [`include/api/order_monitor_api.h`](../include/api/order_monitor_api.h)
- [`include/api/position_monitor_api.h`](../include/api/position_monitor_api.h)

## 2. 三组接口的职责边界

### 2.1 `order_api`

职责：

- 接收外部下单 / 撤单请求
- 构造内部 `OrderRequest`
- 把订单写入 `orders_shm`
- 把对应槽位索引推入 `upstream_shm->upstream_order_queue`

这组接口是“策略 / 调用方 -> 账户服务”的入口适配层。

### 2.2 `order_monitor_api`

职责：

- 只读打开 `orders_shm`
- 读取订单池头部元信息
- 按索引读取稳定订单快照

这组接口是订单池的只读观测面。

### 2.3 `position_monitor_api`

职责：

- 只读打开 `positions_shm`
- 读取资金行和证券行快照
- 在并发读冲突时返回 `RETRY`

这组接口是资金 / 持仓状态的只读观测面。

## 3. `order_api` 设计

### 3.1 核心 ABI

头文件：

- [`include/api/order_api.h`](../include/api/order_api.h)

关键接口：

- `acct_init()`
- `acct_init_ex()`
- `acct_destroy()`
- `acct_new_order()`
- `acct_new_order_ex()`
- `acct_send_order()`
- `acct_submit_order()`
- `acct_submit_order_ex()`
- `acct_cancel_order()`
- `acct_queue_size()`
- `acct_strerror()`
- `acct_version()`
- `acct_cleanup_shm()`

关键扩展类型：

- `acct_init_options_t`
- `acct_order_exec_options_t`
- `acct_passive_exec_algo_t`

关键特点：

- 使用不透明句柄 `acct_ctx_t`
- 错误码为 `acct_error_t`
- 允许通过 `acct_init_options_t` 指定：
  - `upstream_shm_name`
  - `orders_shm_name`
  - `trading_day`
  - `create_if_not_exist`
- 允许通过 `_ex` 接口为单笔订单指定 `passive_exec_algo`

### 3.2 初始化流程

`acct_init_ex()` 的流程：

1. 解析 `acct_init_options_t`
2. 补齐默认值
   - `upstream_shm_name` 默认 `/upstream_order_shm`
   - `orders_shm_name` 默认 `/orders_shm`
   - `trading_day` 默认环境变量 `ACCT_TRADING_DAY`，否则 `19700101`
3. 创建 `acct_context`
4. 用 `SHMManager` 打开：
   - 上游 SHM
   - 带交易日后缀的 `orders_shm`
5. 当允许创建且遇到 `ShmResizeFailed` / `ShmHeaderInvalid` 时，当前实现仍可能尝试 `unlink + recreate`

### 3.3 下单与撤单数据流

#### 直接提交路径

1. 调用 `acct_submit_order(...)` 或 `acct_submit_order_ex(...)`
2. 校验 `security_id / side / market / volume`
3. 解析 `acct_order_exec_options_t.passive_exec_algo`
4. 构造 `InternalSecurityId`
5. 从 `upstream_shm->header.next_order_id` 分配内部订单 ID
6. 构造 `OrderRequest`
7. 写入 `orders_shm`，阶段记为 `UpstreamQueued`
8. 把 `OrderIndex` 推入上游队列

#### 两阶段提交路径

1. `acct_new_order(...)` / `acct_new_order_ex(...)` 只创建并缓存订单到 `acct_context::cached_orders`
2. `acct_send_order(order_id)` 再把缓存订单真正入队

适用场景：

- 上层需要先创建订单、后决定是否真正发出

#### 撤单路径

1. `acct_cancel_order(...)` 分配新的内部撤单 ID
2. 调 `OrderRequest::init_cancel(...)`
3. 复用相同的 `enqueue_order(...)` 路径写入订单池并推送上游索引

### 3.4 被动执行算法透传

`_ex` 接口支持的逐单被动执行算法：

- `ACCT_PASSIVE_EXEC_DEFAULT`
- `ACCT_PASSIVE_EXEC_NONE`
- `ACCT_PASSIVE_EXEC_FIXED_SIZE`
- `ACCT_PASSIVE_EXEC_TWAP`
- `ACCT_PASSIVE_EXEC_VWAP`
- `ACCT_PASSIVE_EXEC_ICEBERG`

这一步只负责把 ABI 枚举转成内部 `PassiveExecutionAlgo`，真正是否进入 managed execution 由账户服务侧的 `EventLoop` 和 `ExecutionEngine` 决定。

### 3.5 与主服务的关系

`order_api` 不直接调用 `AccountService` 或 `EventLoop`，它与主服务之间通过共享内存解耦：

- 写入 `orders_shm`
- 推送 `upstream_shm->upstream_order_queue`

之后才由 `src/core/EventLoop` 消费并进入真正的订单处理链。

## 4. `order_monitor_api` 设计

### 4.1 核心 ABI

头文件：

- [`include/api/order_monitor_api.h`](../include/api/order_monitor_api.h)

关键接口：

- `acct_orders_mon_open()`
- `acct_orders_mon_close()`
- `acct_orders_mon_info()`
- `acct_orders_mon_read()`
- `acct_orders_mon_strerror()`

关键快照类型：

- `acct_orders_mon_info_t`
- `acct_orders_mon_snapshot_t`

### 4.2 只读快照模型

当前实现不暴露内部 `OrderRequest` 原始内存布局，而是输出稳定 C ABI 快照结构：

- 避免第三方绑定到 `std::atomic`、padding、对齐细节
- 允许内部 `OrderRequest` 演进而不直接破坏外部 ABI

### 4.3 读取流程

1. `acct_orders_mon_open()` 通过 `basecore_shm_bridge::open_reader(...)` 只读打开带交易日后缀的 `orders_shm`
2. 校验 `OrdersHeader`
3. `acct_orders_mon_info()` 返回：
   - 容量
   - `next_index`
   - `full_reject_count`
   - `trading_day`
4. `acct_orders_mon_read(index)` 通过双读 `seq` 的方式读取稳定快照
5. 若遇到并发写入窗口，返回 `ACCT_MON_ERR_RETRY`

## 5. `position_monitor_api` 设计

### 5.1 核心 ABI

头文件：

- [`include/api/position_monitor_api.h`](../include/api/position_monitor_api.h)

关键接口：

- `acct_positions_mon_open()`
- `acct_positions_mon_close()`
- `acct_positions_mon_info()`
- `acct_positions_mon_read_fund()`
- `acct_positions_mon_read_position()`
- `acct_positions_mon_strerror()`

关键快照类型：

- `acct_positions_mon_info_t`
- `acct_positions_mon_fund_snapshot_t`
- `acct_positions_mon_position_snapshot_t`

### 5.2 读取语义

`position_monitor_api` 把资金行和证券行分开提供：

- FUND 行固定读取 `positions[0]`
- 证券行按逻辑索引读取 `positions[index + 1]`

读一致性策略：

- 读取前后检查行锁 `locked`
- 若发现并发写冲突，返回 `ACCT_POS_MON_ERR_RETRY`

打开共享内存时，当前实现同样通过 `basecore_shm_bridge::open_reader(...)` 统一收口读端行为，而不是把底层 `shm_open/mmap` 细节直接暴露给调用方。

## 6. 依赖与边界

### 依赖其他模块

- `src/shm`
  - `order_api` 使用 `SHMManager`
  - 监控 API 使用 `basecore_shm_bridge` 和 `shm_layout`
- `src/order`
  - `order_api` 构造内部 `OrderRequest`
- `src/common`
  - 常量、错误、日志、类型、证券标识
- `src/portfolio`
  - `position_monitor_api` 依赖 `positions.h`

### 与主服务的边界

- `src/api` 不直接共享 `AccountService` 或 `EventLoop` 对象
- 所有交互都通过共享内存完成

这让它可以被视为：

- 外部入口层
- 外部观测层

而不是账户服务内部业务层。

## 7. 相关专题文档

- [订单监控 SDK 使用文档](order_monitor_sdk.md)
- [订单处理流程图](order_flowchart.md)

## 8. 维护提示

- 若修改 `OrderRequest` 内部字段，不要直接把内部布局泄露到 C ABI；优先考虑是否需要扩展稳定快照结构。
- 若修改 `orders_shm` 或 `positions_shm` 头部校验规则，必须同步检查监控 API 的打开逻辑。
- 若调整下单默认 SHM 名称、交易日策略或 `_ex` 接口选项，需要同步更新：
  - `order_api`
  - 监控 API
  - 相关使用文档
