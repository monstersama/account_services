# 旧/重复成交回报触发 OrderNotFound 噪音日志计划

## 问题现象
- 运行日志出现同一时间窗口内多条：
  - `update_status order not found`
  - `update_trade order not found`
  - `archive_order order not found`
- 典型位置：
  - `src/order/order_book.cpp:179`
  - `src/order/order_book.cpp:216`
  - `src/order/order_book.cpp:278`

## 已确认触发路径
- `event_loop::handle_trade_response()` 会按顺序执行：
  1. `order_book_.update_status(...)`
  2. 若 `volume_traded > 0`，`order_book_.update_trade(...)`
  3. 若终态，`order_book_.archive_order(...)`
- 当 `internal_order_id` 已不在 `order_book` 时，上述调用都会命中 not found 分支并分别打日志。

## 当前判断
- 高概率是“旧回报/重复回报”被再次消费：
  - 默认 SHM 为 `OpenOrCreate`，复用已有对象时队列内容可能残留。
  - 订单已归档后再收到同一 `internal_order_id` 回报，会触发该组日志。

## 本次已落地改动
- 在 `src/core/event_loop.cpp` 的 `handle_trade_response()` 增加早期存在性检查：
  - 若 `order_book` 中不存在该 `internal_order_id`，直接忽略回报并返回。
  - 目的：避免同一条旧回报在 `order_book` 连续触发三条 not found 错误。

## 后续可选增强（TODO）
1. 启动期清队策略：
   - 为 `trades_shm/downstream_shm` 增加“启动即 `init()` 清队”的可配置开关，降低跨进程重启残留影响。
2. 观测增强：
   - 对“忽略的未知回报”增加低频统计（计数器）而非每条错误日志。
3. 协议增强：
   - 若后续引入 run_id / session_id，可在回报端做会话隔离，彻底避免旧回报混入。

## 验收标准
- 压测/重启场景下不再出现成组三连的 `OrderNotFound` 噪音日志。
- 正常回报路径不受影响：`update_status/update_trade/archive_order` 仍按预期执行。
- 相关测试通过且无新增 Fatal/停服回归。

