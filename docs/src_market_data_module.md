# `src/market_data` 模块设计

## 1. 模块定位

`src/market_data` 负责把 vendored `snapshot_reader` 适配成账户服务内部稳定可用的行情读接口。它不直接决定订单是否进入执行会话，但会为执行引擎和主动策略提供统一的盘口与 prediction 读视图。

源码范围：

- [`src/market_data/market_data_service.hpp`](../src/market_data/market_data_service.hpp)
- [`src/market_data/market_data_service.cpp`](../src/market_data/market_data_service.cpp)

## 2. 核心职责

- 统一封装 `signal_engine::snapshot_reader::SnapshotReader` 的生命周期。
- 根据 `MarketDataConfig` 决定是否启用行情模块。
- 把 `internal_security_id` 规范化为 `snapshot_reader` 使用的 canonical symbol。
- 读取单标的最新 L10 行情与 prediction 字段。
- 把第三方的 `prediction_state` 收敛成项目内 `PredictionState`。

## 3. 关键类型与角色

### `PredictionState`

项目内统一预测值状态：

- `None`
- `Fresh`
- `Carried`

### `PredictionView`

`PredictionView` 是对 prediction 相关字段的稳定视图封装，包含：

- `signal`
- `flags`
- `state`
- `publish_seq_no`
- `publish_mono_ns`

当前系统中的主动策略只把 `Fresh` 视为可触发主动决策的 prediction。

### `MarketDataView`

`MarketDataView` 是单标的统一读视图，包含：

- `symbol`
- `seq`
- `snapshot`
- `prediction`

其中：

- `snapshot` 直接复用 `snapshot_shm::LobSnapshot`
- `prediction` 使用本模块定义的 `PredictionView`

### `MarketDataService`

`MarketDataService` 是本模块唯一的服务对象。

关键函数：

- `initialize()`
- `close()`
- `is_enabled()`
- `is_ready()`
- `read(internal_security_id, out_view)`

## 4. 当前使用方式

### 4.1 主动策略

主动策略通过 `MarketDataView::prediction` 读取：

- 是否有 `Fresh` prediction
- `signal` 是否命中方向阈值

### 4.2 执行引擎定价

执行引擎通过 `MarketDataView::snapshot` 读取盘口生成 managed child 价格。

当前定价规则：

- 买单：优先取卖一
- 卖单：优先取买一
- 然后再用父单 `dprice_entrust` 做价格边界约束
- 若没有有效盘口，不发子单

所以当前 `market_data` 不只是“主动策略的 prediction 输入”，也是受管执行子单的实际定价输入。

## 5. 运行流程

### 5.1 初始化

1. `AccountService::init_market_data()` 创建 `MarketDataService`
2. 调用 `initialize()`
3. 若 `market_data.enabled=false`，模块进入 no-op 模式
4. 若 `market_data.enabled=true`
   - 打开 `snapshot_shm_name`
   - 校验 reader 已成功打开
   - 标记服务 `ready`

### 5.2 单标的读取

1. 上层调用 `MarketDataService::read(internal_security_id, out_view)`
2. 模块先对 `internal_security_id` 执行规范化
3. 生成 snapshot reader 所需的 symbol
4. 通过 `SnapshotReader::read(symbol, result)` 读取稳定快照
5. 把 payload 投影为：
   - `MarketDataView::snapshot`
   - `PredictionView`
6. 返回给执行引擎或主动策略

## 6. 依赖与边界

### 依赖其他模块

- `src/common`
  - 证券键规范化工具
- `src/core`
  - 由 `AccountService` 创建并管理生命周期
- `third_party/snapshot_reader`
  - 实际的共享内存读端实现与协议定义

### 被哪些模块依赖

- `src/strategy`
  - 主动策略读取 prediction
- `src/execution`
  - 执行引擎读取盘口并生成 managed child 价格

### 不负责的内容

- 不决定父单是否接管
- 不管理时间片预算
- 不生成子单
- 不缓存全市场快照

## 7. 相关配置

当前由 `ConfigManager` 暴露的相关配置块：

- `market_data.enabled`
- `market_data.snapshot_shm_name`

校验约束：

- 仅当 `enabled=true` 时，`snapshot_shm_name` 必须非空
- 若启用了 managed execution 默认策略（`split.strategy != none`），也要求 `market_data.enabled=true`

## 8. 维护提示

- 若 `snapshot_reader` 协议字段变化，优先更新本模块的投影逻辑，而不是把第三方类型直接扩散到上层。
- 若需要修改 managed child 定价规则，优先在执行引擎中调整使用方式，并同步更新相关测试。
- 若需要新增 prediction 语义判断，优先在 `PredictionView` 或 `MarketDataService` 内统一收敛。
