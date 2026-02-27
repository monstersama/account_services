# 全链路可观测 E2E 方案（真实多进程）

## 摘要
目标是交付一套可重复执行的“下单 -> 风控 -> 下游 -> 成交回报 -> 订单状态更新 -> 持仓更新”全链路测试，并且实时可见订单与持仓变化，且落盘 CSV 便于复盘。  
采用你确认的方案：`自动化E2E` + `终端实时输出+CSV` + `新增持仓监控对外API与工具` + `真实多进程拓扑`。

## 关键现状与必修修复
当前存在一个会影响全链路结果的缺口：`order_api` 提交订单时 `internal_security_id=0`，而成交回报后的持仓更新逻辑要求 `security_id!=0` 才执行。  
必须先修复该映射链路，否则“有成交但持仓不变”。

## 方案范围

1. 修复证券 ID 映射（保证持仓可更新）
- 修改 `src/core/event_loop.cpp` 的 `handle_order_request`。
- 在订单进入风控/路由前，若 `request.internal_security_id==0` 且 `security_id` 非空：
  - 先 `positions_.find_security_id(code)`；
  - 不存在则 `positions_.add_security(code, code, market)`；
  - 将得到的 `security_id` 写回 `request.internal_security_id`；
  - 同步更新 `orders_shm` 快照，保证监控可见一致值。
- 若映射失败，订单进入失败路径并记录错误（不中断主循环）。

2. 新增持仓监控公共 API（对外 C ABI）
- 新增 `include/api/position_monitor_api.h`。
- 新增 `src/api/position_monitor_api.cpp`，并在 `src/api/CMakeLists.txt` 新增 `libacct_position_monitor.so`。
- API 设计（稳定 C 接口）：
  - `acct_positions_mon_open / close`
  - `acct_positions_mon_info`
  - `acct_positions_mon_read_fund`
  - `acct_positions_mon_read_position(index)`
  - `acct_positions_mon_strerror`
- 默认读取 `"/positions_shm"`，只读 mmap。
- 提供 `info + row snapshot` 语义，供外部监控进程实时轮询。

3. 新增实时观测工具（终端+CSV）
- 新增 `examples/full_chain_observer.cpp`（单工具同时观测订单与持仓）。
- 工具输入参数：
  - 订单池名、交易日、持仓 shm 名、轮询间隔、输出目录、超时。
- 工具输出：
  - 终端实时事件行（订单状态变化、持仓变化）。
  - `orders_events.csv`（每次 seq 变化落一行）。
  - `positions_events.csv`（header/资金行/证券行变化落一行）。
- 订单观测逻辑：维护 `index->last_seq`，同一槽位有更新就输出。
- 持仓观测逻辑：基于 `positions_header.last_update` + 行快照变化检测输出。

4. 新增下单 CLI（用于真实多进程 E2E 驱动）
- 新增 `examples/order_submit_cli.cpp`。
- 支持传入 `upstream_shm/orders_shm/trading_day/security/side/market/volume/price`，调用 `acct_init_ex + acct_submit_order`。
- 输出 `order_id` 到 stdout，供脚本与测试断言引用。

5. 新增真实多进程 E2E 运行器（自动化）
- 新增 `test/full_chain_e2e.sh`（或同等 runner）。
- 运行器职责：
  - 生成唯一 run_id 与临时配置（唯一 shm 名，避免脏数据冲突）。
  - 启动 `acct_service_main`、`acct_broker_gateway_main`、`full_chain_observer`。
  - 调用 `order_submit_cli` 发单。
  - 轮询 CSV 直到满足完成条件或超时。
  - 收集并输出 artifacts 目录，失败时打印关键日志片段。
- 在 `test/CMakeLists.txt` 注册 `ctest` 用例 `test_full_chain_e2e`。

6. 文档补充
- 新增 `docs/full_chain_e2e.md`：
  - 一键运行命令
  - 终端输出示例
  - CSV 字段解释
  - 常见失败排查（交易日不一致、shm 名不一致、进程未就绪）

## 公共接口/类型变更
- 新增公共头文件：`include/api/position_monitor_api.h`。
- 新增共享库：`libacct_position_monitor.so`。
- `order_monitor_api` 保持兼容，不改现有 ABI。
- 内部行为变化：`event_loop` 在新单入口补齐 `internal_security_id`（不改外部 API 签名）。

## 自动化测试与验收

1. 单元测试
- 新增 `test/test_position_monitor_api.cpp`：
  - `open/info/read/invalid_param/not_found/retry`。
- 新增/扩展 `test/test_event_loop.cpp`：
  - 验证 `internal_security_id` 自动解析与回写。
  - 验证成交后持仓确实更新。

2. 集成测试（真实多进程）
- `ctest -R test_full_chain_e2e --output-on-failure` 必须通过。
- 必须验证：
  - 订单经历关键状态并终态完成（`Finished`）。
  - `orders_events.csv` 中目标订单 `volume_traded>0` 且 `volume_remain==0`。
  - `positions_events.csv` 中目标证券 `volume_buy_traded>0`，资金行变化可见。
  - 运行过程中终端有实时输出。

3. 回归要求
- 现有核心测试继续通过：`test_account_service`、`test_event_loop`、`test_gateway_loop`、`test_order_monitor_api`、`test_position_manager`。

## 交付物
- 可执行工具：`full_chain_observer`、`order_submit_cli`。
- 公共库：`libacct_position_monitor.so`。
- 自动化用例：`test_full_chain_e2e`。
- 产物目录（每次运行）：
  - `orders_events.csv`
  - `positions_events.csv`
  - `account_service.stdout.log`
  - `gateway.stdout.log`
  - `account_1001.log`（原日志系统）

## 假设与默认值
- 默认环境为 Linux + POSIX SHM。
- 网关使用 `sim` 适配器且 `auto_fill=true`，保证可稳定收到成交回报。
- E2E 主场景先覆盖“买入全成”（最稳定），后续可扩展到撤单与卖出。
- 以唯一 shm 名隔离每次测试，不依赖全局清理。
- 一致性语义为“监控快照可见”，不承诺跨多行强事务快照。
