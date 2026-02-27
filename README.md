## 账户服务进程

高性能低延时的量化交易系统的账户服务进程

### 主要功能

接收上游策略用户发出到共享内存中的order，读取后将其按需经过风控模块的验证，然后拆单或直接发送订单到下游的交易进程的共享内存中。
需要负责维护上游订单状态，更新该订单在共享内存中的状态。
初始化时加载账户信息、持仓信息、当日成交信息、当日委托信息，并初始化账户状态。
从下游交易进程获取成交回报，并更新当前维护的订单状态。


### 模块介绍

portfolio模块负责账户信息管理，账户状态管理，账户持仓管理，账户当日委托管理，账户当日成交管理，账户当日持仓管理。

risk模块负责账户风控管理，账户风控检查，账户风控更新。

shmm负责共享内存的创建

order模块负责接收订单，并做拆单，将订单写入 `orders_shm` 后把索引发送给交易进程。

gateway模块负责读取 `downstream_shm` 中的索引，从 `orders_shm` 读取订单快照，调用券商适配器发送委托，并将成交回报写回 `trades_shm`。

### 流程图

订单处理完整流程图：[docs/order_flowchart.md](docs/order_flowchart.md)

该流程图展示了订单从策略提交到完成的全生命周期，包括：
- 订单完整生命周期流程图
- 状态转换图（基于`order_status_t`枚举）
- 组件交互图（系统架构）
- 错误处理流程图

详细说明和Mermaid源代码请查看文档。

## Gateway 与 Broker API

- Gateway 可执行程序：`acct_broker_gateway_main`
- 设计文档：`gateway/docs/gateway_design.md`
- 时序图：`gateway/docs/gateway_sequence.mmd`
- 券商适配接口契约：`docs/broker_api_contract.md`
- 插件模式：在 `config/gateway.yaml` 中设置 `broker_type: "plugin"` 和 `adapter_so: "<adapter.so>"`
- Gateway 配置文件示例：`config/gateway.yaml`
- 配置文件启动示例：`./build/gateway/acct_broker_gateway_main --config ./config/gateway.yaml`

## 监控 SDK

- 监控 SDK 文档：`docs/order_monitor_sdk.md`
- 监控示例程序：`examples/order_monitor_usage.cpp`

## 待处理事项（工程化）

以下事项来自当前项目结构与实现评估，作为后续迭代清单：

- [ ] P1：统一配置入口与参数语义  
  `acct_service_main` 当前只接收 `--config`，而 `config_manager` 内已有更多 CLI 覆盖项但未接入主入口，需要统一为单一、可预期的配置模型。

- [ ] P1：收敛进程级信号处理副作用  
  `event_loop` 内部直接注册信号处理并依赖全局活动指针，建议将信号注册下沉到进程入口层，循环层只保留可控停止接口。

- [ ] P2：收敛全局头文件注入  
  顶层 `include_directories(...)` 为全局生效，建议逐步改为各 target 显式声明 `target_include_directories(...)`，强化模块边界。

- [ ] P2：修正文档与实现漂移  
  现有部分计划文档仍描述 C++20/TOML 等旧信息，需与当前 C++23/YAML 实现保持一致，避免误导。

- [ ] P2：强化 `trading_day` 配置治理  
  多处默认 `19700101` 适合开发环境，但生产/联调建议改为显式必填或增加启动期严格校验，避免误用默认值。

- [ ] P3：补齐真实多进程 E2E 门禁  
  当前单测与集成测试覆盖较好，但真实多进程全链路 E2E 仍主要在计划文档中，建议落地到 `ctest` 常规门禁。

## OrbStack 虚拟机下 VSCode 调试（GDB）

在 OrbStack 虚拟机环境中，直接使用 VSCode `cppdbg + gdb` 启动 x86_64 程序，可能出现以下错误：

- `Couldn't get CS register: Input/output error`
- `Unexpected GDB output from command "-exec-run"`

可使用 `qemu-x86_64-static` + GDB remote 的方式调试。

### 1. 以 Debug 模式构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

### 2. 安装 QEMU user static

```bash
sudo apt update
sudo apt install -y qemu-user-static
```

### 3. 用 QEMU 启动程序并打开调试端口

```bash
qemu-x86_64-static -g 1234 ./build/src/acct_service_main --config ./config/default.yaml
```

说明：该命令会等待 GDB 连接，不会立即继续执行。

### 4. VSCode 连接调试

项目中已提供配置：`QEMU Remote acct_service_main (:1234)`（见 `.vscode/launch.json`）。

在 VSCode 中：
- 打开 `Run and Debug`
- 选择 `QEMU Remote acct_service_main (:1234)`
- 按 `F5` 连接

连接后即可正常下断点、单步、查看变量。
