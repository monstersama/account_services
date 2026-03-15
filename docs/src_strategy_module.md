# `src/strategy` 模块设计

## 1. 模块定位

`src/strategy` 负责“主动策略覆盖层”的抽象与内建策略注册。它不直接管理父单生命周期，也不直接投递子单；它只负责在执行引擎给定的预算窗口中做一次主动决策。

当前定位：

- 主动策略是服务级可选项
- 它可被 `FixedSize / Iceberg / TWAP` 这类受管执行会话复用
- 它的输出是“是否消耗当前预算，以及消耗多少”

源码范围：

- [`src/strategy/active_strategy.hpp`](../src/strategy/active_strategy.hpp)
- [`src/strategy/active_strategy.cpp`](../src/strategy/active_strategy.cpp)

## 2. 核心职责

- 定义主动策略的统一抽象接口。
- 定义主动策略评估时可见的最小上下文。
- 定义主动策略在单次预算窗口内的统一输出结构。
- 提供内建注册表，根据服务配置创建具体策略实现。

## 3. 关键类型与角色

### `ActiveDecision`

`ActiveDecision` 是主动策略在单次预算窗口内的统一输出：

- `should_submit`
- `volume`
- `price`

当前实现语义：

- `should_submit=false`：本轮不动作，由执行引擎继续等待或回落到被动执行
- `should_submit=true`：表示策略希望立即消耗一部分预算

需要注意：

- 当前执行引擎会统一按最新盘口生成子单价格
- `ActiveDecision::price` 暂未作为最终子单价的权威来源

### `ActiveStrategyContext`

主动策略评估上下文包含：

- `parent_request`
- `market_data`
- `budget_volume`

其中：

- `parent_request` 描述当前父单
- `market_data` 提供最新行情与 prediction 视图
- `budget_volume` 表示当前窗口允许消耗的预算

### `ActiveStrategy`

主动策略统一抽象：

- `name()`
- `evaluate(context)`

当前约束：

- 每次 `evaluate()` 只针对一个预算窗口做一次判断
- 不直接操作订单簿或下游队列
- 不管理长期状态机，长期状态由 `ExecutionSession` 持有

### `StrategyRegistry`

`StrategyRegistry` 负责把服务级配置映射成具体内建策略对象。

当前实现：

- `enabled=false` 或 `name="none"`：返回空指针
- `name="prediction_signal"`：创建 `PredictionSignalStrategy`
- 未识别名称：返回空指针，由 `AccountService` 视为无效配置

### `PredictionSignalStrategy`

这是当前唯一的内建主动策略实现。

行为：

- 只消费 `Fresh` prediction
- 当父单方向与 `signal_threshold` 命中时，请求消耗当前 `budget_volume`
- 若信号未命中，返回 no-op

## 4. 运行流程

### 4.1 初始化

1. `AccountService::init_execution_engine()` 调用 `StrategyRegistry::create(...)`
2. 若配置关闭或名称为 `none`，执行引擎持有空策略指针
3. 若配置为已知策略名，则生成对应的内建策略对象
4. 执行引擎在每个受管执行会话中复用这同一个服务级策略对象

### 4.2 预算窗口内评估

1. `ExecutionEngine` 在某个活跃会话上推进当前窗口
2. 读取当前父单的 `MarketDataView`
3. 组装 `ActiveStrategyContext`
4. 调用 `ActiveStrategy::evaluate(...)`
5. 根据返回的 `ActiveDecision`
   - `should_submit=true`：执行引擎生成主动子单
   - `should_submit=false`：继续等待或回落到被动执行

## 5. 依赖与边界

### 依赖其他模块

- `src/market_data`
  - 通过 `MarketDataView` 获取盘口与 prediction
- `src/core`
  - 由 `AccountService` 根据配置创建

### 被哪些模块依赖

- `src/execution`
  - `ExecutionEngine` 在预算窗口内调用主动策略

### 不负责的内容

- 不持有父单生命周期
- 不直接操作 `OrderBook`
- 不直接写 `orders_shm`
- 不直接决定 managed child 的最终发单价格

## 6. 相关配置

当前 `ConfigManager` 暴露：

- `active_strategy.enabled`
- `active_strategy.name`
- `active_strategy.signal_threshold`

当前已实现的配置语义：

- `name="prediction_signal"` 对应首个内建策略
- 其他名称会导致 `AccountService` 初始化失败

## 7. 相关专题文档

- [execution 模块设计](src_execution_module.md)
- [market_data 模块设计](src_market_data_module.md)
- [core 模块设计](src_core_module.md)

## 8. 维护提示

- 若新增主动策略，优先通过 `StrategyRegistry` 扩展内建注册表，而不是在执行引擎里写分支。
- 若主动策略需要长期状态，不要把状态塞进 `ActiveStrategy` 接口本身；优先评估是否应由 `ExecutionSession` 持有。
- 若主动策略要使用新的盘口或 prediction 字段，优先扩展 `MarketDataView`，不要直接依赖 third-party payload 结构。
