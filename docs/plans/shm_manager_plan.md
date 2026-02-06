# 共享内存审查与修复建议计划（仅 `shm_manager.cpp`）

## 简要结论 / 要输出的缺陷清单（含修复方向）
- **尺寸未校验导致潜在 SIGBUS**：`mmap` 前没有验证已有 shm 对象大小是否等于期望布局大小，若更小会在访问时报错。修复：`fstat` 校验，大小不匹配直接失败。对应 `src/shm/shm_manager.cpp:81` 与 `src/shm/shm_manager.cpp:100`。
- **OpenOrCreate 初始化存在竞态与误判**：用 `st_size==0` 与 `next_order_id==0` 识别“新建”，可能并发初始化或误重置已有内存。修复：OpenOrCreate 使用 `O_CREAT|O_EXCL` 先尝试创建；在 `open_*` 中用“是否新建”的明确标记而非数据字段判断。对应 `src/shm/shm_manager.cpp:67`、`src/shm/shm_manager.cpp:154`、`src/shm/shm_manager.cpp:181`、`src/shm/shm_manager.cpp:207`、`src/shm/shm_manager.cpp:233`。
- **创建失败后遗留损坏对象**：`ftruncate/mmap` 失败时未 `shm_unlink`，可能留下 0 字节对象，后续再次 OpenOrCreate 会异常。修复：若本次创建为新对象且失败，执行 `shm_unlink` 清理。对应 `src/shm/shm_manager.cpp:90` 与 `src/shm/shm_manager.cpp:100`。
- **FD 泄漏风险（exec 继承）**：未设置 `O_CLOEXEC`/`FD_CLOEXEC`。修复：在 `shm_open` flags 增加 `O_CLOEXEC` 或调用 `fcntl` 设置。对应 `src/shm/shm_manager.cpp:74`。
- **positions 重初始化未清理数据**：当 `magic` 异常时直接写 header，可能保留旧 fund/positions。修复：仅在“明确新建”时初始化，异常直接报错（不重置）。对应 `src/shm/shm_manager.cpp:233`。

## 实施步骤（明确到改动点）
1. **改造 OpenOrCreate 的创建/打开逻辑**  
   在 `open_impl` 中改为：  
   - `Create`：使用 `O_CREAT|O_EXCL|O_RDWR`（可加 `O_CLOEXEC`），失败直接报错。  
   - `Open`：使用 `O_RDWR`（可加 `O_CLOEXEC`）。  
   - `OpenOrCreate`：先 `O_CREAT|O_EXCL|O_RDWR` 尝试创建；若 `errno==EEXIST` 再以 `O_RDWR` 打开；用布尔值记录“是否新建”。  
   位置：`src/shm/shm_manager.cpp:48`。

2. **加入大小校验与错误清理**  
   - `fstat` 获取 `st_size`，若非新建且 `st_size != size` 直接失败（避免 SIGBUS）。  
   - 新建时 `ftruncate` 失败或 `mmap` 失败，若本次新建则 `shm_unlink` 清理。  
   位置：`src/shm/shm_manager.cpp:81` 至 `src/shm/shm_manager.cpp:109`。

3. **避免用业务字段判断“新建”**  
   - 在类内新增 `bool last_open_is_new_`（私有成员），由 `open_impl` 设置，`open_*` 仅依赖该标记初始化，否则严格 `validate_header`。  
   - `positions` 分支在非新建时只做校验，不再“魔数异常就重置”。  
   位置：`src/shm/shm_manager.cpp:142` 至 `src/shm/shm_manager.cpp:260`，以及私有成员新增在 `src/shm/shm_manager.hpp:70`。

4. **补充 close 后状态归零**  
   - `close()` 内把 `last_open_is_new_` 复位为 `false`，避免误用上一次状态。  
   位置：`src/shm/shm_manager.cpp:264`。

## 对外 API / 类型影响
- **公共 API 不变**。  
- **内部实现变更**：新增私有成员 `last_open_is_new_`，位于 `src/shm/shm_manager.hpp:70`；行为上更严格（size 不匹配直接失败，不会“重初始化已有内存”）。

## 测试用例与场景
- **正确创建与打开**：Create → Open，验证 header 正常且不会被二次初始化。  
- **尺寸不匹配**：手动创建较小 shm 后用 `open_*` 打开，应返回 `nullptr`。  
- **OpenOrCreate 竞争**：模拟另一个进程已创建但未初始化的场景，确保不会误判重置（需要在测试中显式区分“新建”标记）。  
- **失败清理**：模拟 `ftruncate/mmap` 失败后确保对象被 `shm_unlink` 清理（可用 mock 或在受控环境下验证）。  
说明：测试新增可放入现有 CTest 体系，需在 `test/CMakeLists.txt:1` 增加新的测试可执行文件。

## 明确假设
- 运行环境为 POSIX（Linux），`shm_open`/`ftruncate`/`mmap` 语义符合标准。
- `shm_manager` 实例不会被多线程并发调用同一对象的方法（类本身不提供线程安全）。
- 允许改动 `src/shm/shm_manager.hpp:70` 增加私有成员以携带“是否新建”状态。
