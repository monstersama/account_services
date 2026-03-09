# `src/risk` 模块设计

## 1. 模块定位

`src/risk` 负责把配置转换成一组可执行风控规则，并在订单进入路由前输出“通过/拒绝 + 原因”。它不直接冻结资源，也不直接推进订单状态；这些工作由 `EventLoop` 负责消费风控结果后完成。

源码范围：

- [`src/risk/risk_checker.hpp`](../src/risk/risk_checker.hpp)
- [`src/risk/risk_checker.cpp`](../src/risk/risk_checker.cpp)
- [`src/risk/risk_manager.hpp`](../src/risk/risk_manager.hpp)
- [`src/risk/risk_manager.cpp`](../src/risk/risk_manager.cpp)

## 2. 核心职责

- 定义风控规则抽象接口 `risk_rule`。
- 根据 `RiskConfig` 装配默认规则链。
- 对新单执行短路式规则检查。
- 维护风控统计和拒绝原因分类。
- 提供价格上下限、规则开关和规则动态替换能力。

## 3. 关键类型与角色

### `risk_check_result`

风控返回值，包含：

- `code`
- `message`

辅助函数：

- `passed()`
- `pass()`
- `reject(...)`

### `risk_rule`

所有规则的基类，核心接口：

- `name()`
- `check(const OrderRequest&, const PositionManager&)`
- `enabled()`
- `set_enabled(...)`

### 具体规则

- `fund_check_rule`
- `position_check_rule`
- `max_order_value_rule`
- `max_order_volume_rule`
- `price_limit_rule`
- `duplicate_order_rule`
- `rate_limit_rule`

### `RiskConfig`

驱动规则装配的配置快照，包括：

- 单笔金额上限
- 单笔数量上限
- 每秒订单数上限
- 价格限制开关
- 重复单检测开关
- 资金检查开关
- 持仓检查开关
- 重复单时间窗口

### `RiskManager`

核心管理器，负责：

- `check_order()`
- `check_orders()`
- `add_rule() / remove_rule() / enable_rule() / get_rule()`
- `update_price_limits() / clear_price_limits()`
- `update_config()`
- `stats() / reset_stats()`

## 4. 规则链装配方式

`RiskManager::initialize_default_rules()` 按配置生成默认规则链，顺序大致是：

1. 资金检查
2. 持仓检查
3. 单笔金额限制
4. 单笔数量限制
5. 涨跌停价格限制
6. 重复单检查
7. 每秒下单速率限制

这个顺序很重要，因为 `check_order()` 采用短路逻辑：

- 一旦某条规则拒绝，后续规则不再执行
- 最终只保留第一条命中的拒绝原因

## 5. 各规则当前语义

### 5.1 `fund_check_rule`

适用范围：

- 仅新单
- 仅买单

检查逻辑：

- 读取 `PositionManager::get_available_fund()`
- 所需金额 = `volume_entrust * dprice_entrust + dfee_estimate`
- 如果 `dfee_estimate == 0` 且委托金额大于 0，当前实现会兜底按 `1` 分手续费估算

### 5.2 `position_check_rule`

适用范围：

- 仅新单
- 仅卖单

检查逻辑：

- 使用 `PositionManager::get_sellable_volume(internal_security_id)`

### 5.3 `max_order_value_rule`

适用范围：

- 仅新单
- `max_value_ > 0`

检查逻辑：

- 比较 `volume_entrust * dprice_entrust` 与上限

### 5.4 `max_order_volume_rule`

适用范围：

- 仅新单
- `max_volume_ > 0`

检查逻辑：

- 比较 `volume_entrust` 与上限

### 5.5 `price_limit_rule`

适用范围：

- 仅新单
- 只有给定证券已配置价格上下限时才生效

价格上下限由以下接口注入：

- `RiskManager::update_price_limits(...)`

### 5.6 `duplicate_order_rule`

适用范围：

- 仅新单

当前实现特点：

- 指纹由 `internal_order_id` 构造
- 并不是按“证券 + 方向 + 价格 + 数量”的业务语义去识别重复单
- 更接近“同内部订单 ID 的重复进入保护”

### 5.7 `rate_limit_rule`

适用范围：

- 仅新单
- `max_orders_per_second_ > 0`

当前实现特点：

- 以单调时钟的 1 秒窗口计数
- 超限时返回 `RiskResult::RejectUnknown`
- `RiskState::update_stats()` 会把这类未显式识别拒绝归入 `rejected_rate_limit`

## 6. 运行期交互

1. `AccountService` 使用 `PositionManager` 和 `RiskConfig` 构造 `RiskManager`。
2. `EventLoop::handle_order_request()` 在新单进入路由前调用 `check_order()`。
3. 规则链逐条执行，直到：
   - 所有规则通过
   - 或遇到第一条拒绝规则
4. `EventLoop` 根据结果推进订单状态：
   - 通过 -> `RiskControllerAccepted`
   - 拒绝 -> `RiskControllerRejected`

模块边界上需要明确：

- `src/risk` 只给出结论
- 订单状态更新、订单镜像回写、资金冻结、下游路由都发生在 `src/core`

## 7. 统计与可观测性

`RiskState` 会累积：

- 总检查数
- 通过数
- 拒绝总数
- 各类拒绝原因计数
- 最近检查时间

统计更新逻辑位于 `RiskManager::update_stats()`。如果新增规则或新增 `RiskResult`，要同时检查：

- 拒绝码本身
- `update_stats()` 的归类逻辑
- 上层日志与监控是否需要新增指标维度

## 8. 依赖与边界

### 依赖其他模块

- `src/portfolio`
  - 提供可用资金和可卖持仓
- `src/common`
  - 时间、类型、订单枚举
- `src/core`
  - 负责真正调用和落地风控结果

### 不负责的内容

- 不持有 `orders_shm` 或其他共享内存对象。
- 不冻结/释放资金或持仓。
- 不维护订单状态机。

## 9. 相关专题文档

- [订单处理流程图](order_flowchart.md)
- [订单错误流](order_error_flow.mmd)
- [错误日志设计说明](plans/error_logging_design.md)

## 10. 维护提示

- 新增规则时，除了实现 `risk_rule` 子类，还要明确它在默认规则链中的插入顺序。
- 若调整重复单定义，必须同步修正文档中“当前语义”部分，避免把“同 ID 去重”和“业务重复单识别”混为一谈。
- 若把速率限制映射到新的 `RiskResult`，应同步更新 `RiskState::update_stats()` 的分类逻辑。
