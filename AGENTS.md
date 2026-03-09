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

### 配置与文档
- `config/`：主服务、网关和 observer 的 YAML 配置。
- `docs/`：命名规范、流程图、接口约定和设计方案文档。

## 构建、测试与开发命令
### 构建
- `cmake -S . -B build`：生成默认构建目录。
- `cmake --build build -j4`：编译所有库、可执行文件、网关目标和测试。
- `cmake --build build -j4 --target test_event_loop test_account_service`：仅重编译当前关注的测试目标。

### 测试
- `ctest --test-dir build --output-on-failure`：运行完整 CTest 测试集。
- `./build/test/test_order_api` 或 `./build/test/test_gateway_loop`：直接运行单个测试二进制。
- `test/full_chain_e2e.sh build`：执行全链路端到端验证。

### 本地运行
- `./build/src/acct_service_main --config config/default.yaml`：使用默认配置启动主服务。

## 编码风格与命名规范
使用 C++20，并保持仓库现有风格：4 空格缩进，左花括号与声明同行，单个源文件职责尽量聚焦。命名遵循 `docs/cpp_naming_baseline.md`：命名空间和函数使用 `snake_case`，类型使用 `PascalCase`，成员变量使用带尾下划线的 `snake_case_`，常量使用 `kXxx`。C++ 头文件优先使用 `.hpp`，仅对 C 兼容接口保留 `.h`。修改 C/C++ 文件后，建议使用 `clang-format -i src/foo.cpp include/foo.hpp` 统一格式；如无额外样式文件，请至少保持与相邻代码一致。提交前确保代码可在 `-Wall -Wextra -Wpedantic` 下无新增告警。

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
