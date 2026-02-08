# order_book 拆单追踪与父单映射方案

## 概述

- 目标：当系统从上游原始订单生成拆单子订单后，持续追踪父单与子单（含自动子撤单）的关系，并支持从任意一端进行回溯查询。
- 方案范围：端到端覆盖 `order_book` 与 `order_router`（必要时联动事件流调用点），确保映射不仅定义在数据结构中，还会在真实路由流程中被写入和维护。
- 状态策略：子单状态和成交变化实时聚合到父单；父单遇到发送失败时错误状态锁存为 `TraderError`。
- 生命周期策略：父子映射保留到 `clear()`；`archive_order()` 不删除映射。

## 公共 API / ABI 变化

- `order_book` 新增双向查询接口：
  - `std::vector<internal_order_id_t> get_children(internal_order_id_t parent_id) const;`
  - `bool try_get_parent(internal_order_id_t child_id, internal_order_id_t& out_parent_id) const noexcept;`
- `order_book` 内部新增追踪结构：
  - `parent_to_children`：父单到子单列表映射。
  - `child_to_parent`：子单到父单映射。
  - `split_parent_error_latched`：父单发送错误锁存标记。
- 明确不变项：
  - 不修改 `order_request` 布局与 ABI，保持 `sizeof(order_request) == 192`。
  - 不扩展 `order_status_t` 枚举，父单进度通过已有状态 + 聚合字段表达。

## 实施步骤

### 1) order_book：映射登记、聚合与清理策略

- 在 `add_order()` 中：
  - 当订单为拆单子单（`is_split_child == true` 且 `parent_order_id != 0`）时，登记 `parent_to_children` 与 `child_to_parent`。
  - 子撤单请求同样作为子链路写入映射，保证追踪完整性。
- 在 `update_status()` 与 `update_trade()` 中：
  - 若更新对象为子单，触发父单聚合刷新。
  - 聚合内容包含：`volume_traded`、`dvalue_traded`、`dfee_executed`、`volume_remain`、`last_update_ns`。
- 父单状态收敛规则：
  - 若父单错误已锁存，状态固定为 `TraderError`。
  - 若所有拆出的新委托子单均进入终态，父单置为 `Finished`。
  - 否则按既有状态优先级收敛到父单进行中状态。
- 在 `archive_order()` 与 `clear()` 中：
  - `archive_order()` 仅归档订单，不清理父子映射。
  - `clear()` 清理全部订单数据与映射数据（含错误锁存状态）。

### 2) order_router：拆单登记、父单撤单扇出、发送失败策略

- 在拆单路径（如 `handle_split_order()`）中：
  - 为每个子单创建 `order_entry` 并标记父单关系（`is_split_child` / `parent_order_id`）。
  - 子单先写入 `order_book` 再发送下游，确保失败场景仍保留可追踪链路。
- 在父单撤单路径（`route_cancel()`）中：
  - 若撤单目标是父单，则自动扇出到该父单全部活跃子单。
  - 每个子撤单请求保留可回溯关系，支持“子撤单 -> 子单 -> 父单”追踪。
- 下游发送失败策略：
  - 采用“尽力继续”：单个子单发送失败时继续处理剩余子单。
  - 一旦出现发送失败，父单错误状态锁存为 `TraderError`，后续不升为 `Finished`。

### 3) 构建配置同步

- 若新增或落地 `order_book.cpp`、`order_router.cpp`、`order_splitter.cpp` 等实现文件，需要同步更新 `src/CMakeLists.txt` 与相关测试构建目标，避免接口声明与编译入口不一致。

## 测试与验收场景

### A. order_book 映射与聚合正确性

- 父单/子单双向可查：`get_children()` 与 `try_get_parent()` 返回一致。
- 子撤单可回溯到父单：任一子撤单都可反查所属父单。
- 子单成交变化后，父单聚合字段实时更新且可解释。
- 子单状态推进后，父单状态按收敛规则变化。
- 子单全部终态后，父单进入终态；若曾发生发送失败，父单最终保持 `TraderError`。

### B. order_router 扇出撤单与部分失败策略

- 父单撤单会扇出为多个子撤单请求并写入下游。
- 部分子单下游发送失败时，剩余子单仍继续发送（尽力继续）。
- 失败发生后父单错误状态锁存有效，防止错误被后续事件覆盖为 `Finished`。

### C. 生命周期与清理行为

- 映射保留到 `clear()`：`archive_order()` 后仍可查询历史父子关系。
- 执行 `clear()` 后，所有父子映射与错误锁存状态被完整清空。

## 假设与默认值

- 只做单层父子关系，不实现多层递归拆单树。
- 映射数据保留到 `clear()`，不在 `archive_order()` 或父单终态时自动清理。
- 不自动归档父单，父单归档仍走现有统一清理流程。
- 父单撤单语义固定为对子单扇出撤单。
- 下游发送失败策略固定为“尽力继续 + 父单错误锁存”。
