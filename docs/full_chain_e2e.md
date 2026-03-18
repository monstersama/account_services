# 全链路 E2E 运行说明

本文说明如何运行并观察真实多进程链路：

`下单 -> 风控 -> 下游路由 -> gateway 成交回报 -> 订单状态更新 -> 持仓更新`

## 1. 组件拓扑

本 E2E 场景会启动并协作以下进程：

- `acct_service_main`
- `acct_broker_gateway_main`（`sim` 适配器，`auto_fill=true`）
- `full_chain_observer`
- `order_submit_cli`（由 `test/full_chain_submit.sh` 调用）

## 2. 一键运行（推荐）

在仓库根目录执行：

```bash
cmake -S . -B build
cmake --build build --target acct_service_main acct_broker_gateway_main full_chain_observer order_submit_cli -j8
ctest -R test_full_chain_e2e --output-on-failure --test-dir build
```

通过标准：

- `test_full_chain_e2e` 返回 `Passed`
- `order_ids.csv` 中记录的所有已提交订单，都能在 `orders_final.csv` 中看到 `stage >= 4`
- 对于 `orders_final.csv` 中已经产生 `volume_traded > 0` 的证券，`positions_final.csv` 中对应 `event_kind="position"` 的行存在买入或卖出成交累计值

## 3. 手工运行脚本

也可以直接运行脚本：

```bash
./test/full_chain_e2e.sh ./build
```

仅需发单（不拉起进程）时可使用：

```bash
./test/full_chain_submit.sh ./build
```

启动 `account_service + gateway` 并固定发送 200 条订单（默认保持进程运行，不清理共享内存）时可使用：

```bash
./test/full_chain_no_cleanup.sh ./build
```

脚本会自动：

- 生成唯一 `run_id` 和唯一 SHM 名称
- 通过 `--config` 参数启动 `account_service/gateway/observer` 三进程
- 调用 `test/full_chain_submit.sh` 按固定频率连续发单
- 轮询 `orders_final.csv` / `positions_final.csv` 至成功或超时
- 失败时打印关键日志 tail
- 退出时统一清理本次运行生成的 5 类 SHM（upstream/downstream/trades/orders/positions）

说明：

- 默认行为：控制台实时输出 `account_service/gateway/observer` 日志，默认提交 `100` 笔订单，发单间隔 `0.1s`，买卖方向默认随机。
- `full_chain_observer` 仅支持 `--config`（或位置参数 `config_path`）启动，业务参数统一走 YAML。
- `order_submit_cli` 的参数组织由 `test/full_chain_submit.sh` 负责，E2E runner 本身不内嵌发单细节。

常用可调参数（环境变量）：

- `MONITOR_CONSOLE=0|1`：是否在控制台实时输出进程日志（默认 `1`）
- `ORDER_COUNT=N`：固定发单笔数（默认 `100`）
- `SUBMIT_INTERVAL_SEC=N`：发单间隔秒数（默认 `0.1`）
- `SUBMIT_DURATION_SEC=N`：持续发单时长秒数（默认 `60`；仅在未指定 `ORDER_COUNT` 时生效）
- `RANDOM_SIDE=0|1`：是否随机买卖方向（默认 `1`）
- `VALID_SEC=N`：传给 `order_submit_cli --valid-sec` 的有效秒数（默认 `0`）
- `PASSIVE_EXEC_ALGO=default|none|fixed_size|twap|vwap|iceberg`：传给 `order_submit_cli --passive-exec-algo` 的逐单被动执行算法（默认 `default`）
- `TIMEOUT_SEC=N`：总等待超时；脚本会根据发单笔数和间隔自动抬高下限

## 4. 产物目录

每次运行都会生成独立目录：

- `build/e2e_artifacts/<run_id>/account_service.stdout.log`
- `build/e2e_artifacts/<run_id>/gateway.stdout.log`
- `build/e2e_artifacts/<run_id>/observer.stdout.log`
- `build/e2e_artifacts/<run_id>/order_submit.stderr.log`
- `build/e2e_artifacts/<run_id>/order_ids.csv`
- `build/e2e_artifacts/<run_id>/order_ids.txt`
- `build/e2e_artifacts/<run_id>/order_id.txt`
- `build/e2e_artifacts/<run_id>/<observer output_dir>/orders_final.csv`
- `build/e2e_artifacts/<run_id>/<observer output_dir>/positions_final.csv`
- `build/e2e_artifacts/<run_id>/account_1001.log`（由原日志系统生成）

默认 `observer.yaml` 中 `output_dir` 为相对路径时，脚本会把它解析到本次 `run_id` 目录下。

## 5. CSV 字段说明

### 5.1 `orders_final.csv`

头部示例：

```text
last_update_time,last_update_ns,index,seq,internal_order_id,security_id,internal_security_id,stage,status,volume_entrust,volume_traded,volume_remain,...
```

当前 E2E 主要关注字段：

- `internal_order_id`：用于关联 `order_ids.csv` / `order_ids.txt`
- `stage`：E2E 成功判定使用 `stage >= 4`
- `status`：订单状态码，便于排查是否卡在风控、路由或交易回报阶段
- `volume_traded` / `volume_remain`：用于判断是否有成交以及剩余数量
- `internal_security_id`：用于把订单成交和 `positions_final.csv` 对应起来

### 5.2 `positions_final.csv`

头部示例：

```text
last_update_time,last_update_ns,event_kind,row_key,header_position_count,header_last_update_time,header_last_update_ns,fund_total_asset,fund_available,fund_frozen,fund_market_value,position_index,position_id,position_name,position_available,...
```

`event_kind` 取值：

- `header`：持仓 SHM 头变化
- `fund`：资金行变化
- `position`：证券行变化
- `position_removed`：证券行移除

当前 E2E 主要关注字段：

- `event_kind="position"`
- `row_key`
- `position_volume_buy_traded`
- `position_volume_sell_traded`

脚本会从 `orders_final.csv` 中筛出已经发生实际成交的证券，再检查这些证券在 `positions_final.csv` 中是否出现了对应的成交累计值。

## 6. 常见排查

### 6.1 交易日不一致

现象：`full_chain_observer` 打开 orders 失败，或长时间无订单事件。

排查：

- 检查 account/gateway/observer 是否使用同一 `trading_day`
- orders 实际 SHM 名称为：`orders_shm_name + "_" + trading_day`

### 6.2 SHM 名称不一致

现象：发单成功但服务 / 网关无响应。

排查：

- `upstream/downstream/trades/orders/positions` 五类 SHM 名称必须在进程间一致
- 优先使用脚本自动生成配置，避免手工拼写错误

### 6.3 进程未就绪

现象：observer 启动后很快退出，或 CSV 文件未生成。

排查：

- 查看 `observer.stdout.log` 是否有 `open ... failed`
- 查看 `account_service.stdout.log` / `gateway.stdout.log` 是否有共享内存打开失败
- 检查 `/dev/shm` 下的 `orders / positions / upstream` 对象是否已经就绪

### 6.4 风控拦截导致无成交

现象：订单有状态流转但 `volume_traded=0`。

排查：

- 在 E2E 配置中确认已关闭会干扰主路径的风控开关
- 观察 `orders_final.csv` 中是否进入拒单状态

### 6.5 observer 输出目录和预期不一致

现象：脚本运行中提示找不到 `orders_final.csv` / `positions_final.csv`。

排查：

- 检查 `observer.yaml` 的 `output_dir`
- 若为相对路径，脚本会把它解析到当前 `run_id` 目录下
- 失败时可直接查看脚本打印的 `artifacts` 目录
