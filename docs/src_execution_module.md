# `src/execution` 模块设计

## 1. 模块定位

`src/execution` 负责统一执行会话运行时。当前它托管：

- `FixedSize`
- `Iceberg`
- `TWAP`

`VWAP` 已接入统一入口，但当前明确返回 unsupported。

它位于：

- `src/core/EventLoop`
- `src/order/order_router`
- `src/order/order_book`
- `src/strategy`
- `src/market_data`

之间，专门承接“父单持续推进、子单动态释放、父单镜像回写”这类一次性拆单模型装不下的逻辑。

源码范围：

- [`src/execution/execution_engine.hpp`](../src/execution/execution_engine.hpp)
- [`src/execution/execution_engine.cpp`](../src/execution/execution_engine.cpp)
- [`src/execution/execution_config.hpp`](../src/execution/execution_config.hpp)

## 2. 核心职责

- 判断一笔新单是否应该进入统一执行会话框架。
- 为受管父单创建具体会话类型。
- 维护统一 child ledger，而不是只跟踪活动子单 ID。
- 基于权威 `cancelled_volume` 和确认成交量派生父单预算。
- 在每轮 `tick()` 中推进活跃会话。
- 把父单执行镜像写回 `OrderBook` / `orders_shm`。

## 3. 关键类型与角色

### `split_config`

`split_config` 现在是执行算法默认参数的配置载体，不再代表旧的一次性 splitter 运行时。

当前会话使用的字段：

- `strategy`
- `max_child_volume`
- `min_child_volume`
- `max_child_count`
- `interval_ms`

### `ExecutionEngine`

`ExecutionEngine` 是模块对外唯一公开的服务对象。

关键函数：

- `should_manage(request)`
- `has_active_strategy()`
- `start_session(parent_index, parent_request, strategy_id, start_time_ns)`
- `tick(now_ns_value)`
- `on_trade_response(response)`

当前语义：

- 只要解析后的被动算法属于 `FixedSize / Iceberg / TWAP / VWAP`，`should_manage()` 就会返回 `true`
- `start_session()` 会把 `VWAP` 明确拒绝
- `tick()` 是真正的运行入口，由 `EventLoop` 在每轮循环中调用
- `on_trade_response()` 通过子单回报更新 ledger、释放预算并刷新父单镜像

### `ExecutionSession`

`ExecutionSession` 是统一会话基类，对外不暴露。

它维护：

- 父单请求快照
- 执行算法枚举
- 子单统一账本 `child_execution_ledger`
- `cancel_requested / failed / terminal`
- 父单镜像派生函数
- 市场行情读取与子单定价逻辑

### `FixedSizeSession / IcebergSession / TwapSession`

当前三种会话的推进方式分别是：

- `FixedSizeSession`
  - 无在途子单时释放下一笔 clip
- `IcebergSession`
  - 首版复用顺序 clip 推进器，但保留独立会话类型和执行态
- `TwapSession`
  - 按固定时间片节奏推进，每片先主动、后被动

## 4. 定价与预算语义

### 4.1 父单预算派生

父单预算统一由 child ledger 派生：

- `remaining_target = target_volume - confirmed_traded_volume`
- `working_volume = sum(unfinalized child unresolved qty)`
- `schedulable_volume = max(0, remaining_target - working_volume)`

这意味着：

- 预算释放不再依赖旧的子单 `volume_remain` 聚合
- 收到权威 `cancelled_volume` 后可以立即释放预算

### 4.2 子单价格如何生成

当前 managed execution 子单会先尝试读取最新行情，再生成市场派生价格。

规则固定为：

- 买单：优先取卖一，卖一无效时回退到买一
- 卖单：优先取买一，买一无效时回退到卖一
- 若盘口无效：
  - `allow_order_price_fallback=false` 时，不发子单
  - `allow_order_price_fallback=true` 时，回退到父单 `dprice_entrust`

也就是说：

- 只要有有效盘口，managed child 直接使用盘口派生价
- 只有在行情缺失或顶档无效且显式启用 fallback 时，才会复用父单委托价

### 4.3 主动策略与价格

主动策略当前只负责回答：

- 是否在当前预算窗口内发单
- 要消耗多少预算

实际子单价格仍由执行引擎统一按行情或 fallback 规则生成，不允许主动策略绕开定价链路。

## 5. 运行流程

### 5.1 何时进入会话

1. `EventLoop::handle_order_request()` 完成基础校验和风控。
2. 若 `ExecutionEngine::should_manage(request)` 为真：
   - 父单状态先推进到 `TraderPending`
   - 直接调用 `ExecutionEngine::start_session(...)`
   - 不再走普通 `order_router::route_order()`
3. 后续由 `ExecutionEngine` 在多个 `tick()` 中逐步生成子单。

### 5.2 会话创建条件

当前受管父单除了算法支持外，还要求：

- 对应算法的配置参数有效
- 满足以下其一：
  - `MarketDataService` 已初始化且 `ready`
  - `allow_order_price_fallback=true`

如果行情模块未就绪且 fallback 也未开启：

- managed execution 订单会被拒绝
- 不会回退到普通单直发路径

### 5.3 顺序型算法推进

`FixedSize / Iceberg` 当前的推进顺序是：

1. 刷新父单状态和取消请求
2. 若仍有在途子单，则继续等待
3. 若可释放预算，先尝试主动策略
4. 主动策略无动作时，尝试按行情派生价格发一笔子单
5. 若行情不可用但已启用 fallback，则按父单委托价发子单

### 5.4 TWAP 推进

对每个活跃 `TwapSession`，`tick()` 的顺序是：

1. 检查父单是否仍有效
2. 刷新当前片是否已可以推进到下一片
3. 若当前片仍存在：
   - 读取最新行情
   - 先尝试主动策略
   - 若到片末仍无主动子单，再按行情或 fallback 规则发被动子单
4. 本片子单全部终态后推进到下一个时间片

### 5.5 回报与预算释放

`ExecutionEngine::on_trade_response()` 的处理顺序：

1. 找到子单所属父单会话
2. 更新 child ledger 中的：
   - `confirmed_traded_volume`
   - `confirmed_traded_value`
   - `confirmed_fee`
   - `final_cancelled_volume`
3. 若子单 finished：
   - 使用权威 `cancelled_volume` finalize 子单
   - 立即释放父单预算
4. 刷新父单镜像

若在子单已 finalized 后又收到会破坏 `entrust = traded + cancelled` 的晚到回报：

- 记录警告
- 不重复释放预算

## 6. 与其它模块的关系

### 与 `src/order`

- `ExecutionEngine` 不直接写下游队列
- 子单通过 `order_router::submit_internal_order(...)` 统一登记和下发
- 父单镜像通过 `OrderBook::sync_managed_parent_view(...)` 回写

### 与 `src/strategy`

- 主动策略只决定“是否在当前预算窗口内发单”
- 不直接操作订单簿或下游队列
- 不直接控制 managed child 的最终发单价

### 与 `src/market_data`

- 行情模块同时提供盘口快照和 prediction
- 执行引擎优先读取盘口生成 managed child 价格
- 主动策略读取 prediction 做时机判断
- 若配置允许，执行引擎可在行情不可用时回退到父单价

## 7. 维护提示

- 若新增执行算法，优先新增会话类型和工厂分支，不要重新引入一次性 splitter。
- 若修改 managed child 定价规则，优先补充 `test_execution_engine.cpp`。
- 若修改父单镜像字段语义，需同步更新 monitor 相关文档和测试。
