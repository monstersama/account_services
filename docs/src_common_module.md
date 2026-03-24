# `src/common` 模块设计

## 1. 模块定位

`src/common` 提供全仓库复用的基础设施，包括错误模型、日志、时间工具、定长字符串、证券标识和轻量并发原语。它本身不承载业务流程，但几乎被所有业务模块依赖。

源码范围：

- [`src/common/constants.hpp`](../src/common/constants.hpp)
- [`src/common/types.hpp`](../src/common/types.hpp)
- [`src/common/fixed_string.hpp`](../src/common/fixed_string.hpp)
- [`src/common/error.hpp`](../src/common/error.hpp)
- [`src/common/error.cpp`](../src/common/error.cpp)
- [`src/common/log.hpp`](../src/common/log.hpp)
- [`src/common/log.cpp`](../src/common/log.cpp)
- [`src/common/spinlock.hpp`](../src/common/spinlock.hpp)
- [`src/common/spinlock.cpp`](../src/common/spinlock.cpp)
- [`src/common/security_identity.hpp`](../src/common/security_identity.hpp)
- [`src/common/time_utils.hpp`](../src/common/time_utils.hpp)
- [`src/common/time_utils.cpp`](../src/common/time_utils.cpp)

## 2. 核心职责

- 统一基础类型、枚举和时间函数。
- 定义跨模块一致的错误对象、错误域、错误码和停服策略。
- 提供异步日志设施与全局日志入口。
- 提供共享内存友好的定长字符串 `FixedString`。
- 提供 `SpinLock`、`LockGuard`、`UniqueLock` 等轻量并发原语。
- 统一内部证券 ID 的构造规则。

## 3. 关键子系统

### 3.1 类型与常量

`types.hpp` 定义了仓库级基础别名，例如：

- `InternalOrderId`
- `InternalSecurityId`
- `Volume`
- `DPrice`
- `DValue`
- `TimestampNs`

也定义了多个跨模块共享枚举：

- `RiskResult`
- `AccountState`
- `AccountType`
- `SplitStrategy`
- `PositionChange`

`constants.hpp` 则集中定义：

- 队列容量
- 订单池容量
- 字段长度
- 共享内存默认名等常量

### 3.2 `FixedString`

`FixedString<N>` 的设计目标是：

- 避免动态分配
- 允许直接嵌入 SHM 结构或稳定 ABI 结构
- 提供足够轻量的字符串视图操作

常见使用场景：

- `SecurityId`
- `InternalSecurityId`
- `BrokerOrderId`
- `ErrorStatus` 中的 `module/file/message`

### 3.3 错误模型

`error.hpp` / `error.cpp` 定义了：

- `ErrorDomain`
- `ErrorCode`
- `ErrorSeverity`
- `ErrorPolicy`
- `ErrorStatus`
- `ErrorRegistry`

关键函数：

- `make_error_status(...)`
- `classify(...)`
- `record_error(...)`
- `last_error()`
- `latest_error()`
- `request_shutdown(...)`
- `shutdown_reason()`
- `should_stop_service()`

错误处理链路：

1. 业务模块调用 `ACCT_MAKE_ERROR(...)` 生成 `ErrorStatus`
2. 调用 `record_error(status)`
3. `record_error()` 会：
   - 更新线程本地最近错误
   - 更新全局最近错误
   - 记入 `ErrorRegistry`
   - 按 `classify(domain, code)` 决定是否触发停机闩锁

需要注意：

- `api` 域错误保留严重级别，但不会替外部调用方进程决定 stop/exit。
- `portfolio` 和 `shm` 中的一些关键错误会被提升为 `Critical` 或 `Fatal`。

### 3.4 日志系统

`log.hpp` / `log.cpp` 定义了：

- `LogLevel`
- `LogRecord`
- `AsyncLogger`
- 全局函数 `init_logger()`、`shutdown_logger()`、`flush_logger()`、`log_message()`

当前实现要点：

- 默认使用后台线程异步落盘到 `log_dir/account_<account_id>.log`
- 内部维护一个按容量归一化的环形日志队列
- 如果异步关闭，则走 `SpinLock` 保护的同步写文件路径
- 若队列已满：
  - 增加 `dropped_count`
  - `error/fatal` 日志同步回退到 `stderr`

常用日志宏：

- `ACCT_LOG_DEBUG`
- `ACCT_LOG_INFO`
- `ACCT_LOG_WARN`
- `ACCT_LOG_ERROR`
- `ACCT_LOG_FATAL`
- `ACCT_LOG_ERROR_STATUS`

### 3.5 轻量并发原语

`spinlock.hpp` 提供：

- `SpinLock`
- `LockGuard<Lock>`
- `UniqueLock<Lock>`

用途：

- `OrderBook`
- `ErrorRegistry`
- 同步写日志路径

### 3.6 证券标识与时间工具

`security_identity.hpp` 统一把内部证券键构造成：

- `<MIC>_<security_id>`

例如：

- `XSHE_000001`
- `XSHG_600000`

`time_utils.hpp` 和 `types.hpp` 提供两套常用时间语义：

- `CLOCK_REALTIME`
  - 用于事件记录时间戳
- `CLOCK_MONOTONIC`
  - 用于延迟统计和窗口判断

## 4. 依赖与边界

### 模块边界

`src/common` 基本上是底层模块，不依赖业务流程模块。

但有两个需要注意的耦合点：

- `log.cpp` 依赖 `core/config_manager.hpp` 中的 `LogConfig`
- `security_identity.hpp` 依赖 `order/order_request.hpp` 中的 `Market` 枚举

这意味着它并不是一个完全独立的“纯底层库”，文档和重构时应意识到这两个边界穿透。

### 被哪些模块使用

- `src/core`
- `src/order`
- `src/portfolio`
- `src/risk`
- `src/shm`
- `src/api`

## 5. 相关专题文档

- [错误日志设计说明](plans/error_logging_design.md)
- [C++ 命名基线](cpp_naming_baseline.md)
- [C++ 函数签名设计规范](cpp_function_api_guideline.md)

## 6. 维护提示

- 新增跨模块错误码时，除了补充枚举，还要同步检查：
  - `to_string(ErrorCode)`
  - `classify(...)`
  - 日志和监控是否需要新的解释文本
- 修改 `FixedString`、类型别名或公共枚举时，必须评估：
  - SHM 布局兼容性
  - 对外 C ABI 结构兼容性
  - 监控 SDK 是否依赖这些字段宽度
- 若将 `common` 继续下沉为更纯的基础库，应优先处理 `LogConfig` 和 `Market` 这两个跨层依赖点。
