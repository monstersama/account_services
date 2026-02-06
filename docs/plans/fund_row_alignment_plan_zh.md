# 持仓共享内存按“资金行为第0行”对齐方案

## 概述

- 让持仓共享内存布局与业务约束一致：`positions[0]` 固定为资金行，`id` 固定为 `"FUND"`。
- 证券持仓位于 `positions[1..position_count]`，其中 `position_count` 只统计证券行，不包含资金行。
- 移除共享内存布局中的 `fund_info` 独立字段，资金信息统一存储在资金行，并沿用现有行级锁机制。

## 公共 API / ABI 变化

- 共享内存 ABI 变化：`positions_shm_layout` 删除 `fund_info` 字段后，`sizeof(positions_shm_layout)` 与 `header.total_size` 会变化。
- `positions_header::kVersion` 保持 `3`（不升级版本号）。
- `position_manager` 对外函数签名保持不变，但语义变化：`internal_security_id_t` 按行索引解释（`1..position_count`）。

## 实施步骤

### 1) 调整持仓共享内存布局

- 文件：`src/shm/shm_layout.hpp`
- 从 `positions_shm_layout` 中删除 `fund_info fund`。
- 保留 `positions_header`、`position_count`、`positions[]` 结构与对齐策略。
- 保持 `positions_header::kVersion = 3`，并使用新布局尺寸写入 `header.total_size`。

### 2) 定义资金行字段映射

- 文件：`src/portfolio/positions.h`
- 更新注释，明确：
  - `positions[0]` 为资金行，`id = "FUND"`；
  - `position_count` 仅统计证券行。
- 保留 `fund_info` 作为对外资金视图，并新增/调整内联映射辅助：
  - `available`  ↔ `position.available`
  - `total_asset` ↔ `position.volume_available_t0`
  - `frozen` ↔ `position.volume_available_t1`
  - `market_value` ↔ `position.volume_buy`

### 3) 重构 `position_manager` 读写路径

#### 3.1 初始化逻辑

- 文件：`src/portfolio/position_manager.cpp`
- 首次初始化（`init_state != 1`）时：
  - 清空全部 `positions[]`；
  - 设置 `positions[0].id = "FUND"`（必要时同步 `name`）；
  - 写入默认资金并映射到资金行字段；
  - `position_count = 0`；
  - `header.id = 1`；
  - 更新 `header.last_update`。
- 非首次初始化时：
  - 校验/补齐 `positions[0].id` 为 `"FUND"`；
  - 将 `position_count` 限制在 `kMaxPositions - 1`；
  - 扫描 `positions[1..position_count]` 重建 `code_to_id_`。

#### 3.2 资金相关操作

- 文件：`src/portfolio/position_manager.hpp`、`src/portfolio/position_manager.cpp`
- 去除 `fund_cache_`、`fund_lock_`、`sync_fund_to_shm()` 缓存同步路径。
- `get_available_fund`、`freeze_fund`、`unfreeze_fund`、`deduct_fund`、`add_fund`、`get_fund_info` 均改为：
  - 直接锁 `positions[0]`（`position_lock`）；
  - 通过资金行字段映射读写；
  - 修改后更新 `header.last_update`。

#### 3.3 证券持仓操作

- 将 `internal_security_id_t` 作为行索引：
  - `get_position/get_position_mut`：仅在 `id >= 1 && id <= position_count` 时返回对应行；否则返回 `nullptr`。
- `add_security(code, name, market)`：
  - 已存在则返回旧 id；
  - 否则使用 `new_id = position_count + 1` 作为新行；
  - 检查上限 `< kMaxPositions`；
  - 初始化该行后 `position_count++`，`header.id = position_count + 1`，更新 `last_update`。
- `find_security_id` 从 `code_to_id_` 返回行索引。
- `get_all_positions` 仅遍历 `positions[1..position_count]`，排除资金行。

#### 3.4 行创建策略

- `add_position` 不再隐式创建新行。
- 若证券未提前通过 `add_security` 注册，`add_position` 直接返回 `false`，避免产生无标识或错位行。

### 4)（可选）文档同步

- 更新 `docs/plans/phase3_position_manager_plan.md` 中“资金存储在 `positions_shm_layout::fund`”等陈述。
- 更新 `docs/lock_design_discussion.md` 中对资金锁/缓存的说明，使其反映“资金行+行锁”的新实现。

## 测试与验收场景

1. **空共享内存初始化**：
   - `position_count == 0`
   - `positions[0].id == "FUND"`
   - 默认资金值正确写入映射字段。
2. **新增证券**：
   - `add_security("000001", ...)` 返回 `1`
   - `positions[1].id == "000001"`
   - `position_count == 1`。
3. **资金操作正确性**：
   - 冻结/解冻/扣减/增加资金均能正确更新资金行映射字段。
4. **持仓操作边界**：
   - 不允许访问/修改 `id=0` 作为证券；
   - 未注册证券执行 `add_position` 返回失败。
5. **查询行为**：
   - `get_all_positions()` 不返回资金行。

## 假设与默认值

- 共享内存目标布局：`positions_header + position_count + positions[]`。
- 资金行固定在 `positions[0]`，其 `id` 固定为 `"FUND"`。
- `position_count` 仅统计证券行。
- 证券 ID 不做稀疏分配，按行顺序连续增长。
- 历史旧布局（含 `fund_info`）由于 `total_size` 不匹配会在打开阶段失败，需要外部重建共享内存。
