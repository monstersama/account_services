# 账户服务项目组件实现计划

## 项目概述

这是一个 C++20 高性能低延迟量化交易账户服务进程，通过共享内存与策略进程和交易进程通信，负责订单风控检查、拆单、路由和成交回报处理。

## 当前状态

### 已完整实现的组件
| 组件 | 文件 | 状态 |
|------|------|------|
| 类型定义 | `src/common/types.hpp` | 完整 |
| 常量定义 | `src/common/constants.hpp` | 完整 |
| 定长字符串 | `src/common/fixed_string.hpp` | 完整 |
| 高性能自旋锁 | `src/common/spinlock.hpp`, `src/common/spinlock.cpp` | 完整 |
| 时间工具 | `src/common/time_utils.hpp`, `src/common/time_utils.cpp` | 完整 |
| 无锁队列 | `src/shm/spsc_queue.hpp` | 完整 |
| 共享内存布局 | `src/shm/shm_layout.hpp` | 完整 |
| 共享内存管理器 | `src/shm/shm_manager.hpp`, `src/shm/shm_manager.cpp` | 完整 |
| 订单请求结构 | `src/order/order_request.hpp` | 完整 |
| 持仓结构 | `src/portfolio/positions.h` | 完整 |
| 持仓管理器 | `src/portfolio/position_manager.hpp`, `src/portfolio/position_manager.cpp` | 完整 |
| 风控检查器 | `src/risk/risk_checker.hpp`, `src/risk/risk_checker.cpp` | 完整 |
| 风控管理器 | `src/risk/risk_manager.hpp`, `src/risk/risk_manager.cpp` | 完整 |
| 拆单器 | `src/order/order_splitter.hpp`, `src/order/order_splitter.cpp` | 完整 |
| 订单簿 | `src/order/order_book.hpp`, `src/order/order_book.cpp` | 完整（含父子追踪） |
| 订单路由器 | `src/order/order_router.hpp`, `src/order/order_router.cpp` | 完整（含拆单撤单扇出） |
| 事件循环 | `src/core/event_loop.hpp`, `src/core/event_loop.cpp` | 完整（可选 trades SHM 注入） |
| 配置管理器 | `src/core/config_manager.hpp`, `src/core/config_manager.cpp` | 完整（文件加载/命令行解析/导出/reload） |
| 账户信息管理器 | `src/portfolio/account_info.hpp`, `src/portfolio/account_info.cpp` | 完整（配置加载/权限校验/手续费计算） |
| 成交记录管理器 | `src/portfolio/trade_record.hpp`, `src/portfolio/trade_record.cpp` | 完整（索引查询/统计/导出） |
| 委托记录管理器 | `src/portfolio/entrust_record.hpp`, `src/portfolio/entrust_record.cpp` | 完整（订单快照更新/活跃查询/导出） |
| 账户服务主类 | `src/core/account_service.hpp`, `src/core/account_service.cpp` | 完整（初始化/运行/停止/清理） |
| 对外 C API | `src/api/order_api.cpp` | 完整 |

### 仍待实现的组件
| 组件 | 文件 | 当前状态 | 下一步 |
|------|------|----------|--------|
| （暂无） | - | - | - |

### 本轮已完成更新（扩展到配置/服务主类）
- 新增 `order_book` 父子追踪接口：`get_children()`、`try_get_parent()`。
- 新增父子映射维护：`parent_to_children_`、`child_to_parent_`、`split_parent_error_latched_`。
- 实现父单聚合收敛逻辑：子单状态/成交变更触发父单实时聚合。
- 实现 `order_router` 拆单登记、父单撤单扇出、下游发送失败“尽力继续 + 父单错误锁存”。
- 实现 `risk_checker` 与 `risk_manager`，补齐资金/持仓/价格/重复/频率规则与统计。
- 实现 `event_loop` 编排层：上游请求处理、风控接入、下游成交回报收口到 `order_book`。
- 实现 `config_manager`（文件加载、命令行覆盖、配置导出、reload）。
- 实现 `account_info` / `trade_record` / `entrust_record` 三个记录组件。
- 实现 `account_service` 主类（配置注入、SHM 初始化、组件装配、run/stop 生命周期）。
- 新增 `acct_portfolio`、`acct_core_service` 构建目标，并更新现有依赖关系。
- 新增测试：
  - `test/test_order_book_split_tracking.cpp`
  - `test/test_order_router_split_cancel.cpp`
  - `test/test_risk_manager.cpp`
  - `test/test_event_loop.cpp`
  - `test/test_config_manager.cpp`
  - `test/test_account_service.cpp`

---

## 组件实现顺序

### 阶段 1: 基础工具组件（第1-2周）

#### 1.1 自旋锁实现 (spinlock) ✅
**文件**: `src/common/spinlock.cpp`
**说明**: 已实现 TTAS + 指数退避 + 平台 pause/yield 优化。

#### 1.2 时间工具实现 (time_utils) ✅
**文件**: `src/common/time_utils.cpp`
**说明**: 已实现常用市场时间转换与当前时间获取工具。

---

### 阶段 2: 共享内存基础设施（第3-4周）

#### 2.1 共享内存管理器 (shm_manager) ✅
**文件**: `src/shm/shm_manager.cpp`
**说明**: 已实现 POSIX 共享内存创建、打开、验证、关闭与清理流程。

---

### 阶段 3: 核心交易组件（第5-7周）

#### 3.1 持仓管理器 (position_manager) ✅
**文件**: `src/portfolio/position_manager.cpp`
**说明**: 已实现初始化、资金操作、持仓操作与查询接口（含 FUND 行映射逻辑）。

#### 3.2 订单簿 (order_book) ✅
**文件**: `src/order/order_book.cpp`
**说明**: 已实现活跃订单管理、索引查询、状态与成交更新，以及拆单父子追踪。

**已实现关键功能**:
- `add_order()`, `find_order()`, `find_by_broker_id()`, `update_status()`, `update_trade()`, `archive_order()`,
  `next_order_id()`, `clear()`
- 拆单追踪接口：`get_children()`, `try_get_parent()`
- 父单聚合逻辑：子单更新触发父单成交/状态收敛
- 生命周期策略：`archive_order()` 不清映射，`clear()` 清映射

#### 3.3 风控检查器 (risk_checker) ✅
**文件**: `src/risk/risk_checker.cpp`
**说明**: 已实现基础风控规则与结果封装。

**已实现关键功能**:
- `risk_check_result::pass/reject/passed`
- 规则启停控制：`risk_rule::enabled()/set_enabled()`
- 资金检查、持仓检查、单笔金额/数量限制
- 涨跌停价格检查、重复订单检查、流速限制

#### 3.4 风控管理器 (risk_manager) ✅
**文件**: `src/risk/risk_manager.cpp`
**说明**: 已实现规则装配、检查调度、统计与配置更新。

**已实现关键功能**:
- `check_order()`, `check_orders()`
- 规则管理：`add_rule()`, `remove_rule()`, `enable_rule()`, `get_rule()`
- 配置与价格限制更新：`update_config()`, `update_price_limits()`, `clear_price_limits()`
- 统计：`risk_stats::reset()`, `stats()`, `reset_stats()`

---

### 阶段 4: 订单处理组件（第8-9周）

#### 4.1 拆单器 (order_splitter) ✅
**文件**: `src/order/order_splitter.cpp`
**说明**: 已实现拆单执行流程与 ID 生成器接入。

**已实现关键功能**:
- `split()`: 按策略分发拆单
- `should_split()`: 拆单判定
- `split_fixed_size()`, `split_iceberg()`, `split_twap()`
- `set_order_id_generator()`: 子单 ID 生成器

#### 4.2 订单路由器 (order_router) ✅
**文件**: `src/order/order_router.cpp`
**说明**: 已实现路由、拆单入簿下发、父单撤单扇出与失败处理策略。

**已实现关键功能**:
- `route_order()`, `route_orders()`, `route_cancel()`
- `send_to_downstream()`, `handle_split_order()`
- 拆单子单先入 `order_book` 再下发，保证可追踪链路
- 父单撤单自动扇出活跃子单撤单
- 下游发送失败采用“尽力继续 + 父单 `TraderError` 锁存”

> 详细方案见：`docs/plans/order_book_split_tracking_plan_zh.md`

---

### 阶段 5: 配置和记录管理（第10-11周）

#### 5.1 配置管理器 (config_manager) ✅
**文件**: `src/core/config_manager.cpp`
**说明**: 已实现轻量 TOML 解析、命令行覆盖、配置导出与 reload。

#### 5.2 成交记录管理器 (trade_record) ✅
**文件**: `src/portfolio/trade_record.cpp`
**说明**: 已实现成交索引、按订单/证券查询、统计与 CSV 导出。

#### 5.3 委托记录管理器 (entrust_record) ✅
**文件**: `src/portfolio/entrust_record.cpp`
**说明**: 已实现订单快照更新、活跃委托筛选与 CSV 导出。

#### 5.4 账户信息管理器 (account_info) ✅
**文件**: `src/portfolio/account_info.cpp`
**说明**: 已实现账户配置加载、交易权限判断与手续费计算。

---

### 阶段 6: 核心服务组件（第12-13周）

#### 6.1 事件循环 (event_loop) ✅
**文件**: `src/core/event_loop.cpp`
**说明**: 已实现统一编排层，并保持订单语义收口在 `order_book` / `order_router`。

**已实现关键功能**:
- `run()`, `stop()`, `loop_iteration()`
- `process_upstream_orders()`：上游请求统一编排（风控 -> 路由）
- `process_downstream_responses()`：回报统一收口到 `order_book::update_status/update_trade`
- `handle_trade_response()`：成交后驱动 `position_manager` 更新
- 性能统计：处理计数、空轮询计数、延迟统计（min/max/avg）
- 可选成交 SHM 注入：`set_trades_shm()`

**后续优化计划**:
- 将当前父单全量扫描聚合（O(N 子单数)）演进为增量聚合（O(1) 更新），减少高拆单场景开销。
- 在统计输出中补充尾延迟分位（p99/p999）采样能力。

#### 6.2 账户服务主类 (account_service) ✅
**文件**: `src/core/account_service.cpp`
**说明**: 已实现全组件初始化、运行与退出生命周期管理。

---

### 阶段 7: 主程序入口（第14周）

#### 7.1 主程序 (main) ✅
**文件**: `src/main.cpp`
**说明**: 已实现启动入口、参数解析（`--config` / `--help`）、服务初始化与运行。

---

## 依赖关系图

```
阶段1: 基础工具
├── spinlock (无依赖)
└── time_utils (无依赖)

阶段2: 共享内存基础设施
└── shm_manager (依赖: types, shm_layout)

阶段3: 核心交易组件
├── position_manager (依赖: types, positions, spinlock, shm_layout)
├── order_book (依赖: types, constants, order_request, spinlock)
├── risk_checker (依赖: types, order_request, position_manager)
└── risk_manager (依赖: risk_checker, position_manager)

阶段4: 订单处理组件
├── order_splitter (依赖: types, order_request)
└── order_router (依赖: order_book, order_splitter, shm_layout)

阶段5: 配置和记录
├── config_manager (依赖: types, risk_config, split_config)
├── trade_record (依赖: types)
├── entrust_record (依赖: types, order_request)
└── account_info (依赖: types)

阶段6: 核心服务
├── event_loop (依赖: config, shm_layout, order_book, router, positions, risk)
└── account_service (依赖: 所有其他组件)

阶段7: 主程序
└── main (依赖: account_service)
```

---

## 验证测试计划

每个阶段完成后应运行以下测试：

### 阶段1-2 验证
- 自旋锁并发测试
- 共享内存创建/打开/关闭测试（`test/test_shm_manager.cpp`）
- 共享内存队列读写测试

### 阶段3-4 验证
- 持仓管理测试（`test/test_position_manager.cpp`）
- 风控管理测试（`test/test_risk_manager.cpp`）
- 订单簿拆单追踪测试（`test/test_order_book_split_tracking.cpp`）
- 路由拆单撤单测试（`test/test_order_router_split_cancel.cpp`）
- 拆单策略测试（固定量、冰山、TWAP）

### 阶段5-6 验证
- 配置加载/验证/热更新测试（`test/test_config_manager.cpp`）
- 事件循环测试（`test/test_event_loop.cpp`）
- 账户服务初始化/运行/停止测试（`test/test_account_service.cpp`）
- 集成测试（完整订单生命周期）

### 阶段7 验证
- 端到端测试（策略 API -> 账户服务 -> 模拟交易进程）
- 性能基准测试（吞吐 + p99/p999 延迟）
- 压力测试

---

## 文件清单

### 已新建文件

```
src/
├── common/
│   ├── spinlock.cpp
│   └── time_utils.cpp
├── main.cpp
├── shm/
│   └── shm_manager.cpp
├── core/
│   ├── config_manager.cpp
│   ├── event_loop.cpp
│   └── account_service.cpp
├── portfolio/
│   ├── position_manager.cpp
│   ├── account_info.cpp
│   ├── trade_record.cpp
│   └── entrust_record.cpp
├── risk/
│   ├── risk_checker.cpp
│   └── risk_manager.cpp
└── order/
    ├── order_book.cpp
    ├── order_router.cpp
    └── order_splitter.cpp

test/
├── test_order_book_split_tracking.cpp
├── test_order_router_split_cancel.cpp
├── test_risk_manager.cpp
├── test_event_loop.cpp
├── test_config_manager.cpp
└── test_account_service.cpp

docs/plans/
└── order_book_split_tracking_plan_zh.md
```

### 仍需新建文件

```
（无）
```

### 已更新的文件
- `src/CMakeLists.txt`: 新增 `acct_portfolio`、`acct_core_service`、`acct_service_main`，并梳理核心目标依赖
- `test/CMakeLists.txt`: 新增配置与服务测试目标，统一改为链接库目标
- `src/order/order_book.hpp`: 新增父子追踪接口与内部映射结构
- `src/core/event_loop.hpp`: 新增 `set_trades_shm()` 接口
- `src/core/config_manager.hpp`: 更新默认 SHM 名称与默认 account_id
- `src/core/account_service.hpp`: `state_` 改为原子状态

---

## 估计时间表（滚动）

| 阶段 | 内容 | 状态 |
|------|------|------|
| 1 | 基础工具组件 | ✅ 完成 |
| 2 | 共享内存基础设施 | ✅ 完成 |
| 3 | 核心交易组件 | ✅ 完成 |
| 4 | 订单处理组件 | ✅ 完成 |
| 5 | 配置和记录管理 | ✅ 完成 |
| 6 | 核心服务组件 | ✅ 完成 |
| 7 | 主程序入口 | ✅ 完成 |

---

## 执行清单（可勾选）

### 阶段级清单
- [x] 阶段 1：基础工具组件
- [x] 阶段 2：共享内存基础设施
- [x] 阶段 3：核心交易组件
- [x] 阶段 4：订单处理组件
- [x] 阶段 5：配置和记录管理
- [x] 阶段 6：核心服务组件（`event_loop` / `account_service`）
- [x] 阶段 7：主程序入口

### 阶段 3（核心交易组件）
- [x] `position_manager.cpp`
- [x] `order_book.cpp`（含拆单父子追踪）
- [x] `order_router.cpp`（含扇出撤单与错误锁存）
- [x] `order_splitter.cpp`
- [x] `risk_checker.cpp`
- [x] `risk_manager.cpp`

### 阶段 5（配置和记录管理）
- [x] `config_manager.cpp`
- [x] `trade_record.cpp`
- [x] `entrust_record.cpp`
- [x] `account_info.cpp`

### 阶段 6（核心服务组件）
- [x] `event_loop.cpp`
  - [x] 上游请求统一编排（风控 -> 路由）
  - [x] 下游回报统一收口到 `order_book::update_status/update_trade`
  - [x] 增加关键性能埋点（处理条数、空轮询比、回报延迟）
- [x] `account_service.cpp`

### 阶段 7（主程序入口）
- [x] `main.cpp`

---

## 注意事项

1. **线程安全**: `order_book` 与 `position_manager` 需持续保持锁粒度可控。
2. **性能优先**: 事件循环保持单线程编排与低开销路径，避免无谓跨线程切换。
3. **一致性**: 所有订单与成交更新必须收口到 `order_book` 接口，避免父单聚合失效。
4. **错误处理**: 路由失败策略维持“尽力继续 + 父单错误锁存”，并记录统计。
5. **测试覆盖**: 新增功能必须补对应单元测试和性能回归测试。
