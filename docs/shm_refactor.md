# 共享内存订单池 + 索引队列改造方案（支持外部监控，日内不删）

## Brief Summary
1. 这个方向可行，并且可以满足“另一个进程监控订单数组”的需求。
2. 方案采用一块独立的 `orders_shm` 作为日内追加式订单池，`upstream/downstream` 两条队列都只传 `order_index`，不再传 `order_request` 全量对象（当前全量对象在 `src/shm/shm_layout.hpp:77` 与 `src/shm/shm_layout.hpp:85`）。
3. 订单槽位日内不复用、不删除，日终由后处理进程接管；池满时按你的要求“拒单并告警”。
4. 交易日从配置读取（先用配置，后续可替换交易日历服务），`orders_shm` 名称按 `base + "_" + trading_day` 生成新池。

## Public APIs / Interfaces / Types Changes
1. 新增共享内存布局类型（`src/shm/shm_layout.hpp`）：
   - `using order_index_t = uint32_t;`
   - `orders_header`
   - `order_slot`
   - `orders_shm_layout`
2. 修改队列载荷类型（`src/shm/shm_layout.hpp`）：
   - `upstream_shm_layout.strategy_order_queue` 从 `spsc_queue<order_request,...>` 改为 `spsc_queue<order_index_t,...>`
   - `downstream_shm_layout.order_queue` 同步改为 `order_index_t`
3. 新增 SHM 管理接口（`src/shm/shm_manager.hpp/.cpp`）：
   - `orders_shm_layout* open_orders(std::string_view name, shm_mode mode, account_id_t account_id);`
4. 配置项新增（`src/core/config_manager.hpp/.cpp`）：
   - `shm.orders_shm_name`（默认 `/orders_shm`）
   - `trading_day`（`YYYYMMDD`，必填校验）
   - CLI 增加 `--orders-shm`、`--trading-day`
5. Gateway 配置与参数新增（`gateway/src/gateway_config.hpp/.cpp`）：
   - `orders_shm_name`
   - `trading_day`
   - CLI 增加 `--orders-shm`、`--trading-day`
6. C API 扩展（`include/api/order_api.h`、`src/api/order_api.cpp`）：
   - 新增 `acct_init_ex(const acct_init_options_t* opts, acct_ctx_t* out_ctx)`
   - `acct_init` 保留为兼容包装（默认值路径），推荐策略侧改用 `acct_init_ex` 显式传 `trading_day`
7. `order_entry` 扩展（`src/order/order_book.hpp`）：
   - 增加 `order_index_t shm_order_index`，作为 `internal_order_id -> order_slot` 的稳定映射

## Implementation Spec (Decision Complete)

### 1) 订单池模型与一致性协议
1. `orders_shm_layout` 使用固定容量 `kDailyOrderPoolCapacity = kMaxActiveOrders`（`src/common/constants.hpp` 新增常量），每日单独一块池。
2. 分配策略为“单调递增索引，不复用”：
   - `orders_header.next_index` 用 CAS 递增保序分配。
   - `index in [0, next_index)` 视为当日已发布槽位集合。
3. 每个 `order_slot` 使用 seqlock 读写协议：
   - 写：`seq` 置奇数 -> 写 payload/meta -> `seq` 置偶数（release）
   - 读：两次读取偶数且相等才接受快照（acquire）
4. 槽位阶段 `stage` 明确状态机：
   - `Reserved -> UpstreamQueued -> UpstreamDequeued -> (RiskRejected | DownstreamQueued)`
   - `DownstreamQueued -> DownstreamDequeued -> Terminal`
   - 任意阶段可转 `QueuePushFailed`
5. 订单池永不日内删除；`archive_order` 只改阶段与状态，不清槽。

### 2) 链路改造（上游、核心、下游）
1. 策略/API 写入流程（`src/api/order_api.cpp`）：
   - 先分配 `orders_shm` 槽位并写 `order_request`
   - 再把 `order_index_t` 推入 `upstream` 队列
   - 若队列推送失败：槽位标记 `QueuePushFailed`，API 返回队列满
   - 若池满：直接拒单并告警，返回新错误码（建议 `OrderPoolFull`）
2. 账户服务上游消费（`src/core/event_loop.cpp`）：
   - 从 `upstream` 队列 pop `order_index_t`
   - 到 `orders_shm` 读取对应 `order_request`
   - 建立 `order_entry.shm_order_index = index`
3. 路由与拆单（`src/order/order_router.hpp/.cpp`）：
   - `send_to_downstream` 改为发送 `order_index_t`
   - 拆单/子撤单由账户服务在 `orders_shm` 新分配槽位（source=内部生成），再推下游索引
4. Gateway 消费（`gateway/src/gateway_loop.cpp`）：
   - 从下游队列 pop `order_index_t`
   - 从 `orders_shm` 读订单快照后做 `map_order_request_to_broker`
5. 成交回报更新（`src/core/event_loop.cpp` + `src/order/order_book.*`）：
   - 通过 `internal_order_id -> shm_order_index` 映射回写 `orders_shm`
   - `order_status`、成交字段、时间戳与 `stage` 一并更新

### 3) 订单簿与订单池同步机制
1. 在 `order_book` 增加“变更回调”挂钩（`add_order/update_status/update_trade/archive_order/refresh_parent_from_children` 全触发）。
2. 回调统一写入 `orders_shm`，避免在多处手工同步造成漏更。
3. 回调写入失败只记录错误，不影响订单主状态机推进（可观测性与交易路径解耦）。

### 4) 日切与命名策略
1. `orders_shm` 运行时名称固定为：`orders_shm_name + "_" + trading_day`。
2. `trading_day` 必须来自配置；账户服务、gateway、策略 API 三端一致。
3. 每日切换流程：
   - 停机协同切换（你已确认）
   - 新交易日启动新 `orders_shm_YYYYMMDD`
   - 旧 `orders_shm_YYYYMMDD` 留给后处理进程
   - 后处理完成后再 `shm_unlink`
4. `upstream/downstream/trades/positions` 仍按现有名称管理，但切换窗口执行清理并重建，避免旧队列残留索引。

### 5) 池满策略与可观测性
1. 池满统一策略：拒单并告警（你已确认）。
2. 新增计数器：
   - `orders_header.full_reject_count`
   - `orders_header.next_index`（可直接算利用率）
3. 新增告警阈值：
   - 利用率 >= 80% 告警
   - 利用率 >= 95% 严重告警
4. 新增错误码（建议）：
   - `error_code::OrderPoolFull`
   - C API 对应 `ACCT_ERR_ORDER_POOL_FULL`

## File-Level Change List
1. `src/shm/shm_layout.hpp`：新增 `orders_*` 结构；上游/下游队列元素改为索引。
2. `src/shm/shm_manager.hpp` 与 `src/shm/shm_manager.cpp`：新增 `open_orders` 与 header 校验。
3. `src/common/constants.hpp`：新增 `kDailyOrderPoolCapacity`、`kOrdersShmName`。
4. `src/core/config_manager.hpp` 与 `src/core/config_manager.cpp`：新增 `orders_shm_name`/`trading_day` 解析、校验、导出、CLI。
5. `src/core/account_service.hpp` 与 `src/core/account_service.cpp`：新增 `orders_shm_manager_` 与 `orders_shm_` 初始化、清理、依赖注入。
6. `src/core/event_loop.hpp` 与 `src/core/event_loop.cpp`：上游读取索引、读取订单池、更新订单池。
7. `src/order/order_book.hpp` 与 `src/order/order_book.cpp`：`shm_order_index` 字段与变更回调。
8. `src/order/order_router.hpp` 与 `src/order/order_router.cpp`：下游发送索引、内部生成订单写池。
9. `gateway/src/gateway_config.hpp` 与 `gateway/src/gateway_config.cpp`：新增参数。
10. `gateway/src/main.cpp` 与 `gateway/src/gateway_loop.hpp` 与 `gateway/src/gateway_loop.cpp`：打开订单池并按索引消费。
11. `include/api/order_api.h` 与 `src/api/order_api.cpp`：`acct_init_ex`、策略侧写池+推索引。
12. `docs/order_flowchart.md` 与 `docs/order_architecture.mmd` 与 `gateway/docs/gateway_design.md`：更新数据流与监控协议。
13. `test/test_shm_manager.cpp`、`test/test_order_api.cpp`、`test/test_event_loop.cpp`、`test/test_gateway_loop.cpp`、`test/test_config_manager.cpp`：补充与改造测试。

## Test Cases and Scenarios
1. `orders_shm` 分配顺序测试：并发分配无重复索引，`next_index` 单调递增。
2. seqlock 一致性测试：监控读线程在高频更新下不读到撕裂数据。
3. API 提交流程测试：`submit/cancel/new+send` 均写入订单池并只推索引。
4. 上游消费测试：`event_loop` 能从索引恢复完整订单并正确进入风控/路由。
5. 下游消费测试：`gateway_loop` 能从索引恢复订单并提交适配器。
6. 拆单与子撤单测试：内部生成订单获得新索引并被监控可见。
7. 回报更新测试：成交回报后 `orders_shm` 中订单状态与成交字段同步更新。
8. 池满测试：触发拒单、错误码正确、计数器递增、日志告警触发。
9. 日切测试：`trading_day` 改变后新建新池；旧池仍可被后处理进程读取。
10. 回归测试：现有 `risk/order_book/router/event_loop/gateway` 语义不回退。

## Assumptions and Defaults
1. 监控语义：只要求“活跃快照可见”，不要求事件流 Exactly-once。
2. 覆盖范围：监控池覆盖所有提交单（含风控拒绝、路由失败、成交终态）。
3. 生命周期：日内不删除不复用；日终由外部进程处理历史池。
4. 池满行为：拒单并告警，不阻塞主循环，不回退旧路径。
5. 交易日来源：先走配置 `trading_day`，并在代码与文档标注后续切到交易日历服务。
6. 发布方式：停机协同切换，新旧 layout 不做在线双兼容。
