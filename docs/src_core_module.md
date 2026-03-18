# `src/core` 模块设计

## 1. 模块定位

`src/core` 负责主服务的生命周期编排，而不是承载具体业务规则。它完成配置加载、日志初始化、共享内存打开、`portfolio / risk / order / shm` 组件装配，并通过 `EventLoop` 驱动订单与回报处理。

源码范围：

- [`src/main.cpp`](../src/main.cpp)
- [`src/core/config_manager.hpp`](../src/core/config_manager.hpp)
- [`src/core/config_manager.cpp`](../src/core/config_manager.cpp)
- [`src/core/account_service.hpp`](../src/core/account_service.hpp)
- [`src/core/account_service.cpp`](../src/core/account_service.cpp)
- [`src/core/event_loop.hpp`](../src/core/event_loop.hpp)
- [`src/core/event_loop.cpp`](../src/core/event_loop.cpp)

## 2. 核心职责

- 加载 YAML 配置，并由 `ConfigManager` 提供可选的命令行覆盖能力。
- 初始化日志与五类共享内存映射。
- 初始化持仓、账户信息、成交记录和委托记录。
- 创建 `RiskManager`、`OrderBook`、`order_router`、`ExecutionEngine` 和 `EventLoop`。
- 驱动上游订单消费、成交回报处理、统计输出和可选归档。
- 统一收敛错误并把关键错误提升为停服 / 退出决策。

## 3. 关键类型与角色

### `ServiceState`

服务状态机：

- `Created`
- `Initializing`
- `Ready`
- `Running`
- `Stopping`
- `Stopped`
- `Error`

### `ConfigManager`

职责：

- 从 YAML 文件装载配置
- 支持命令行覆盖
- 做基本语义校验
- 支持 `reload()` 和 `export_to_file()`

关键配置块：

- `SHMConfig`
- `EventLoopConfig`
- `LogConfig`
- `DBConfig`
- `RiskConfig`
- `split_config`
  - 当前作为执行算法默认参数配置，而不是旧的一次性 splitter 运行时配置

实现说明：

- 同时接受 `event_loop.*` 和 `EventLoop.*` 两种配置键路径。
- `validate()` 只做基础校验，例如 `account_id != 0`、`trading_day` 合法、SHM 名非空、`poll_batch_size > 0`。
- `ConfigManager::parse_command_line()` 具备比当前主程序更完整的命令行覆盖能力。

### `AccountService`

`AccountService` 是服务装配中心。它不直接实现订单业务，而是负责把其他模块连接起来。

关键函数：

- `initialize(const std::string& config_path)`
- `run()`
- `stop()`
- `init_config()`
- `init_shared_memory()`
- `init_portfolio()`
- `init_risk_manager()`
- `init_order_components()`
- `init_market_data()`
- `init_execution_engine()`
- `init_event_loop()`
- `cleanup()`
- `raise_service_error()`

实现说明：

- `initialize()` 会先 `cleanup()`，再重新装配所有组件。
- `init_order_components()` 中会调用 `order_router_->recover_downstream_active_orders()`，把重启恢复作为初始化的一部分。
- `load_positions()` 目前只是一个占位接口；实际持仓加载发生在 `PositionManager::initialize()` 内部。

### `EventLoop`

`EventLoop` 是运行期的真正调度核心。

关键函数：

- `run()`
- `process_upstream_orders()`
- `process_downstream_responses()`
- `handle_order_request()`
- `handle_trade_response()`
- `process_pending_archives()`
- `prepare_order_estimate()`
- `reserve_order_resources()`
- `release_order_resources()`
- `settle_buy_trade_fund()`
- `on_order_book_changed()`

配套统计结构：

- `event_loop_stats`

## 4. 启动与运行流程

### 4.1 启动顺序

当前 `acct_service_main` 的 CLI 入口只支持：

- `--config <path>`
- 位置参数形式的配置路径

启动顺序：

1. `main.cpp` 解析 `--config` 或位置参数配置路径。
2. 创建 `AccountService`。
3. `AccountService::initialize()` 依次执行：
   - 清理旧状态
   - 读取配置
   - 初始化日志
   - 打开 `upstream/downstream/trades/orders/positions` 五类 SHM
   - 初始化 `portfolio` 组件
   - 初始化 `RiskManager`
   - 初始化 `OrderBook` 与 `order_router`
   - 恢复下游在途订单
   - 初始化 `MarketDataService`
   - 初始化 `ExecutionEngine`
   - 创建 `EventLoop`
4. 若全部成功，状态进入 `Ready`。
5. `run()` 将状态推进到 `Running` 并阻塞执行事件循环。

### 4.2 上游订单处理

`EventLoop::process_upstream_orders()` 的主逻辑如下：

1. 从 `upstream_shm->upstream_order_queue` 弹出 `OrderIndex`。
2. 用 `orders_shm_read_snapshot()` 读取稳定快照。
3. 把订单槽位阶段更新为 `UpstreamDequeued`。
4. 调 `handle_order_request()`：
   - 补齐 `internal_security_id`
   - 必要时生成内部订单号
   - 为买单预估手续费
   - 把订单加入 `OrderBook`
   - 推进到 `RiskControllerPending`
   - 对新单调用 `RiskManager`
   - 风控通过后：
     - managed execution 订单进入 `ExecutionEngine`
     - 普通订单仅在买单路径预冻结资金，然后调 `order_router` 发往下游

需要注意：

- 当前只冻结买单资金，不冻结卖单仓位。
- managed execution 父单会先进入 `TraderPending`，普通新单成功路由后直接进入 `TraderSubmitted`。

### 4.3 下游回报处理

`EventLoop::process_downstream_responses()` 的主逻辑如下：

1. 从 `trades_shm->response_queue` 取出 `TradeResponse`。
2. 通过 `OrderBook` 更新订单状态和成交累计值。
3. 对买卖方向分别调用 `PositionManager`：
   - 买单：结算买入资金、必要时建档证券行、增加持仓
   - 卖单：结算卖出资金、扣减持仓
4. 若订单进入终态，释放剩余冻结的买单资金。
5. 若启用了终态延迟归档，将订单加入 `pending_archive_deadlines_ns_`。

### 4.4 订单镜像同步

`EventLoop` 在构造时向 `OrderBook` 注册 `change_callback`。这样 `OrderBook` 的变化会被回写到 `orders_shm`：

- 终态或已归档订单写成 `OrderSlotState::Terminal`
- 风控拒绝写成 `OrderSlotState::RiskRejected`
- 其他场景只同步最新 `OrderRequest`

这让 `orders_shm` 同时承担：

- 用户指令提交入口
- 监控侧只读镜像
- 重启恢复时的状态来源

## 5. 错误处理与停机语义

- `AccountService::raise_service_error()` 会记录 `last_error_`，同时调用 `record_error()` 和日志记录。
- `common/error` 会把错误分类为 `Recoverable / Critical / Fatal`。
- `run()` 返回前，如果 `shutdown_reason >= Critical` 或 `should_stop_service()`，服务会：
  - 请求停止事件循环
  - 刷新日志
  - 将状态置为 `Error`

需要注意：

- `core` 负责“装配和收敛错误”，但错误分级规则定义在 `src/common/error.cpp`。
- `api` 域错误不会替调用方进程做停服决定，而 `core / shm / portfolio` 中的关键错误会触发停机闩锁。

## 6. 依赖与边界

### 依赖其他模块

- `src/common`
  - 错误模型、日志、时间、证券标识工具
- `src/shm`
  - SHM 打开、头部校验、订单池辅助函数
- `src/portfolio`
  - 资金 / 持仓、账户信息、成交 / 委托记录
- `src/risk`
  - 风控规则链
- `src/order`
  - 订单簿、路由、父子索引、重启恢复
- `src/execution`
  - 统一执行会话、行情驱动子单定价、父单镜像回写

### 不负责的内容

- 不定义共享内存协议细节，这由 `src/shm` 负责。
- 不定义风控规则，这由 `src/risk` 负责。
- 不维护订单状态聚合细节，这由 `src/order` 负责。
- 不直接实现对外 SDK 或 ABI，这由 `src/api` 和 `src/broker_api` 负责。

## 7. 相关专题文档

- [订单处理流程图](order_flowchart.md)
- [订单架构图](order_architecture.mmd)
- [订单错误流](order_error_flow.mmd)
- [错误日志设计说明](plans/error_logging_design.md)

## 8. 维护提示

- 修改初始化顺序时，优先检查 `AccountService::initialize()` 和这份文档的启动链路是否一致。
- 修改订单进入或回报处理路径时，优先同步 `EventLoop` 相关章节，并检查 [订单处理流程图](order_flowchart.md) 是否需要更新。
- 若主程序 CLI 行为变化，需同时检查 `src/main.cpp` 与这份文档中“ConfigManager 能力”和“当前入口行为”的区分是否仍然准确。
