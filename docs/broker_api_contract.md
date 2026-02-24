# Broker API Contract

`broker_api` 是 `account_services` 对外暴露的券商适配层 ABI。下游网关与外部券商实现都通过该接口对接。

## 目录与产物

- 头文件：`include/broker_api/broker_api.hpp`
- 导出宏：`include/broker_api/export.h`
- 共享库：`libacct_broker_api.so`

## ABI 版本

- 当前 ABI 版本常量：`acct_service::broker_api::kBrokerApiAbiVersion`
- 运行时查询接口：`acct_service::broker_api::broker_api_abi_version()`
- 库版本字符串：`acct_service::broker_api::broker_api_version()`

新增字段、枚举值、接口方法时，需要同步提升 ABI 版本并更新调用方兼容矩阵。

## 核心接口

`acct_service::broker_api::IBrokerAdapter`

- `initialize(const broker_runtime_config&)`
- `submit(const broker_order_request&)`
- `poll_events(broker_event* out_events, std::size_t max_events)`
- `shutdown()`

### 插件导出接口（C 符号）

当网关使用 `--broker-type plugin` 时，适配器插件需导出以下符号：

- `acct_broker_plugin_abi_version()`
- `acct_create_broker_adapter()`
- `acct_destroy_broker_adapter(IBrokerAdapter*)`

符号名常量定义在 `broker_api.hpp`：

- `kPluginAbiSymbol`
- `kPluginCreateSymbol`
- `kPluginDestroySymbol`

`submit` 的行为约束：

- `accepted=true`：网关认为请求已被适配器受理，后续状态通过 `poll_events` 返回。
- `accepted=false && retryable=true`：网关按重试策略重提。
- `accepted=false && retryable=false`：网关直接生成 `TraderError` 回报。

## 数据契约

### broker_order_request

- `internal_order_id`：账户服务内部订单 ID，必须保留。
- `type`：`New` 或 `Cancel`。
- `orig_internal_order_id`：撤单时的原订单 ID。
- `trade_side/order_market/volume/price/security_id`：新单必填。

### broker_event

- `internal_order_id`：回报关联键。
- `kind`：事件语义（受理/拒绝/成交/完成）。
- `volume_traded/price_traded/value_traded/fee`：成交类事件填写。

## gateway 状态映射

网关将 `broker_event.kind` 映射为 `trade_response.new_status`：

- `BrokerAccepted` -> `order_status_t::BrokerAccepted`
- `BrokerRejected` -> `order_status_t::BrokerRejected`
- `MarketRejected` -> `order_status_t::MarketRejected`
- `Trade` -> `order_status_t::MarketAccepted`
- `Finished` -> `order_status_t::Finished`

## 外部仓库接入方式

- 头文件搜索路径：`-I<account_services>/include`
- 链接库：`libacct_broker_api.so`
- 适配器实现可独立仓库维护，网关侧只依赖 ABI，不依赖外部源码。

插件模式下，外部仓库需额外产出 `adapter.so`，并保证上述 3 个导出符号可被 `dlsym` 解析。
