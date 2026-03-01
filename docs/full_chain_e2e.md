# 全链路 E2E 运行说明

本文说明如何运行并观察真实多进程链路：

`下单 -> 风控 -> 下游路由 -> gateway 成交回报 -> 订单状态更新 -> 持仓更新`

## 1. 组件拓扑

本 E2E 场景会启动并协作以下进程：

- `acct_service_main`
- `acct_broker_gateway_main`（`sim` 适配器，`auto_fill=true`）
- `full_chain_observer`
- `order_submit_cli`（发起测试订单）

## 2. 一键运行（推荐）

在仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build --target acct_service_main acct_broker_gateway_main full_chain_observer order_submit_cli -j8
ctest -R test_full_chain_e2e --output-on-failure --test-dir build
```

通过标准：

- `test_full_chain_e2e` 返回 `Passed`
- 订单 CSV 中目标订单满足 `volume_traded > 0 && volume_remain == 0`
- 持仓 CSV 中 `event_kind="position"` 且 `row_key="SZ.000001"` 的记录满足 `position_volume_buy_traded > 0`

## 3. 手工运行脚本

也可以直接运行脚本：

```bash
./test/full_chain_e2e.sh ./build
```

脚本会自动：

- 生成唯一 `run_id` 和唯一 SHM 名称
- 生成临时配置文件并启动三进程
- 调用 `order_submit_cli` 按固定频率连续发单
- 轮询 `orders_events.csv` / `positions_events.csv` 至成功或超时
- 失败时打印关键日志 tail
- 退出时统一清理本次运行生成的 5 类 SHM（upstream/downstream/trades/orders/positions）

说明：

- 默认行为：控制台实时输出 `account_service/gateway/observer` 日志，`1s/单`，随机 `buy/sell`，持续 `60s`。
- `order_submit_cli` 默认在退出时清理其使用的 `upstream/orders` 共享内存名称。
- `test/full_chain_e2e.sh` 发单时会显式传 `--no-cleanup-shm-on-exit`，避免持续发单过程中中途清理导致后续发单失败。

常用可调参数（环境变量）：

- `MONITOR_CONSOLE=0|1`：是否在控制台实时输出进程日志（默认 `1`）
- `SUBMIT_INTERVAL_SEC=N`：发单间隔秒数（默认 `1`）
- `SUBMIT_DURATION_SEC=N`：持续发单时长秒数（默认 `60`）
- `RANDOM_SIDE=0|1`：是否随机买卖方向（默认 `1`）
- `ORDER_COUNT=N`：若设置则改为“固定笔数模式”，优先于持续时长

## 4. 产物目录

每次运行都会生成独立目录：

- `build/e2e_artifacts/<run_id>/orders_events.csv`
- `build/e2e_artifacts/<run_id>/positions_events.csv`
- `build/e2e_artifacts/<run_id>/account_service.stdout.log`
- `build/e2e_artifacts/<run_id>/gateway.stdout.log`
- `build/e2e_artifacts/<run_id>/observer.stdout.log`
- `build/e2e_artifacts/<run_id>/order_submit.stderr.log`
- `build/e2e_artifacts/<run_id>/order_id.txt`
- `build/e2e_artifacts/<run_id>/account_1001.log`（由原日志系统生成）

## 5. CSV 字段说明

### 5.1 `orders_events.csv`

头部：

```text
observed_time_ns,index,seq,internal_order_id,security_id,internal_security_id,stage,status,volume_entrust,volume_traded,volume_remain
```

关键字段：

- `internal_order_id`：用于关联 `order_id.txt`
- `stage`：订单槽位阶段
- `status`：订单状态码
- `volume_traded` / `volume_remain`：判断是否完成成交

### 5.2 `positions_events.csv`

头部：

```text
observed_time_ns,event_kind,row_key,header_position_count,header_last_update_ns,fund_total_asset,fund_available,fund_frozen,fund_market_value,position_index,position_id,position_name,position_available,position_volume_available_t0,position_volume_available_t1,position_volume_buy_traded,position_volume_sell_traded,position_removed
```

`event_kind` 取值：

- `header`：持仓 SHM 头变化
- `fund`：资金行变化
- `position`：证券行变化
- `position_removed`：证券行移除

使用建议：

- `event_kind="position"` 重点关注 `position_id`、`position_volume_buy_traded`、`position_volume_sell_traded`
- `event_kind="fund"` 重点关注 `fund_available`、`fund_frozen`、`fund_market_value`
- `event_kind="header"` 重点关注 `header_position_count`

## 6. 常见排查

### 6.1 交易日不一致

现象：`full_chain_observer` 打开 orders 失败，或长时间无订单事件。

排查：

- 检查 account/gateway/observer 是否使用同一 `trading_day`
- orders 实际 SHM 名称为：`orders_shm_name + "_" + trading_day`

### 6.2 SHM 名称不一致

现象：发单成功但服务/网关无响应。

排查：

- `upstream/downstream/trades/orders/positions` 五类 SHM 名称必须在进程间一致
- 优先使用脚本自动生成配置，避免手工拼写错误

### 6.3 进程未就绪

现象：观测器启动后很快退出，或 CSV 文件未生成。

排查：

- 查看 `observer.stdout.log` 是否有 `open ... failed`
- 查看 `account_service.stdout.log` / `gateway.stdout.log` 是否有共享内存打开失败

### 6.4 风控拦截导致无成交

现象：订单有状态流转但 `volume_traded=0`。

排查：

- 在 E2E 配置中确认已关闭会干扰主路径的风控开关
- 观察 `orders_events.csv` 中是否进入拒单状态
