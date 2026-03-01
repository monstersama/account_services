# Position Loader 切换为真实 DB 加载（SQLite）并按配置初始化对象（重载）

## Summary
将 `position_loader` 从“伪 DB（CSV）”改为“真实 SQLite DB”加载，且在 `position_manager` 内根据 `db.enable_persistence` 选择不同的 loader 初始化对象（通过构造重载区分 DB/File 模式）。  
同时保持现有重启语义不变：`init_state == 1` 时直接从 SHM 恢复，不再触发 loader。

## 目标与成功标准
- `db.enable_persistence=true` 时：
  - 读取 SQLite 的 `account_info`（FUND 行来源）和 `positions`（证券持仓来源）。
  - 不再读取 `*.positions.csv`。
- `db.enable_persistence=false` 时：
  - 使用文件 loader（`config_file + ".positions.csv"`）加载。
- 当日重启（SHM 已初始化）：
  - 完全忽略 DB/File，直接用 SHM 数据重建索引。
- 所有现有测试 + 新增 DB 路径测试通过。

## 约束与确定的决策
- DB 引擎：SQLite（文件路径来自 `db.db_path`）。
- `positions`：按你的要求“全表加载”（单账户进程，不按账户过滤 positions）。
- FUND：从 `account_info` 按配置账户加载。
- loader 选择规则：`db.enable_persistence=true` 初始化 DB loader；否则初始化 File loader（不自动回退）。
- `account_info` 目标字段类型：`INTEGER`（分值，`uint64` 等价）。
- `account_info.account_id`：`INTEGER`（`uint32` 等价）并按数值精确匹配。

## 需要落地的接口/类型变更
- `position_loader` 改为“模式化对象 + 构造重载”：
  - `position_loader(std::string config_file_path)`：File 模式
  - `position_loader(std::string db_path)`：DB 模式
  - `bool load(account_id_t account_id, position_manager& manager)`：按内部模式分发
- `position_manager` 保持“内部创建 loader”：
  - 在 fresh SHM 分支里按 `db_enabled_` 选择构造重载：
    - `db_enabled_ == true` -> `position_loader(db_path_)`
    - `db_enabled_ == false` -> `position_loader(config_file_path_)`

## DB 加载实现细节（decision-complete）
1. SQLite 依赖
- `src/CMakeLists.txt` 新增 `find_package(SQLite3 REQUIRED)`。
- `acct_portfolio` 链接 `SQLite::SQLite3`（若 target 不可用则退回 `${SQLite3_LIBRARIES}` + include dirs）。

2. `position_loader` 内部结构
- 保留一个内部 `enum class source_type { File, Db };`。
- 两个构造重载分别设置 `source_type_` 与路径成员。
- `load()` 内部调用：
  - `load_from_db(account_id, manager)` 或
  - `load_from_file(manager)`。

3. DB 资源管理（优雅实现）
- 在 `position_loader.cpp` 用 RAII 封装：
  - `sqlite3*` -> `unique_ptr` + custom deleter（`sqlite3_close`）。
  - `sqlite3_stmt*` -> `unique_ptr` + custom deleter（`sqlite3_finalize`）。
- 所有 SQL 失败路径统一记录日志并返回 `false`。

4. SQL 查询与映射
- 账户查询（FUND）：
  - `SELECT total_assets, available_cash, frozen_cash, position_value FROM account_info WHERE account_id = ? LIMIT 1;`
  - `?` 绑定为 `uint32 account_id`。
  - 未查到记录 -> 返回 `false`（初始化失败）。
- 持仓查询（全表）：
  - `SELECT security_id, internal_security_id, volume_available_t0, volume_available_t1, volume_buy, dvalue_buy, volume_buy_traded, dvalue_buy_traded, volume_sell, dvalue_sell, volume_sell_traded, dvalue_sell_traded, count_order FROM positions ORDER BY ID ASC;`
- FUND 映射：
  - `total_assets -> fund.total_asset`
  - `available_cash -> fund.available`
  - `frozen_cash -> fund.frozen`
  - `position_value -> fund.market_value`
- 证券行映射：
  - 通过 `internal_security_id` 解析 market+code，调用 `add_security(code, name, market)`。
  - `name` 使用 `internal_security_id`（当前 schema 无 name 字段）。
  - 其余字段按列直接赋值到 `position` 对应字段。
- 数据合法性：
  - `internal_security_id` 格式非法、数值列越界/空值 -> 失败返回 `false`。
  - 重复 `internal_security_id`：按 `ORDER BY ID ASC` 顺序覆盖，最后一条生效（确定性）。

5. File loader 行为
- 继续读取 `config_file + ".positions.csv"`，沿用当前 CSV schema。
- 仅在 `db.enable_persistence=false` 时会被初始化并执行。

## account_service / position_manager 变更点
- `account_service` 不参与具体 loader 逻辑，只传 `config_file/db_path/db_enabled` 给 `position_manager`（现状保持）。
- `position_manager::initialize` fresh 分支中：
  - 去掉“DB 优先再文件回退”策略。
  - 改为“按 `db_enabled_` 选择 loader 初始化对象（构造重载）”。

## 测试计划
1. `test_position_manager`
- 保留现有 fresh/restart 语义测试。
- 新增 DB 模式单测（可放同文件）：
  - 创建临时 sqlite 文件。
  - 建表 `account_info/positions` 并插入样例。
  - `db_enabled=true` 初始化，断言 FUND 与证券行加载正确。
- 新增 DB 错误路径：
  - `account_info` 无匹配 `account_id` -> 初始化失败，`init_state` 保持 0。
  - `positions.internal_security_id` 非法 -> 初始化失败。

2. `test_account_service`
- 现有 `position_loader_only_on_fresh_shm` 拆分为两条：
  - File 模式（`db.enable_persistence=false`）验证 file loader 生效。
  - DB 模式（`db.enable_persistence=true`）验证 DB loader 生效与重启不重载（修改 DB 后第二次启动仍沿用 SHM）。

3. 回归
- `test_risk_manager` 保证无行为回归。
- 全部相关 target 编译通过。

## Schema 与迁移前提（明确假设）
- `account_info` 将按你最新要求使用：
  - `account_id INTEGER UNIQUE NOT NULL`
  - `total_assets/available_cash/frozen_cash/position_value INTEGER NOT NULL`
- `positions` 使用你提供结构（全表加载，不加 account 过滤）。
- 本次代码不做自动迁移脚本；假设运行环境 schema 已按上述约定准备好。
- 若实际 schema 不一致，loader 会报错并返回初始化失败（fail-fast）。

## 实施文件清单
- `src/portfolio/position_loader.hpp/.cpp`
- `src/portfolio/position_manager.cpp`
- `src/CMakeLists.txt`
- `test/test_position_manager.cpp`
- `test/test_account_service.cpp`
