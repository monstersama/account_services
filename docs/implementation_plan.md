# 账户服务项目组件实现计划

## 项目概述

这是一个 C++17 高性能低延迟量化交易账户服务进程，通过共享内存与策略进程和交易进程通信，负责订单风控检查、拆单、路由和成交回报处理。

## 当前状态

### 已完整实现的组件
| 组件 | 文件 | 状态 |
|------|------|------|
| 类型定义 | `src/common/types.hpp` | 完整 |
| 常量定义 | `src/common/constants.hpp` | 完整 |
| 定长字符串 | `src/common/fixed_string.hpp` | 完整 |
| 无锁队列 | `src/shm/spsc_queue.hpp` | 完整 |
| 共享内存布局 | `src/shm/shm_layout.hpp` | 完整 |
| 订单请求结构 | `src/order/order_request.hpp` | 完整 |
| 持仓结构 | `src/portfolio/positions.h` | 完整 |
| 对外 C API | `src/api/order_api.cpp` | 完整 |

### 待实现的组件（只有接口框架）
| 组件 | 文件 | 依赖 |
|------|------|------|
| 自旋锁 | `src/common/spinlock.hpp` | 无 |
| 时间工具 | `src/common/time_utils.hpp` | 无 |
| 共享内存管理器 | `src/shm/shm_manager.hpp` | types, shm_layout |
| 配置管理器 | `src/core/config_manager.hpp` | types, order_splitter, risk_manager |
| 事件循环 | `src/core/event_loop.hpp` | config_manager, shm_layout, order_book, router, positions, risk |
| 账户服务主类 | `src/core/account_service.hpp` | 所有其他组件 |
| 订单簿 | `src/order/order_book.hpp` | types, constants, order_request, spinlock |
| 订单路由器 | `src/order/order_router.hpp` | types, order_book, order_splitter, shm_layout |
| 拆单器 | `src/order/order_splitter.hpp` | types, order_request |
| 风控检查器 | `src/risk/risk_checker.hpp` | types, order_request, position_manager |
| 风控管理器 | `src/risk/risk_manager.hpp` | types, order_request, position_manager, risk_checker |
| 持仓管理器 | `src/portfolio/position_manager.hpp` | types, order_request, positions, spinlock, shm_layout |
| 成交记录管理器 | `src/portfolio/trade_record.hpp` | types |
| 委托记录管理器 | `src/portfolio/entrust_record.hpp` | types, order_request |
| 账户信息管理器 | `src/portfolio/account_info.hpp` | types |

---

## 组件实现顺序

### 阶段 1: 基础工具组件（第1-2周）

#### 1.1 自旋锁实现 (spinlock)
**文件**: `src/common/spinlock.cpp`
**说明**: 实现自旋锁，用于保护订单簿和持仓数据结构
**依赖**: 无
**关键功能**:
- `lock()`: 获取锁（忙等待直到成功）
- `try_lock()`: 尝试获取锁，立即返回
- `unlock()`: 释放锁

#### 1.2 时间工具实现 (time_utils)
**文件**: `src/common/time_utils.cpp`
**说明**: 补充时间工具函数的实现
**依赖**: 无
**关键功能**:
- `md_time_to_str()`: 将市场时间转换为字符串
- `now_md_time()`: 获取当前市场时间（毫秒）

---

### 阶段 2: 共享内存基础设施（第3-4周）

#### 2.1 共享内存管理器 (shm_manager)
**文件**: `src/shm/shm_manager.cpp`
**说明**: 实现 POSIX 共享内存的创建、打开、关闭、删除
**依赖**: types, shm_layout
**关键功能**:
- `open_upstream()`: 创建/打开上游共享内存
- `open_downstream()`: 创建/打开下游共享内存
- `open_positions()`: 创建/打开持仓共享内存
- `close()`: 关闭并解除映射
- `unlink()`: 删除共享内存对象
- `open_impl()`: 内部实现（mmap/shm_open）
- `init_header()`: 初始化共享内存头部
- `validate_header()`: 验证共享内存头部魔数和版本

---

### 阶段 3: 核心交易组件（第5-7周）

#### 3.1 持仓管理器 (position_manager)
**文件**: `src/portfolio/position_manager.cpp`
**说明**: 管理账户资金和持仓（**仅实现初始化，其余函数留空**）
**依赖**: types, positions, spinlock, shm_layout
**完整实现**:
- `initialize()`: 从数据库/文件加载持仓，初始化持仓映射

**仅写函数框架（空实现）**:
- **资金操作**: `freeze_fund()`, `unfreeze_fund()`, `deduct_fund()`, `add_fund()`
- **持仓操作**: `freeze_position()`, `unfreeze_position()`, `deduct_position()`, `add_position()`
- **查询接口**: `get_position()`, `get_all_positions()`, `find_security_id()`, `add_security()`
- **内部辅助**: `get_or_create_position()`, `sync_fund_to_shm()`

#### 3.2 订单簿 (order_book)
**文件**: `src/order/order_book.cpp`
**说明**: 管理活跃订单，提供订单ID生成和状态跟踪
**依赖**: types, constants, order_request, spinlock
**关键功能**:
- `add_order()`: 添加新订单到订单簿
- `find_order()`: 根据内部订单ID查找订单
- `find_by_broker_id()`: 根据券商订单ID查找订单
- `update_status()`: 更新订单状态
- `update_trade()`: 更新订单成交信息（数量、金额、费用）
- `archive_order()`: 将已完成订单移到历史
- `next_order_id()`: 生成新的内部订单ID（原子递增）

#### 3.3 风控检查器 (risk_checker)
**文件**: `src/risk/risk_checker.cpp`
**说明**: 实现各类风控规则
**依赖**: types, order_request, position_manager
**关键功能**:
- **资金检查** (`fund_check_rule`): 检查可用资金是否充足
- **持仓检查** (`position_check_rule`): 检查可卖持仓是否充足
- **单笔金额限制** (`max_order_value_rule`): 检查订单金额是否超过限制
- **单笔数量限制** (`max_order_volume_rule`): 检查订单数量是否超过限制
- **涨跌停价格检查** (`price_limit_rule`): 检查委托价格是否在涨跌停范围内
- **重复订单检查** (`duplicate_order_rule`): 检查是否重复提交相同订单
- **流速限制** (`rate_limit_rule`): 检查每秒订单数是否超过限制

#### 3.4 风控管理器 (risk_manager)
**文件**: `src/risk/risk_manager.cpp`
**说明**: 统筹管理所有风控规则
**依赖**: types, order_request, position_manager, risk_checker
**关键功能**:
- `check_order()`: 对单个订单执行所有风控检查
- `check_orders()`: 批量风控检查
- `initialize_default_rules()`: 初始化默认风控规则
- 规则管理: `add_rule()`, `remove_rule()`, `enable_rule()`
- 价格限制: `update_price_limits()`, `clear_price_limits()`
- 统计: `stats()`, `reset_stats()`

---

### 阶段 4: 订单处理组件（第8-9周）

#### 4.1 拆单器 (order_splitter)
**文件**: `src/order/order_splitter.cpp`
**说明**: 根据策略将大单拆分成多个子订单
**依赖**: types, order_request
**关键功能**:
- `split()`: 执行拆单
- `should_split()`: 判断是否需要拆单
- **拆单策略**:
  - `split_fixed_size()`: 固定数量拆单
  - `split_iceberg()`: 冰山订单（显示小部分，隐藏大部分）
  - `split_twap()`: 时间加权平均价格拆单
- `set_order_id_generator()`: 设置子订单ID生成器

#### 4.2 订单路由器 (order_router)
**文件**: `src/order/order_router.cpp`
**说明**: 将订单发送到下游交易进程
**依赖**: types, order_book, order_splitter, shm_layout
**关键功能**:
- `route_order()`: 路由单个订单（经过风控后调用）
- `route_orders()`: 批量路由
- `route_cancel()`: 处理撤单请求
- `send_to_downstream()`: 写入下游共享内存队列
- `handle_split_order()`: 处理拆单后的子订单

---

### 阶段 5: 配置和记录管理（第10-11周）

#### 5.1 配置管理器 (config_manager)
**文件**: `src/core/config_manager.cpp`
**说明**: 加载和管理系统配置
**依赖**: types, order_splitter, risk_manager
**关键功能**:
- `load_from_file()`: 从 TOML 文件加载配置
- `parse_command_line()`: 从命令行参数解析配置
- `validate()`: 验证配置有效性
- `reload()`: 热更新配置
- `export_to_file()`: 导出当前配置

#### 5.2 成交记录管理器 (trade_record)
**文件**: `src/portfolio/trade_record.cpp`
**说明**: 管理当日成交记录
**依赖**: types
**关键功能**:
- `load_today_trades()`: 从数据库加载当日成交
- `add_trade()`: 添加新成交记录
- `find_trade()`: 查找成交记录
- `save_to_db()`: 保存到数据库

#### 5.3 委托记录管理器 (entrust_record)
**文件**: `src/portfolio/entrust_record.cpp`
**说明**: 管理当日委托记录
**依赖**: types, order_request
**关键功能**:
- `load_today_entrusts()`: 从数据库加载当日委托
- `add_or_update()`: 添加或更新委托记录
- `update_from_order()`: 从订单请求更新委托状态
- `find_entrust()`: 查找委托记录
- `save_to_db()`: 保存到数据库

#### 5.4 账户信息管理器 (account_info)
**文件**: `src/portfolio/account_info.cpp`
**说明**: 管理账户基本信息
**依赖**: types
**关键功能**:
- `load()`: 从数据库/配置加载账户信息
- `update()`: 更新账户信息
- `save()`: 保存到数据库

---

### 阶段 6: 核心服务组件（第12-13周）

#### 6.1 事件循环 (event_loop)
**文件**: `src/core/event_loop.cpp`
**说明**: 订单处理主循环，协调各模块工作
**依赖**: config_manager, shm_layout, order_book, router, positions, risk
**关键功能**:
- `run()`: 运行事件循环（阻塞）
- `stop()`: 请求停止
- `loop_iteration()`: 单次循环迭代
- `process_upstream_orders()`: 处理上游订单队列
- `process_downstream_responses()`: 处理下游成交回报
- `handle_order_request()`: 处理单个订单请求
- `handle_trade_response()`: 处理单个成交回报
- `update_latency_stats()`: 更新延迟统计
- `print_periodic_stats()`: 定期打印统计信息
- `set_cpu_affinity()`: 设置 CPU 亲和性

#### 6.2 账户服务主类 (account_service)
**文件**: `src/core/account_service.cpp`
**说明**: 服务入口，协调所有组件初始化
**依赖**: 所有其他组件
**关键功能**:
- `initialize()`: 初始化服务
- `run()`: 运行服务（阻塞）
- `stop()`: 请求停止
- **初始化子模块**:
  - `init_config()`: 初始化配置
  - `init_shared_memory()`: 初始化共享内存
  - `init_portfolio()`: 初始化持仓管理
  - `init_risk_manager()`: 初始化风控
  - `init_order_components()`: 初始化订单组件
  - `init_event_loop()`: 初始化事件循环
- **加载历史数据**:
  - `load_account_info()`: 加载账户信息
  - `load_positions()`: 加载持仓
  - `load_today_trades()`: 加载当日成交
  - `load_today_entrusts()`: 加载当日委托
- `cleanup()`: 清理资源
- `print_stats()`: 打印统计信息

---

### 阶段 7: 主程序入口（第14周）

#### 7.1 主程序 (main)
**文件**: `src/main.cpp`
**说明**: 程序入口点
**依赖**: account_service
**关键功能**:
- 解析命令行参数
- 创建并初始化 account_service
- 设置信号处理（SIGINT, SIGTERM）
- 运行服务
- 优雅退出

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
- 共享内存创建/打开/关闭测试
- 共享内存队列读写测试

### 阶段3-4 验证
- 持仓管理测试（仅验证初始化）
- 订单簿测试（添加/查找/更新/归档）
- 风控规则测试（各类拒绝场景）
- 拆单器测试（各种策略）

### 阶段5-6 验证
- 配置加载/验证/热更新测试
- 事件循环测试（订单处理流程）
- 集成测试（完整订单生命周期）

### 阶段7 验证
- 端到端测试（策略API -> 账户服务 -> 模拟交易进程）
- 性能基准测试
- 压力测试

---

## 文件清单

### 需要新建的文件

```
src/
├── common/
│   ├── spinlock.cpp
│   └── time_utils.cpp
├── shm/
│   └── shm_manager.cpp
├── portfolio/
│   ├── position_manager.cpp
│   ├── account_info.cpp
│   ├── trade_record.cpp
│   └── entrust_record.cpp
├── order/
│   ├── order_book.cpp
│   ├── order_router.cpp
│   └── order_splitter.cpp
├── risk/
│   ├── risk_checker.cpp
│   └── risk_manager.cpp
├── core/
│   ├── config_manager.cpp
│   ├── event_loop.cpp
│   ├── account_service.cpp
│   └── main.cpp
└── CMakeLists.txt (根目录，添加子目录)
```

### 需要更新的文件

- `src/api/CMakeLists.txt`: 添加依赖的新库
- `src/CMakeLists.txt`: 创建新的 CMake 配置
- 根目录 `CMakeLists.txt`: 添加 src 子目录

---

## 估计时间表

| 阶段 | 内容 | 估计时间 |
|------|------|----------|
| 1 | 基础工具组件 | 1-2周 |
| 2 | 共享内存基础设施 | 1-2周 |
| 3 | 核心交易组件 | 2-3周 |
| 4 | 订单处理组件 | 1-2周 |
| 5 | 配置和记录管理 | 1-2周 |
| 6 | 核心服务组件 | 2-3周 |
| 7 | 主程序入口 | 1周 |
| **总计** | | **约10-15周** |

---

## 注意事项

1. **线程安全**: 持仓管理和订单簿使用自旋锁保护，需确保锁的粒度最小化
2. **性能优先**: 事件循环使用忙轮询模式，避免系统调用开销
3. **错误处理**: 所有组件需有完善的错误处理和日志记录
4. **共享内存**: 注意共享内存的持久化和清理，避免内存泄漏
5. **测试覆盖**: 每个组件都应有单元测试，关键路径要有性能测试
