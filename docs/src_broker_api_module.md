# `src/broker_api` 模块设计

## 1. 模块定位

`src/broker_api` 本身非常薄，它的职责不是实现券商逻辑，而是提供一个稳定的券商适配 ABI 壳层：

- 导出动态库版本号
- 导出 ABI 版本号
- 配合 `include/broker_api` 中的头文件定义稳定接口和插件符号约定

源码范围：

- [`src/broker_api/broker_api.cpp`](../src/broker_api/broker_api.cpp)
- [`include/broker_api/broker_api.hpp`](../include/broker_api/broker_api.hpp)
- [`include/broker_api/export.h`](../include/broker_api/export.h)

## 2. 核心职责

- 暴露 `broker_api_version()`
- 暴露 `broker_api_abi_version()`
- 定义 `IBrokerAdapter` 的稳定接口契约
- 定义插件模式需要导出的 C 符号名和函数签名

当前 `src/broker_api/broker_api.cpp` 的实现非常简单，真正的契约主体在头文件中。

## 3. 关键接口与 ABI

### 3.1 版本与 ABI

- `kBrokerApiAbiVersion`
- `broker_api_version()`
- `broker_api_abi_version()`

它们的用途分别是：

- 版本字符串：标识当前库版本
- ABI 版本号：让调用方在运行时判断二进制兼容性

### 3.2 核心抽象接口 `IBrokerAdapter`

外部券商适配器只需要实现：

- `initialize(const broker_runtime_config&)`
- `submit(const broker_order_request&)`
- `poll_events(broker_event* out_events, std::size_t max_events)`
- `shutdown() noexcept`

这让网关层可以用统一 ABI 驱动不同券商实现，而无需依赖券商源码仓库本身。

### 3.3 关键 ABI 数据结构

- `broker_runtime_config`
- `broker_order_request`
- `send_result`
- `broker_event`

这些结构承载：

- 网关下发的运行参数
- 统一订单请求
- submit 返回结果
- 适配器回报事件

### 3.4 插件模式导出符号

插件模式下要求导出：

- `acct_broker_plugin_abi_version()`
- `acct_create_broker_adapter()`
- `acct_destroy_broker_adapter(IBrokerAdapter*)`

对应符号名常量：

- `kPluginAbiSymbol`
- `kPluginCreateSymbol`
- `kPluginDestroySymbol`

## 4. 在系统中的位置

`src/broker_api` 自身不直接依赖 `src/core`、`src/order` 或 `src/risk` 的实现逻辑，但它处在整条交易生态的外部边界上。

可以把它理解为：

- 账户服务内部统一请求 / 回报语义
- 与外部券商适配器之间
- 的稳定 ABI 边界

典型数据流可概括为：

1. 账户服务或网关生成统一订单请求
2. 网关把请求封装为 `broker_order_request`
3. 调用 `IBrokerAdapter::submit(...)`
4. 券商适配器与柜台交互
5. 适配器通过 `poll_events(...)` 产出 `broker_event`
6. 网关再把 `broker_event` 映射回内部 `trade_response`
7. 后续由 `core/event_loop` 与 `order/portfolio` 继续处理

因此：

- `src/broker_api` 不是“业务模块”
- 它是“稳定接口模块”

## 5. 当前实现的边界

需要明确区分：

- `include/broker_api/broker_api.hpp`
  - 定义 ABI 契约
- `src/broker_api/broker_api.cpp`
  - 只实现版本函数

这意味着当前仓库中的 `src/broker_api` 并不包含：

- 券商通讯实现
- 订单状态映射实现
- 插件加载器实现

这些能力应由网关或外部适配器仓库承载。

## 6. 依赖与边界

### 直接依赖

- `include/broker_api/export.h`
- 生成的版本头 `version.h`

### 间接系统关系

- 上游：账户服务 / 网关侧的统一订单请求
- 下游：外部券商适配器实现
- 返回：网关把适配器事件翻译回内部回报，再送回账户服务

## 7. 相关专题文档

- [Broker API Contract](broker_api_contract.md)

这份专题文档已经覆盖：

- 产物位置
- ABI 版本
- `IBrokerAdapter` 契约
- 插件符号
- `submit` 的 `accepted / retryable` 语义
- `broker_event.kind` 到内部订单状态的映射

模块文档只负责说明 `src/broker_api` 在仓库中的角色，不重复抄录全部契约细节。

## 8. 维护提示

- 若修改 `broker_order_request`、`broker_event` 或 `IBrokerAdapter`，必须同步评估 ABI 版本是否需要提升。
- 若增加新的插件符号或改变现有符号名，必须同步更新：
  - 头文件常量
  - `broker_api_contract.md`
  - 外部适配器接入说明
- 若未来在 `src/broker_api` 增加更多实现代码，应先明确这些代码属于：
  - ABI 壳层
  - 网关实现
  - 还是具体券商适配器实现
  避免把“稳定接口”与“具体业务逻辑”重新耦合。
