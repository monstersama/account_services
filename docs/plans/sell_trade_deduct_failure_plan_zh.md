# 卖出成交扣仓失败问题保留计划

## 问题现象
- 运行时出现日志：
  - `[ERROR][event_loop] ... code=PositionUpdateFailed ... msg=failed to deduct position from trade response`
- 触发点位于 `src/core/event_loop.cpp:367` 到 `src/core/event_loop.cpp:371`。

## 已确认代码路径
- 卖出成交回报处理：
  - `event_loop::handle_trade_response()` 在 `response.trade_side == Sell` 时调用 `positions_.deduct_position(...)`。
- 失败条件：
  - `src/portfolio/position_manager.cpp:483` 的 `position_manager::deduct_position(...)` 返回 `false`。
  - 典型失败分支：
    - 持仓行不存在（`get_position_mut` 失败）。
    - `volume_sell + volume_available_t0 < response.volume_traded`，可扣减数量不足。
- 错误级别：
  - `PositionUpdateFailed` 在 `portfolio` 域被分类为 `Fatal`，会触发停服路径（见 `src/common/error.cpp`）。

## 当前判断（待后续实证）
- 高概率原因：卖单在风控阶段仅检查可卖量，没有在下单/路由阶段做仓位预冻结，导致并发或连续卖单在成交回报阶段出现超卖扣减失败。
- 代码依据：
  - 风控检查读取 `get_sellable_volume()`，但未调用 `freeze_position()`。
  - `freeze_position()/unfreeze_position()` 当前仅有实现，未接入下单主路径。

## 后续修复方向（TODO）
1. 在卖出新单通过风控后接入预冻结：
   - 预期在下游排队前执行 `freeze_position(security_id, volume_entrust, order_id)`。
2. 在成交/撤单/拒单回报中补齐解冻与扣减规则：
   - 成交：优先扣减 `volume_sell`，不足时按现有兼容逻辑回退。
   - 终态未全成：释放剩余冻结量，避免长期占用可卖仓位。
3. 为 `event_loop` 增加订单级上下文字段（如已冻结数量）并做幂等保护，防止重复回报造成二次解冻/扣减。
4. 增加回归测试：
   - 两笔连续卖单超过可卖量时，第二笔应在风控或冻结环节被拒，不应在成交阶段触发 Fatal。
   - 部分成交 + 撤单场景，验证冻结仓位正确回收。
   - 分单（parent/child）场景下冻结/解冻总量守恒。

## 验收标准
- 不再出现 `failed to deduct position from trade response` 的 Fatal 日志（在覆盖用例内）。
- 持仓字段 `volume_available_t0 / volume_sell / volume_sell_traded` 满足守恒关系。
- `test` 中新增并通过对应回归用例，且不引入现有 E2E 回归。

