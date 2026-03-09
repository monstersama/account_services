# `src/portfolio` 模块设计

## 1. 模块定位

`src/portfolio` 负责账户资金与持仓状态管理，以及账户信息、成交记录、委托记录的装载与查询。它是风控检查和成交结算的状态基础层。

源码范围：

- [`src/portfolio/account_info.hpp`](../src/portfolio/account_info.hpp)
- [`src/portfolio/account_info.cpp`](../src/portfolio/account_info.cpp)
- [`src/portfolio/position_manager.hpp`](../src/portfolio/position_manager.hpp)
- [`src/portfolio/position_manager.cpp`](../src/portfolio/position_manager.cpp)
- [`src/portfolio/position_loader.hpp`](../src/portfolio/position_loader.hpp)
- [`src/portfolio/position_loader.cpp`](../src/portfolio/position_loader.cpp)
- [`src/portfolio/trade_record.hpp`](../src/portfolio/trade_record.hpp)
- [`src/portfolio/trade_record.cpp`](../src/portfolio/trade_record.cpp)
- [`src/portfolio/entrust_record.hpp`](../src/portfolio/entrust_record.hpp)
- [`src/portfolio/entrust_record.cpp`](../src/portfolio/entrust_record.cpp)
- [`src/portfolio/positions.h`](../src/portfolio/positions.h)

## 2. 核心职责

- 把 `positions_shm` 解释为“资金行 + 证券持仓行”的统一状态存储。
- 在 fresh SHM 路径下从文件或 SQLite 快照装载初始资金/持仓。
- 在运行期提供资金冻结、解冻、扣减、增加等操作。
- 在运行期提供持仓冻结、解冻、扣减、增加等操作。
- 维护账户信息、成交记录和委托记录的内存态视图。

## 3. 核心数据模型

### 3.1 `positions_shm` 的两类行

`positions_shm_layout.positions[]` 有固定含义：

- `positions[0]`
  - FUND 行
  - 保存资金总览
- `positions[1..position_count]`
  - 证券持仓行
  - 保存每只证券的仓位、买卖累计量和订单计数

`positions.h` 提供了 FUND 行访问器，例如：

- `fund_total_asset_field(...)`
- `fund_available_field(...)`
- `fund_frozen_field(...)`
- `fund_market_value_field(...)`

因此 `PositionManager` 并不直接把某个字段名硬编码为“总资产”或“冻结资金”，而是通过这些访问器进行读写。

### 3.2 `PositionManager`

`PositionManager` 是本模块的运行期核心。

关键状态：

- `positions_shm_layout* shm_`
- `security_to_row_`
- `config_file_path_`
- `db_path_`
- `db_enabled_`

关键操作：

- 初始化：
  - `initialize(AccountId)`
- 资金：
  - `freeze_fund`
  - `unfreeze_fund`
  - `deduct_fund`
  - `add_fund`
  - `apply_buy_trade_fund`
  - `apply_sell_trade_fund`
- 持仓：
  - `freeze_position`
  - `unfreeze_position`
  - `deduct_position`
  - `add_position`
- 查询：
  - `get_position`
  - `get_all_positions`
  - `get_fund_info`
  - `find_security_id`
- 初始化写入：
  - `overwrite_fund_info`
  - `add_security`

### 3.3 账户、成交与委托记录

#### `account_info` / `account_info_manager`

作用：

- 保存账户权限和费率模型
- 提供 `calculate_fee(...)`
- 提供从配置或数据库加载的入口

当前实现特点：

- `load_from_config()` 已实现，按类 INI/section 格式解析配置文件中的 `account.*` 项。
- `load_from_db()` 当前是占位实现，只回填 `account_id` 并将状态置为 `Ready`。

#### `trade_record_manager`

作用：

- 保存当日成交记录及多维索引

当前实现特点：

- `load_today_trades()` 当前只清空内存态并返回成功。
- `save_to_db()` 当前将内容写到 `db_path + ".trades.csv"`，并非真正写回数据库。

#### `entrust_record_manager`

作用：

- 保存委托记录的持久化视图
- 允许从 `OrderRequest` 映射出 `entrust_record`

当前实现特点：

- `load_today_entrusts()` 当前只清空内存态并返回成功。
- `save_to_db()` 当前将内容写到 `db_path + ".entrusts.csv"`。

## 4. 初始化流程

### 4.1 fresh SHM 路径

当 `positions_shm.header.init_state != 1` 时，`PositionManager::initialize()` 走 fresh SHM 初始化：

1. 校验 `PositionsHeader` 与布局兼容性。
2. 清空全部 `position` 行。
3. 初始化 FUND 行身份为 `FUND`。
4. 写入默认资金：
   - 总资产 = 100000000
   - 可用 = 100000000
   - 冻结 = 0
   - 市值 = 0
5. 按 `db_enabled_` 选择 `position_loader`：
   - DB 模式：`position_loader::db_source`
   - 文件模式：`position_loader::file_source`
6. 快照装载成功后：
   - 设置 `header.id`
   - 设置 `header.init_state = 1`
   - 更新时间戳

### 4.2 重启恢复路径

当 `init_state == 1` 时：

- 不再访问 SQLite 或 CSV
- 直接扫描 SHM 中已有证券行
- 依据 `position.id` 重建 `security_to_row_`
- 更新 `header.id` 和 `last_update`

这意味着：

- fresh SHM 才会触发外部加载器
- 当日重启完全依赖共享内存已有状态

## 5. 快照加载语义

### 5.1 `position_loader` 文件模式

文件模式读取：

- `config_file + ".positions.csv"`

特点：

- 文件缺失时视作“未加载但不报错”
- 只在 fresh SHM 路径下生效
- 解析 `position` 类型行并为证券建档

### 5.2 `position_loader` DB 模式

DB 模式当前读取 SQLite：

- `account_info`
  - 读取 FUND 所需字段
- `positions`
  - 全表扫描证券持仓行

特点：

- 不做 DB/File 自动回退
- 先写 FUND 行，再装证券行
- 证券通过 `internal_security_id` 反解市场和代码，再调用 `add_security(...)`

## 6. 运行期资金与持仓更新

### 6.1 买单路径

典型链路：

1. 风控通过后，父流程冻结买单所需资金。
2. 成交时调用：
   - `apply_buy_trade_fund()` 或旧兼容路径 `deduct_fund()`
3. 再调用 `add_position()`：
   - 累加 `volume_buy`
   - 累加 `dvalue_buy`
   - 累加 `volume_buy_traded`
   - 累加 `dvalue_buy_traded`
   - 增加 `volume_available_t1`

### 6.2 卖单路径

典型链路：

1. 风控通过后，父流程冻结 `volume_available_t0`。
2. 成交时调用 `deduct_position()`：
   - 优先从已冻结卖出量扣减
   - 若旧路径未先冻结，则回退到可卖仓位扣减
3. 再调用 `apply_sell_trade_fund()`：
   - 增加可用资金
   - 减少市值
   - 按手续费调整总资产

### 6.3 动态建档

如果成交回报命中了一个当前尚未建档的证券，父流程可先调用：

- `add_security(code, name, market)`

然后再写入成交增量。这是 `EventLoop::handle_trade_response()` 中“缺失持仓行补建”的基础。

## 7. 依赖与边界

### 依赖其他模块

- `src/common`
  - 错误模型、日志、基础类型、证券标识工具
- `src/shm`
  - `positions_shm_layout`
- `src/order`
  - `trade_record` / `entrust_record` 对订单类型和枚举的依赖
- `src/core`
  - 启动时装载，运行时由 `EventLoop` 调用资金与持仓更新
- `src/risk`
  - 资金和可卖仓位检查都依赖 `PositionManager`

### 不负责的内容

- 不决定订单是否通过风控。
- 不决定订单如何拆单或路由。
- 不直接控制 SHM 对象的创建与映射。

## 8. 相关专题文档

- [position_loader 设计说明](plans/position_loader.md)
- [订单处理流程图](order_flowchart.md)

## 9. 维护提示

- 若扩展真实 DB 持久化，应优先更新：
  - `account_info_manager::load_from_db()`
  - `trade_record_manager::load_today_trades()`
  - `entrust_record_manager::load_today_entrusts()`
  - 本文档中“当前实现特点”一节
- 若调整 FUND 行字段映射，必须同时检查：
  - `positions.h`
  - `PositionManager`
  - `position_loader`
  - 监控 API 对 FUND 行的解读
- 若调整当日重启语义，必须明确说明 fresh SHM 与 initialized SHM 两条路径是否仍然区分。
