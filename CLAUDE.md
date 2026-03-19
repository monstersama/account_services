# 仓库指南

## 项目结构与模块组织
仓库可按“核心服务、网关与接口、测试与工具、配置与文档”四组理解：

### 核心服务
- `src/core/`：服务生命周期、配置加载、事件循环和主流程编排。
- `src/order/`：订单簿、路由、拆单、撤单与恢复逻辑。
- `src/portfolio/`：账户、持仓、成交和委托记录管理。
- `src/risk/`：风控校验器与风控管理逻辑。
- `src/shm/`：共享内存布局与访问管理。
- `src/common/`：日志、错误、时间工具和基础并发组件。

### 网关与接口
- `gateway/src/`：网关配置、适配器加载、报单映射、回报映射和网关事件循环。
- `src/api/`：订单接口、订单监控、持仓监控等 API 实现。
- `src/broker_api/`：券商适配器共享库实现。
- `include/api/`：稳定的对外 C 接口头文件。
- `include/broker_api/`：券商插件或适配器使用的 C++ 头文件。

### 测试与工具
- `test/`：单元测试、组件测试和端到端脚本入口。
- `tools/full_chain_e2e/`：全链路 observer、CSV 落盘与发单 CLI 工具。
- `third_party/`：按需拉取的第三方库源码目录；查看第三方库实现、接口或构建脚本时，统一从这里获取。

### 配置与文档
- `config/`：主服务、网关和 observer 的 YAML 配置。
- `docs/`：命名规范、流程图、接口约定和设计方案文档。

## 构建、测试与开发命令
### 构建
- `CC=clang CXX=clang++ cmake -S . -B build`：使用 Clang/Clang++ 生成默认构建目录。
- `cmake --build build -j4`：编译所有库、可执行文件、网关目标和测试。
- `cmake --build build -j4 --target test_event_loop test_account_service`：仅重编译当前关注的测试目标。

### 测试
- `ctest --test-dir build --output-on-failure`：运行完整 CTest 测试集。
- `./build/test/test_order_api` 或 `./build/test/test_gateway_loop`：直接运行单个测试二进制。
- `test/full_chain_e2e.sh build`：执行全链路端到端验证。

### 本地运行
- `./build/src/acct_service_main --config config/default.yaml`：使用默认配置启动主服务。

### OrbStack / 虚拟机约束
- 适用前提：仅当当前工作流明确使用 OrbStack，或用户要求在 OrbStack 虚拟机内执行命令时，以下虚拟机约束才适用；如果当前不在 OrbStack 环境中，则不需要遵循这些虚拟机约束，可直接在当前环境执行相关命令。
- 适用范围：在使用 OrbStack 的前提下，凡是涉及编译、测试、启动服务、执行二进制或生成构建产物的命令，均应在 OrbStack 虚拟机内执行。
- 文件操作：查看源码、搜索文件、阅读文档、修改仓库内文件等纯文件操作，不要求进入虚拟机，可直接在当前工作区进行。
- 环境判断：优先使用 `uname -s` 判断当前是否已经在 Linux 环境；如需进一步确认发行版，可查看 `/etc/os-release`。不要通过 `orb` 或 `orbctl` 是否存在来反推当前是否为 Linux，因为这只能说明 OrbStack CLI 是否已安装。
- 推荐流程：
  1. 先执行 `uname -s`；若返回 `Linux`，说明当前已经在 Linux 环境，可直接执行后续编译、测试或运行命令，无需遵循 OrbStack 虚拟机流程。
  2. 若 `uname -s` 不是 `Linux`，且当前工作流明确要求使用 OrbStack，再使用 `orbctl list` 或其他 `orbctl` 查询命令检查 `ubuntu` 虚拟机的 `state`。
  3. 若 `state` 为 `running`，则直接执行后续编译、测试或运行命令，不需要执行 `orbctl start ubuntu`。
  4. 若 `state` 不是 `running`，再按需执行 `orbctl start ubuntu`；若相关命令执行失败需要唤醒实例，或用户明确要求启动，也可执行该命令。
  5. 进入虚拟机后，仓库目录应使用 Linux 路径 `/home/ythe/repositories/account_services`。
  6. 未进入虚拟机时，可优先使用 `orb -m ubuntu -u ythe -w /home/ythe/repositories/account_services <command>` 在虚拟机内单条执行命令。
  7. 仓库默认使用 `clang/clang++` 完成 CMake 配置；若从 GCC 切换到 Clang，先删除旧的 `build/` 目录或使用新的构建目录后再重新执行配置命令，避免复用旧编译器缓存。
- 补充约定：如需补充具体进入方式或执行包装命令，应优先复用仓库既有脚本或用户提供的 OrbStack 命令约定。
- 常用命令示例：
  - 快速判断当前是否在 Linux：`uname -s`
  - 进一步确认当前发行版：`cat /etc/os-release`
  - 检查虚拟机状态：`orbctl list`
  - 按需启动虚拟机：`orbctl start ubuntu`
  - 进入虚拟机：`orb -m ubuntu -u ythe`
  - 在虚拟机内进入仓库：`cd /home/ythe/repositories/account_services`
  - 未进入虚拟机时直接生成构建目录：`orb -m ubuntu -u ythe -w /home/ythe/repositories/account_services env CC=clang CXX=clang++ cmake -S . -B build`
  - 未进入虚拟机时直接编译：`orb -m ubuntu -u ythe -w /home/ythe/repositories/account_services cmake --build build -j4`
  - 未进入虚拟机时直接运行测试：`orb -m ubuntu -u ythe -w /home/ythe/repositories/account_services ctest --test-dir build --output-on-failure`
  - 未进入虚拟机时直接启动主服务：`orb -m ubuntu -u ythe -w /home/ythe/repositories/account_services ./build/src/acct_service_main --config config/default.yaml`
  - 已进入虚拟机后的常用命令：`CC=clang CXX=clang++ cmake -S . -B build`、`cmake --build build -j4`、`ctest --test-dir build --output-on-failure`、`./build/src/acct_service_main --config config/default.yaml`

## 编码风格与命名规范
使用 C++23，并保持仓库现有风格：4 空格缩进，左花括号与声明同行，单个源文件职责尽量聚焦。命名遵循 `docs/cpp_naming_baseline.md`：命名空间和函数使用 `snake_case`，类型使用 `PascalCase`，成员变量使用带尾下划线的 `snake_case_`，常量使用 `kXxx`。C++ 头文件优先使用 `.hpp`，仅对 C 兼容接口保留 `.h`。默认使用 Clang/Clang++ 进行本仓库的 CMake 配置与构建；修改 C/C++ 文件后，建议使用 `clang-format -i src/foo.cpp include/foo.hpp` 统一格式；如无额外样式文件，请至少保持与相邻代码一致。提交前确保代码可在 `-Wall -Wextra -Wpedantic` 下无新增告警。

## 测试规范
### 新增测试
- 新测试文件建议命名为 `test_*.cpp`。
- 在 `test/CMakeLists.txt` 中补充目标和 `add_test(...)` 注册。

### 执行顺序
- 开发阶段先运行受影响模块的单测或组件测试。
- 提交前执行 `ctest --test-dir build --output-on-failure` 做完整回归。

### 额外验证
- 若改动涉及网关链路、共享内存或 observer CSV 输出，请额外运行 `test/full_chain_e2e.sh`。

## 提交与合并请求规范
### 提交信息
- 使用 Conventional Commit 前缀，如 `feat:`、`fix:`、`refactor:`。
- 摘要保持简短、直接，建议使用祈使句，例如 `fix: tighten risk check logging`。

### Pull Request
- 说明本次改动带来的行为变化。
- 列出受影响的配置项、共享内存路径或接口。
- 关联对应 issue，并附上实际执行过的验证命令。
- 若输出文件格式、observer CSV 或端到端结果发生变化，附上示例片段或截图。

## 配置与运行说明
本地运行优先复用 `config/` 下已提交的 YAML 配置。端到端脚本默认依赖 `/dev/shm` 下的共享内存路径，并可能在 `build/e2e_artifacts/` 中生成运行产物；请勿提交生成的日志、数据库文件或构建目录。

## 第三方库约束
- 第三方库源码、构建脚本和相关实现统一从 `third_party/` 目录查看；如本地缺失，先执行 `tools/bootstrap_third_party.sh` 拉取后再分析。
- 禁止直接修改 `third_party/` 下第三方库的内容，也不要将这类改动作为本仓库日常开发的一部分提交。
- 若确认必须调整第三方库行为，优先通过向对应第三方仓库提交 issue 或上游修复的方式推进，并在本仓库代码中尽量采用兼容性绕行方案。
