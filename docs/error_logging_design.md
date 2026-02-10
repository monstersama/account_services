# 错误处理与日志设计

## 目标

- 统一错误模型：全模块共享 `error_status`
- 分级终止策略：`Recoverable` / `Critical` / `Fatal`
- 高性能日志：异步队列 + 后台批量落盘

## 错误分级

- `Recoverable`：记录后继续处理当前服务
- `Critical`：触发停服并要求进程退出
- `Fatal`：最高优先级停服退出（用于一致性破坏）

通过 `classify(error_domain, error_code)` 返回 `error_policy`，并由 `record_error()` 自动触发 shutdown 闩锁。

## 错误码分段规则

`error_code` 采用“按模块分段”的显式编号，`Ok` 固定为 `0`：

- `0`：`Ok`
- `[1000, 1099]`：通用/核心基础错误
- `[2000, 2099]`：配置（config）错误
- `[3000, 3099]`：共享内存（shm）错误
- `[4000, 4099]`：订单链路（order）错误
- `[6000, 6099]`：资金/持仓（portfolio）错误
- `[7000, 7099]`：日志设施（logger）错误

设计原则：

- 编号语义与严重级别解耦，严重级别由 `classify()` 决定。
- 同一模块在当前分段内优先“顺序增量”分配，便于检索和维护。
- 预留空位用于后续插入，不要求连续。
- 保持历史编号稳定，禁止重用旧编号。

新增错误码流程：

1. 在对应模块分段内选择未占用编号。
2. 在 `src/common/error.hpp` 增加枚举项（含注释）。
3. 在 `src/common/error.cpp` 补充 `to_string(error_code)`。
4. 在 `classify()` 明确策略（`Recoverable` / `Critical` / `Fatal`）。
5. 补充/更新相关测试（计数、策略、链路行为）。

## 服务终止流程

1. 记录 `error_status`
2. 记录日志（`ERROR/FATAL` 兜底到 stderr）
3. 设置 shutdown reason
4. `event_loop` 检测并停止
5. `account_service` 清理并 `flush_logger(200)`
6. `main` 非 0 退出

## 日志实现

- 架构：MPSC 环形队列 + 单消费者
- 队列容量：`log.async_queue_size`（向上取 2 的幂）
- 批量消费：最多 256 条/批
- 刷盘周期：空闲时 10ms
- 满队列策略：
  - `DEBUG/INFO`：丢弃并计数
  - `WARN`：走异步队列，失败计数
  - `ERROR/FATAL`：失败时同步 stderr 兜底

## 对外 API 兼容

`acct_order` C API 保持原返回码，不在库内直接退出进程；内部仅做错误记录与日志埋点。
