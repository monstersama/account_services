# `src/` 模块设计总览

本文档为 `src/` 一级模块的统一入口，帮助开发者快速建立源码心智模型。它不替代现有专题文档，而是把 `src` 下的核心模块、运行链路和已有专题资料串成一条稳定阅读路径。

## 1. 阅读地图

| 模块 | 主要职责 | 模块文档 |
| --- | --- | --- |
| `src/core` | 服务生命周期、配置加载、组件装配、事件循环 | [core 模块设计](src_core_module.md) |
| `src/order` | 订单簿、内部子单登记、撤单路由、重启恢复 | [order 模块设计](src_order_module.md) |
| `src/execution` | 统一执行会话、父单镜像回写、主动优先/被动回落调度 | [execution 模块设计](src_execution_module.md) |
| `src/strategy` | 主动策略抽象、内建策略注册与时间片决策接口 | [strategy 模块设计](src_strategy_module.md) |
| `src/market_data` | `snapshot_reader` 适配、行情与 prediction 统一读视图 | [market_data 模块设计](src_market_data_module.md) |
| `src/portfolio` | 资金/持仓状态、账户信息、成交与委托记录 | [portfolio 模块设计](src_portfolio_module.md) |
| `src/risk` | 风控规则链与风控统计 | [risk 模块设计](src_risk_module.md) |
| `src/shm` | 共享内存布局、订单池协议、SHM 打开与校验 | [shm 模块设计](src_shm_module.md) |
| `src/common` | 错误模型、日志、时间工具、并发与定长字符串 | [common 模块设计](src_common_module.md) |
| `src/api` | 下单 SDK、订单监控 SDK、持仓监控 SDK | [api 模块设计](src_api_module.md) |
| `src/broker_api` | 券商适配 ABI 壳层与版本导出 | [broker_api 模块设计](src_broker_api_module.md) |

## 2. 入口与装配关系

`src/` 中真正的进程入口只有一个：

- [`src/main.cpp`](../src/main.cpp)
  - 解析 `--config`
  - 创建 `AccountService`
  - 初始化服务并执行 `run()`

`src/CMakeLists.txt` 主要用来说明核心模块如何组合成库，而不是定义唯一的模块边界：

- [`src/CMakeLists.txt`](../src/CMakeLists.txt)
  - `acct_common` 对应 `src/common`
  - `acct_shm` 对应 `src/shm`
  - `acct_portfolio` 对应 `src/portfolio`
  - `acct_order_core` 对应 `src/order`
  - `acct_market_data` 对应 `src/market_data`
  - `acct_strategy` 对应 `src/strategy`
  - `acct_execution` 对应 `src/execution`
  - `acct_risk` 对应 `src/risk`
  - `acct_core_loop` 与 `acct_core_service` 对应 `src/core`
  - `acct_broker_api` 对应 `src/broker_api`
  - `acct_service_main` 是主程序入口

阅读源码时，建议把构建目标看作“装配结果”，把一级目录看作“认知边界”。

## 3. 主运行链路

### 3.1 启动链路

1. `main.cpp` 解析命令行并创建 `AccountService`。
2. `AccountService::initialize()` 调用 `ConfigManager` 加载配置。
3. 初始化日志系统。
4. 打开五类共享内存：
   - `upstream_shm`
   - `downstream_shm`
   - `trades_shm`
   - `orders_shm`
   - `positions_shm`
5. 初始化 `portfolio` 子系统：
   - `account_info_manager`
   - `PositionManager`
   - `trade_record_manager`
   - `entrust_record_manager`
6. 初始化 `RiskManager`。
7. 初始化 `OrderBook`、`order_router`，并执行订单恢复。
8. 初始化 `MarketDataService`。
9. 初始化 `ExecutionEngine` 与服务级主动策略对象。
10. 构造 `EventLoop`，进入 `Ready` 状态。
11. `run()` 启动事件循环，服务进入 `Running`。

### 3.2 新单 / 撤单链路

1. 外部策略或 SDK 通过 `src/api/order_api.cpp` 写入 `orders_shm`。
2. `order_api` 把订单槽位索引推入 `upstream_shm->upstream_order_queue`。
3. `EventLoop` 从上游队列取出索引，并从 `orders_shm` 读取订单快照。
4. 订单进入 `OrderBook`，状态推进到 `RiskControllerPending`。
5. `RiskManager` 对新单执行规则链校验。
6. 风控通过后，`EventLoop` 分两条路径：
   - 受管执行算法订单：创建 `ExecutionEngine` 会话
   - 普通订单：冻结必要资源并调用 `order_router`
7. `ExecutionEngine` 对 `FixedSize / Iceberg / TWAP` 持续推进：
   - 先尝试主动策略
   - 若无主动子单，再回落到被动执行
   - 所有 managed child 均先读取行情，再按盘口派生价格发单
8. `order_router` 负责：
   - 普通订单直接推送下游
   - 接收执行引擎提交的内部子单
   - 为父单或普通单生成撤单请求
9. 下游交易进程消费 `downstream_shm->order_queue`。

### 3.3 成交 / 状态回报链路

1. 交易进程把 `TradeResponse` 写入 `trades_shm->response_queue`。
2. `EventLoop` 批量消费回报。
3. `OrderBook` 更新订单状态与成交累计值。
4. `PositionManager` 结算买卖资金并更新证券持仓。
5. 若订单进入终态：
   - 释放剩余冻结资源
   - 可选进入延迟归档队列
   - 最终由 `OrderBook::archive_order()` 归档

### 3.4 观测与外部边界

- `src/api/order_monitor_api.cpp` 只读打开 `orders_shm`
- `src/api/position_monitor_api.cpp` 只读打开 `positions_shm`
- `src/broker_api` 定义券商适配 ABI 的稳定边界

也就是说：

- `src/api` 面向策略和监控消费者
- `src/broker_api` 面向网关和外部券商适配器开发者

## 4. 推荐阅读顺序

### 初次熟悉仓库

1. 本文档
2. [core 模块设计](src_core_module.md)
3. [order 模块设计](src_order_module.md)
4. [execution 模块设计](src_execution_module.md)
5. [strategy 模块设计](src_strategy_module.md)
6. [market_data 模块设计](src_market_data_module.md)
7. [portfolio 模块设计](src_portfolio_module.md)
8. [risk 模块设计](src_risk_module.md)
9. [shm 模块设计](src_shm_module.md)
10. [api 模块设计](src_api_module.md)

### 排查订单生命周期问题

1. [order 模块设计](src_order_module.md)
2. [core 模块设计](src_core_module.md)
3. [execution 模块设计](src_execution_module.md)
4. [strategy 模块设计](src_strategy_module.md)
5. [market_data 模块设计](src_market_data_module.md)
6. [risk 模块设计](src_risk_module.md)
7. [portfolio 模块设计](src_portfolio_module.md)
8. [订单处理流程图](order_flowchart.md)

### 排查共享内存、监控或重启恢复问题

1. [shm 模块设计](src_shm_module.md)
2. [order 模块设计](src_order_module.md)
3. [api 模块设计](src_api_module.md)

## 5. 专题文档导航

这组模块文档只负责建立稳定入口；更细节的协议、流程图和设计方案仍以下列专题文档为准：

- [订单处理流程图](order_flowchart.md)
- [Broker API Contract](broker_api_contract.md)
- [订单监控 SDK 使用文档](order_monitor_sdk.md)
- [C++ 函数签名设计规范](cpp_function_api_guideline.md)
- [position_loader 设计说明](plans/position_loader.md)
- [错误日志设计说明](plans/error_logging_design.md)
- [SHM manager 方案记录](plans/implemented/shm_manager_plan.md)
- [拆单父子单跟踪方案](plans/order_book_split_tracking_plan_zh.md)

## 6. 维护原则

- 以一级模块为主语描述源码，不把模块文档写成逐文件翻译。
- 只记录当前实现已经存在的行为；规划中的能力应明确写为“专题方案”或“未来扩展”。
- 当 `src` 模块职责变化时，优先更新对应模块文档；当协议细节变化时，再同步更新专题文档。
- `src/main.cpp` 和 `src/CMakeLists.txt` 只在总览中说明，不再单独拆文档。
