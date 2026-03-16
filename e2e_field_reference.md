# 账户服务字段与配置说明

## 适用范围

本文解释以下文件里的字段含义：

| 项目 | 当前仓库里的实际路径 | 说明 |
| --- | --- | --- |
| 订单事件日志 | `logs/order_events_1001_19700101.log` | 按行追加的业务事件日志，格式为 `key=value` |
| 订单调试 Trace 日志 | `logs/order_debug_1001_19700101.log` | 按行追加的执行调试 trace，仅在启用 debug order trace 编译开关时生成 |
| 最新 E2E 输出目录 | `build/e2e_artifacts/20260316_124232_951023/output` | 截至 2026-03-16，仓库里当前没有 `build/e2e/<latest>/output`，最新产物实际在 `build/e2e_artifacts/.../output` |
| 订单最终快照 CSV | `build/e2e_artifacts/20260316_124232_951023/output/orders_final.csv` | 最终快照，不是事件流 |
| 持仓最终快照 CSV | `build/e2e_artifacts/20260316_124232_951023/output/positions_final.csv` | 最终快照，不是事件流 |

需要先区分三类文件：

- `order_events_1001_19700101.log` 是业务事件日志。每一行代表一次订单簿里已经可观察到的订单事实变化。
- `order_debug_1001_19700101.log` 是执行调试 trace。每一行代表一次 session / child 推进过程，不等价于订单快照。
- `orders_final.csv` 和 `positions_final.csv` 是最终态快照文件。每次 flush 都会覆盖重写，只保留“当前最后状态”。

## 1. `order_events_1001_19700101.log`

### 1.1 文件格式

| 项目 | 含义 |
| --- | --- |
| 行粒度 | 一行 = 一条订单相关业务事件 |
| 分隔方式 | 字段之间用空格分隔 |
| 字段格式 | `key=value` |
| 字符串字段 | 用双引号包裹，并对 `\\`、`\"`、`\n`、`\r`、`\t` 做转义 |
| 时间字段 | `ts_ns` 是 Unix Epoch 纳秒时间戳 |
| 固定流标识 | `stream="business"` | 用来和 `order_debug_*.log` 做语义区分 |

### 1.2 基础标识字段

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `ts_ns` | 当前事件写日志时的时间戳 | Unix Epoch 纳秒 |
| `account_id` | 当前日志所属账户 ID | 例如 `1001` |
| `trading_day` | 当前日志所属交易日 | 格式 `YYYYMMDD` |
| `stream` | 日志流类型 | 在业务事件日志里固定为 `business` |
| `event` | 业务事件类型 | 见 1.5 |
| `order_id` | 系统内部订单 ID | 订单主键 |
| `parent_order_id` | 母单 ID | 普通订单通常为 `0`；拆单子单时为母单 ID |
| `strategy_id` | 关联的策略 ID | 未使用时通常为 `0` |
| `shm_order_index` | 订单在 `orders_shm` 中的槽位索引 | 可和监控快照里的 `index` 对应 |
| `security_id` | 外部证券代码 | 例如 `"600000"` |
| `internal_security_id` | 内部证券 ID | 例如 `"XSHG_600000"` |
| `detail` | 附加说明文本 | 业务事件里通常为空；只有极少数业务事件会补充说明 |

### 1.3 标志位、状态和执行上下文

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `is_split_child` | 是否为拆单产生的子单 | `true` / `false` |
| `order_type` | 订单类型 | 常见值：`new`、`cancel` |
| `trade_side` | 买卖方向 | `buy`、`sell`、`not_set` |
| `market` | 市场代码 | `sz`、`sh`、`bj`、`hk`、`not_set`、`unknown` |
| `order_state` | 内部订单状态，已经被渲染成文本 | 见 1.6 |
| `passive_execution_algo` | 订单声明的逐单被动执行算法 | `default`、`none`、`fixed_size`、`twap`、`vwap`、`iceberg` |
| `execution_algo` | 当前真正用于母单执行的算法 | 值域同上 |
| `execution_state` | 受管母单执行状态 | `none`、`running`、`cancelling`、`finished`、`failed` |

### 1.4 数量和价格字段

| 字段 | 含义 | 单位 / 备注 |
| --- | --- | --- |
| `volume_entrust` | 委托数量 | 整数股 / 张 |
| `volume_traded` | 已成交数量 | 整数股 / 张 |
| `volume_remain` | 剩余数量 | 整数股 / 张 |
| `target_volume` | 受管母单目标量 | 主要在母单/拆单场景下有意义 |
| `working_volume` | 在途工作量 | 主要在母单/拆单场景下有意义 |
| `schedulable_volume` | 当前可释放给调度器的量 | 主要在母单/拆单场景下有意义 |
| `dprice_entrust` | 委托价 | 分 |
| `dprice_traded` | 成交均价 | 分 |
| `dvalue_traded` | 已成交金额 | 分 |
| `dfee_executed` | 已发生手续费 | 分 |

### 1.5 业务事件日志里不会出现的调试字段

为了避免把“过程 trace”误当成“订单事实”，`order_events_*.log` 当前不会输出下面这些 debug-only 字段：

| 字段 | 不出现在 `order_events_*.log` 的原因 |
| --- | --- |
| `trace` | 这是 `order_debug_*.log` 的事件名键，不属于业务事件流 |
| `child_order_id` | 业务事件直接用 `order_id` 表示当前订单对象 |
| `active_strategy_claimed` | 属于调度过程信息，不属于订单快照事实 |
| `success` | 只对 `child_submit_result` 这类调试 trace 有语义 |
| `cancel_requested` | 只对 `child_finalized` 调试 trace 有语义 |
| `reason` / `result` / `summary` | 这些是 debug trace 的过程解释字段 |

### 1.6 `event` 可取值

`order_events_*.log` 是业务事件日志，正常会出现下面这些事件：

| `event` 值 | 含义 |
| --- | --- |
| `order_added` | 订单已进入内部订单簿 / 订单池 |
| `order_status_updated` | 订单状态发生变化，但不一定有成交 |
| `order_trade_updated` | 订单成交相关字段发生变化，通常意味着有成交更新 |
| `order_archived` | 订单被归档或移出活跃生命周期 |
| `parent_refreshed` | 母单快照被刷新，常见于母单执行 / 拆单路径 |

### 1.7 `order_state` 文本值

| `order_state` | 含义 |
| --- | --- |
| `not_set` | 未初始化 |
| `strategy_submitted` | 策略/API 已提交到服务 |
| `risk_pending` | 等待风控处理 |
| `risk_rejected` | 被风控拒绝 |
| `risk_accepted` | 通过风控 |
| `trader_pending` | 等待交易链路 / gateway 处理 |
| `trader_rejected` | 交易链路在下游提交前拒绝 |
| `trader_submitted` | 已提交到下游 / gateway |
| `trader_error` | 交易链路内部错误 |
| `broker_rejected` | 被券商拒绝 |
| `broker_accepted` | 券商已接受 |
| `market_rejected` | 被交易所 / 市场拒绝 |
| `market_accepted` | 交易所 / 市场已接受 |
| `finished` | 生命周期结束 |
| `unknown` | 未知状态 |

## 2. `order_debug_1001_19700101.log`

### 2.1 文件角色

| 项目 | 含义 |
| --- | --- |
| 日志定位 | 执行引擎 / 拆单过程的调试 trace，不是订单快照 |
| 生成条件 | 只有在启用 `ACCT_ENABLE_DEBUG_ORDER_TRACE` 编译开关时才会生成 |
| 适用场景 | 排查 session 接管、child 派生、child 提交结果、child 收敛过程 |
| 不适合的用途 | 不建议拿它做“最终订单状态”或“orders_shm 槽位事实”唯一依据 |

### 2.2 文件格式

| 项目 | 含义 |
| --- | --- |
| 行粒度 | 一行 = 一条执行过程 trace |
| 分隔方式 | 字段之间用空格分隔 |
| 字段格式 | `key=value` |
| 固定流标识 | `stream="debug"` |
| 事件名键 | 使用 `trace=...`，和业务日志的 `event=...` 明确区分 |

### 2.3 基础标识字段

| 字段 | 含义 | 备注 |
| --- | --- | --- |
| `ts_ns` | trace 生成时间戳 | Unix Epoch 纳秒 |
| `account_id` | 当前日志所属账户 ID | 例如 `1001` |
| `trading_day` | 当前日志所属交易日 | 格式 `YYYYMMDD` |
| `stream` | 日志流类型 | 在调试 trace 日志里固定为 `debug` |
| `trace` | 调试 trace 类型 | 见 2.5 |
| `parent_order_id` | 当前 trace 所属父单 ID | session 级 trace 会把父单 ID 写在这里 |
| `child_order_id` | 当前 trace 所属子单 ID | 只在 child trace 中出现 |
| `strategy_id` | 关联的策略 ID | 未使用时通常为 `0` |
| `shm_order_index` | 对应 `orders_shm` 槽位索引 | 只有在当前 trace 已拿到真实槽位时才会出现 |

### 2.4 过程字段

| 字段 | 含义 | 何时出现 |
| --- | --- | --- |
| `active_strategy_claimed` | 当前 child 是否由主动策略命中 | 仅 `child_submit_attempt` / `child_submit_result` |
| `success` | child 提交是否成功 | 仅 `child_submit_result` |
| `cancel_requested` | child finalize 时父单是否已收到撤单请求 | 仅 `child_finalized` |
| `reason` | session 被拒绝的原因 | 仅 `session_rejected` |
| `result` | child 提交结果说明 | 仅 `child_submit_result`，常见如 `submitted`、`route_failed` |
| `summary` | child finalize 摘要 | 仅 `child_finalized`；当前实现通常省略 |

补充说明：

- `session_started` / `session_rejected` 是 session 级 trace，不会输出 `child_order_id`。
- `child_submit_attempt` 发生在 orders_shm 槽位真正分配之前，因此通常也不会带 `shm_order_index`。
- `child_submit_result` / `child_finalized` 现在会尽量输出真实 `shm_order_index`。

### 2.5 `trace` 可取值

| `trace` 值 | 含义 |
| --- | --- |
| `session_started` | 父单已被执行引擎接管 |
| `session_rejected` | 父单未能进入执行会话 |
| `child_submit_attempt` | 当前预算窗口下尝试派生一笔 child |
| `child_submit_result` | child 提交结果已经落地 |
| `child_finalized` | child 已经进入终态并完成账本收口 |

## 3. `orders_final.csv`

### 3.1 文件语义

| 项目 | 含义 |
| --- | --- |
| 行粒度 | 一行 = 一个当前可见订单的最终快照 |
| 排序方式 | 由于 sink 内部用的是 `std::map<uint32_t, ...>`，所以通常按 `internal_order_id` 升序输出 |
| 刷新方式 | 每次 flush 覆盖写整张快照 |
| 时间格式 | `last_update_time` 是由 `last_update_ns` 转换出来的本地时间字符串 |

### 3.2 列说明

| 列名 | 含义 | 单位 / 备注 |
| --- | --- | --- |
| `last_update_time` | 人类可读时间 | 本地时区格式：`YYYY-MM-DD HH:MM:SS.mmm` |
| `last_update_ns` | 订单槽位最近更新时间 | Unix Epoch 纳秒 |
| `index` | 订单在 `orders_shm` 中的槽位索引 | 与日志中的 `shm_order_index` 是同一概念 |
| `seq` | 槽位 seqlock 序号 | 偶数表示稳定快照，奇数表示写入中 |
| `internal_order_id` | 系统内部订单 ID | 订单主键 |
| `security_id` | 外部证券代码 | 例如 `600000` |
| `internal_security_id` | 内部证券 ID | 例如 `XSHG_600000` |
| `stage` | 订单槽位阶段 | 原始数值码，见 3.3 |
| `status` | 内部订单状态码 | 原始数值码，见 3.4 |
| `volume_entrust` | 委托数量 | 整数股 / 张 |
| `volume_traded` | 已成交数量 | 整数股 / 张 |
| `volume_remain` | 剩余数量 | 整数股 / 张 |
| `side` | 买卖方向 | 已渲染成文本：`buy`、`sell`、`not_set`、`unknown` |
| `dprice_entrust` | 委托价 | 分 |
| `dprice_traded` | 成交均价 | 分 |
| `price_entrust` | 文本形式的委托价 | 例如 `"50.00"` |
| `price_traded` | 文本形式的成交均价 | 例如 `"0.00"` |

### 3.3 `stage` 数值含义

| 十进制值 | 枚举名 | 含义 |
| --- | --- | --- |
| `0` | `ACCT_MON_STAGE_EMPTY` | 空槽位 |
| `1` | `ACCT_MON_STAGE_RESERVED` | 已保留 |
| `2` | `ACCT_MON_STAGE_UPSTREAM_QUEUED` | 已入上游队列 |
| `3` | `ACCT_MON_STAGE_UPSTREAM_DEQUEUED` | 已被账户服务从上游队列取出 |
| `4` | `ACCT_MON_STAGE_RISK_REJECTED` | 风控拒绝 |
| `5` | `ACCT_MON_STAGE_DOWNSTREAM_QUEUED` | 已入下游队列 |
| `6` | `ACCT_MON_STAGE_DOWNSTREAM_DEQUEUED` | 已被 gateway 消费 |
| `7` | `ACCT_MON_STAGE_TERMINAL` | 终态 |
| `8` | `ACCT_MON_STAGE_QUEUE_PUSH_FAILED` | 队列推送失败 |

### 3.4 `status` 数值含义

CSV 里 `status` 保存的是十进制原始数值；内部文档里通常同时给出十六进制写法。

| 十进制 | 十六进制 | 状态名 | 含义 |
| --- | --- | --- | --- |
| `18` | `0x12` | `StrategySubmitted` | 策略/API 已提交 |
| `32` | `0x20` | `RiskControllerPending` | 等待风控 |
| `33` | `0x21` | `RiskControllerRejected` | 风控拒绝 |
| `34` | `0x22` | `RiskControllerAccepted` | 风控通过 |
| `48` | `0x30` | `TraderPending` | 等待交易链路 |
| `49` | `0x31` | `TraderRejected` | 交易链路拒绝 |
| `50` | `0x32` | `TraderSubmitted` | 交易链路已提交下游 |
| `51` | `0x33` | `TraderError` | 交易链路内部错误 |
| `65` | `0x41` | `BrokerRejected` | 券商拒绝 |
| `66` | `0x42` | `BrokerAccepted` | 券商接受 |
| `81` | `0x51` | `MarketRejected` | 交易所 / 市场拒绝 |
| `82` | `0x52` | `MarketAccepted` | 交易所 / 市场接受 |
| `98` | `0x62` | `Finished` | 已结束 |
| `255` | `0xFF` | `Unknown` | 未知 |

## 4. `positions_final.csv`

### 4.1 文件语义

| 项目 | 含义 |
| --- | --- |
| 行粒度 | 一行 = 一个当前可见的 header / fund / position 快照 |
| 刷新方式 | 每次 flush 覆盖写整张快照 |
| 时间格式 | `last_update_time` 是由 `last_update_ns` 转出来的本地时间 |
| 行模型 | 一张宽表同时承载 `header`、`fund`、`position` 三种行类型 |

### 4.2 `event_kind` 行类型

| `event_kind` | `row_key` 的含义 | 主要有效列 |
| --- | --- | --- |
| `header` | 固定为 `positions_shm` | `header_*` |
| `fund` | 资金行 ID，通常是 `FUND` | `fund_*` |
| `position` | 内部证券 ID，通常与 `position_id` 相同 | `position_*` |

补充说明：

- 当前 `positions_final.csv` 是“最终态快照”，不是事件流。
- position 行如果被移除，会直接从内存快照里删除，因此不会在最终 CSV 中留下单独一行 `position_removed` 事件。
- 因此当前这个文件里的 `position_removed` 列，实际会始终写成 `0`。

### 4.3 列说明

| 列名 | 含义 | 何时有值 / 备注 |
| --- | --- | --- |
| `last_update_time` | 人类可读时间 | 所有行类型都有 |
| `last_update_ns` | 最近更新时间戳 | 所有行类型都有，Unix Epoch 纳秒 |
| `event_kind` | 行类型 | `header`、`fund`、`position` |
| `row_key` | observer 使用的稳定行键 | 可能是 `positions_shm`、资金 ID、内部证券 ID |
| `header_position_count` | 当前可见证券行数量 | 仅 `header` 行有值 |
| `header_last_update_time` | 持仓 SHM 头部更新时间的可读文本 | 仅 `header` 行有值 |
| `header_last_update_ns` | 持仓 SHM 头部更新时间的原始纳秒值 | 仅 `header` 行有值 |
| `fund_total_asset` | 资金总资产 | 仅 `fund` 行有值，单位为分 |
| `fund_available` | 可用资金 | 仅 `fund` 行有值，单位为分 |
| `fund_frozen` | 冻结资金 | 仅 `fund` 行有值，单位为分 |
| `fund_market_value` | 持仓市值 | 仅 `fund` 行有值，单位为分 |
| `position_index` | 证券逻辑索引，范围 `[0, position_count)` | 仅 `position` 行有值 |
| `position_id` | 持仓行的内部证券 ID | 仅 `position` 行有值 |
| `position_name` | 持仓行名称 | 仅 `position` 行有值；当前样例里通常与 `position_id` 相同 |
| `position_available` | 可用持仓数量 | 仅 `position` 行有值 |
| `position_volume_available_t0` | T+0 可用数量 | 仅 `position` 行有值 |
| `position_volume_available_t1` | T+1 可用数量 | 仅 `position` 行有值 |
| `position_volume_buy_traded` | 累计买成交量 | 仅 `position` 行有值 |
| `position_volume_sell_traded` | 累计卖成交量 | 仅 `position` 行有值 |
| `position_removed` | 预留的“行已移除”标志列 | 当前最终快照文件里始终为 `0` |

### 4.4 监控快照里存在但当前 CSV 没导出的字段

`positions_final.csv` 只是 position monitor API 的一个裁剪视图，下面这些字段在监控快照里存在，但当前 CSV 没写出来：

| 快照类型 | 当前未导出的字段 |
| --- | --- |
| Fund snapshot | `name`、`count_order` |
| Position snapshot | `row_index`、`volume_buy`、`dvalue_buy`、`dvalue_buy_traded`、`volume_sell`、`dvalue_sell`、`dvalue_sell_traded`、`count_order` |

## 5. `config/default.yaml` 配置项说明

### 5.1 顶层结构和通用规则

`config/default.yaml` 当前顶层包含这些 section：

| 顶层键 | 说明 |
| --- | --- |
| `account_id` | 账户 ID |
| `trading_day` | 交易日 |
| `shm` | 五类共享内存和打开模式 |
| `event_loop` | 主事件循环行为 |
| `market_data` | 行情快照读入配置 |
| `active_strategy` | 主动策略覆盖层配置 |
| `risk` | 风控规则开关和阈值 |
| `split` | 统一执行会话默认参数 |
| `log` | 通用技术日志配置 |
| `business_log` | 订单业务日志配置 |
| `db` | DB / 初始加载相关配置 |

通用解析规则：

- `event_loop` 和 `EventLoop` 两种写法都能被解析；`default.yaml` 当前使用的是 `event_loop`。
- 任何未知键都会被判为配置错误。
- 所有值都必须是 YAML 标量，不能把某个键写成数组或更深层对象。
- 基础校验包括：`account_id != 0`、`trading_day` 必须是 8 位数字、SHM 名不能为空、`poll_batch_size > 0`。

联动约束：

- `market_data.enabled=true` 时，`market_data.snapshot_shm_name` 不能为空。
- `active_strategy.enabled=true` 时，要求 `market_data.enabled=true`。
- `split.strategy != "none"` 时，要求 `market_data.enabled=true`，并且 `split.max_child_count > 0`。
- `business_log.enabled=true` 时，要求 `output_dir` 非空、`queue_capacity >= 2`、`flush_interval_ms > 0`。

### 5.2 根节点配置

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `account_id` | `1001` | 当前账户服务实例对应的账户 ID | 必须非 0；日志文件名、SHM 打开和账户数据加载都会用到它 |
| `trading_day` | `"19700101"` | 当前交易日 | 必须是 8 位数字；`orders_shm` 的实际运行名会拼上这个交易日后缀 |

### 5.3 `shm` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `shm.upstream_shm_name` | `"/upstream_order_shm"` | 上游订单入口 SHM 名 | 策略/API 把订单索引推入这条队列，`EventLoop` 从这里消费 |
| `shm.downstream_shm_name` | `"/downstream_order_shm"` | 下游发单队列 SHM 名 | 账户服务把下游订单索引推给 gateway |
| `shm.trades_shm_name` | `"/trades_shm"` | 成交 / 状态回报 SHM 名 | gateway 回写回报，账户服务从这里读 |
| `shm.orders_shm_name` | `"/orders_shm"` | 订单池 SHM 基础名 | 实际打开时会变成 `orders_shm_name + "_" + trading_day`，例如 `/orders_shm_19700101` |
| `shm.positions_shm_name` | `"/positions_shm"` | 持仓 SHM 名 | 不带交易日后缀 |
| `shm.create_if_not_exist` | `true` | 打开 SHM 时使用 `OpenOrCreate` 还是 `Open` | `true` 表示不存在就创建；`false` 表示必须已有现成 SHM，否则初始化失败 |

### 5.4 `event_loop` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `event_loop.busy_polling` | `true` | 空闲时是否持续忙轮询 | `true` 时空闲不 sleep，延迟更低但更占 CPU |
| `event_loop.poll_batch_size` | `64` | 每轮最多处理多少条上游订单或下游回报 | `process_upstream_orders()` 和 `process_downstream_responses()` 都会用这个上限；必须大于 0 |
| `event_loop.idle_sleep_us` | `0` | 空闲 sleep 时长 | 只有在 `busy_polling=false` 且本轮无订单无回报时才会生效；单位微秒 |
| `event_loop.stats_interval_ms` | `1000` | 周期性统计打印间隔 | `>0` 时才会打印事件循环统计；单位毫秒 |
| `event_loop.archive_terminal_orders` | `false` | 是否在订单终态后归档 `OrderBook` 里的订单 | `false` 时终态订单继续保留在活动簿里；`true` 时根据延迟配置归档 |
| `event_loop.terminal_archive_delay_ms` | `2000` | 终态订单归档延迟 | 仅在 `archive_terminal_orders=true` 时有意义；`0` 表示一到终态立即归档 |
| `event_loop.pin_cpu` | `false` | 是否给事件循环线程绑核 | `true` 且 `cpu_core >= 0` 时才会尝试设置 CPU affinity |
| `event_loop.cpu_core` | `-1` | 绑核目标核心编号 | `-1` 表示不指定；只有 `pin_cpu=true` 时才考虑这个值 |

### 5.5 `market_data` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `market_data.enabled` | `false` | 是否启用行情模块 | 关闭时 `MarketDataService` 走 no-op 路径 |
| `market_data.snapshot_shm_name` | `"/signal_xshg_v2"` | 行情快照源 SHM 名 | 启用后执行引擎会用它读盘口，主动策略会用它读 prediction |

### 5.6 `active_strategy` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `active_strategy.enabled` | `false` | 是否启用主动策略覆盖层 | `true` 时要求 `market_data.enabled=true` |
| `active_strategy.name` | `"none"` | 主动策略实现名 | 当前内建只支持 `"prediction_signal"`；`"none"` 或空字符串表示不用主动策略 |
| `active_strategy.signal_threshold` | `0.0` | 主动策略信号阈值 | `prediction_signal` 会对其取绝对值：买单要求 `signal >= threshold`，卖单要求 `signal <= -threshold` |

### 5.7 `risk` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `risk.max_order_value` | `0` | 单笔订单金额上限 | 单位为分；`0` 表示关闭该限制 |
| `risk.max_order_volume` | `0` | 单笔订单数量上限 | `0` 表示关闭该限制 |
| `risk.max_daily_turnover` | `0` | 日内成交额 / 周转额上限 | 当前代码里已解析并导出，但默认风控规则链暂未消费这个字段 |
| `risk.max_orders_per_second` | `0` | 每秒最大下单数 | `0` 表示关闭速率限制 |
| `risk.enable_price_limit_check` | `true` | 是否启用涨跌停价格检查 | 只有对应证券已经注入价格上下限时，这条规则才真正生效 |
| `risk.enable_duplicate_check` | `true` | 是否启用重复单检查 | 当前实现更接近“同内部订单 ID 防重复进入”，不是“证券+方向+价格+数量”的业务重复单识别 |
| `risk.enable_fund_check` | `true` | 是否启用买单资金检查 | 只对新买单生效 |
| `risk.enable_position_check` | `true` | 是否启用卖单持仓检查 | 只对新卖单生效 |
| `risk.duplicate_window_ns` | `100000000` | 重复单检查时间窗口 | 单位纳秒；默认约 100ms |

### 5.8 `split` 段

`split` 这个名字沿用了旧叫法，但当前语义已经是“统一执行会话默认参数”，而不只是旧版一次性拆单器。

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `split.strategy` | `"none"` | 默认执行算法 | 可选 `none`、`fixed_size`、`twap`、`vwap`、`iceberg`；当前 `vwap` 已接入口，但会被明确判为 unsupported |
| `split.max_child_volume` | `0` | 单个子单最大数量 | `0` 表示不设显式上限，此时会退回使用 `min_child_volume` 至少 1 股/张 |
| `split.min_child_volume` | `100` | 默认 clip / 最小子单量 | 当 `max_child_volume=0` 时，会作为默认子单量基线 |
| `split.max_child_count` | `100` | 单个父单最多允许生成多少个子单 | 当 `strategy != none` 时必须大于 0 |
| `split.interval_ms` | `0` | 会话推进时间间隔 | 对 TWAP 等时间片算法最关键；单位毫秒 |
| `split.randomize_factor` | `0.0` | 子单随机化因子 | 当前配置已解析并导出，但现有执行引擎代码尚未实际消费这个字段 |

### 5.9 `log` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `log.log_dir` | `"./logs"` | 技术日志输出目录 | 主服务默认文件名形如 `account_<account_id>.log` |
| `log.log_level` | `"info"` | 最低日志级别 | 支持 `debug`、`info`、`warn`、`error`、`fatal`；未知值当前会回落成 `info` |
| `log.async_logging` | `true` | 是否启用异步日志写出 | `true` 时使用异步 logger；关闭后走同步路径 |
| `log.async_queue_size` | `8192` | 异步日志队列容量 | 当前配置能被解析和导出，但 `AsyncLogger::init()` 里还没有实际消费这个值 |

### 5.10 `business_log` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `business_log.enabled` | `true` | 是否启用订单业务日志 recorder | 关闭时 `OrderEventRecorder::init()` 会直接返回成功但不真正开启落盘线程 |
| `business_log.output_dir` | `"./logs"` | 业务日志输出目录 | 一定会生成 `order_events_<account_id>_<trading_day>.log`；若启用了 debug order trace 编译开关，还会额外生成 `order_debug_<account_id>_<trading_day>.log` |
| `business_log.queue_capacity` | `65536` | 业务日志 ring 队列容量 | 代码内部会实际分配 `queue_capacity + 1` 的环形缓冲；要求 `>= 2` 且 `< UINT32_MAX` |
| `business_log.flush_interval_ms` | `100` | 业务日志后台 flush 周期 | 单位毫秒；必须大于 0 |

### 5.11 `db` 段

| 配置项 | 当前值 | 含义 | 备注 |
| --- | --- | --- | --- |
| `db.db_path` | `"./data/account_service.db"` | DB 文件路径 | `enable_persistence=true` 时，账户信息、今日成交、今日委托和 fresh SHM 下的持仓初始化都可能使用它 |
| `db.enable_persistence` | `true` | 是否启用持久化 / DB 加载路径 | `true` 时 fresh SHM 的持仓初始化走 DB loader；`false` 时改为读取 `config_file + ".positions.csv"` |
| `db.sync_interval_ms` | `1000` | 持久化同步周期 | 当前配置已解析并导出，但主服务当前代码路径里还没有实际消费这个字段 |

### 5.12 当前 `default.yaml` 的运行侧重点

按当前默认值，这份配置更接近“基础账户服务模式”：

- 开启了五类 SHM 的自动创建。
- 事件循环采用忙轮询，不主动 sleep。
- 关闭行情模块、主动策略和受管执行算法。
- 风控里只保留资金/持仓/价格/重复单这些开关型检查，金额上限、数量上限、速率限制都未启用。
- 开启通用日志和业务日志。
- 开启 DB 路径，但部分 DB 相关字段目前仍是“预留/半接线”状态，需要结合当前实现理解。

## 6. 本文依据

这份说明是按以下仓库代码和文档核对出来的：

| 文件 | 用途 |
| --- | --- |
| `src/order/order_event_recorder.cpp` | `order_events_*.log` / `order_debug_*.log` 的字段布局和文本渲染方式 |
| `tools/full_chain_e2e/full_chain_observer_csv_sink.cpp` | CSV 表头、列顺序、覆盖写逻辑 |
| `tools/full_chain_e2e/full_chain_observer_position_watch.cpp` | `row_key` 和 `event_kind` 语义 |
| `include/api/order_monitor_api.h` | 订单快照结构和 `stage` 枚举 |
| `include/api/position_monitor_api.h` | 持仓头 / 资金 / 证券快照结构 |
| `docs/order_monitor_sdk.md` | `status` 状态码含义 |
| `config/default.yaml` | 当前默认配置值 |
| `src/core/config_manager.hpp` | 配置结构定义 |
| `src/core/config_manager.cpp` | 配置解析、校验和导出逻辑 |
| `src/core/account_service.cpp` | 配置在服务初始化中的实际落点 |
| `src/risk/risk_manager.hpp` | 风控配置结构定义 |
| `src/execution/execution_config.hpp` | 执行会话默认参数定义 |
| `docs/src_core_module.md` | `event_loop`、SHM 和配置装配语义 |
| `docs/src_risk_module.md` | 风控字段语义 |
| `docs/src_market_data_module.md` | 行情配置语义 |
| `docs/src_strategy_module.md` | 主动策略配置语义 |
| `docs/src_execution_module.md` | `split` 段当前语义 |
