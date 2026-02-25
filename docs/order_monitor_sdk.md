# 订单监控 SDK 使用文档

本文说明如何使用监控 SDK（`libacct_order_monitor.so`）从 `orders_shm` 只读采集订单快照。

## 1. 交付物

- 动态库：`libacct_order_monitor.so`
- 头文件：`include/api/order_monitor_api.h`

监控 SDK 已与下单 SDK 分离：

- 下单：`libacct_order.so`
- 监控：`libacct_order_monitor.so`

## 2. 共享内存命名规则

监控 SDK 读取的目标共享内存名称为：

`orders_shm_name + "_" + trading_day`

例如：

- `orders_shm_name="/orders_shm"`
- `trading_day="20260225"`
- 实际 shm 名称：`/orders_shm_20260225`

`trading_day` 必须是 `YYYYMMDD` 的 8 位数字字符串。

## 3. 快速开始

### 3.1 头文件与链接

```cpp
#include "api/order_monitor_api.h"
```

编译示例：

```bash
g++ -std=c++17 monitor_main.cpp \
  -I/path/to/include \
  -L/path/to/lib -lacct_order_monitor \
  -Wl,-rpath,/path/to/lib
```

### 3.2 最小调用流程

1. `acct_orders_mon_open` 打开监控上下文
2. `acct_orders_mon_info` 读取池头（容量、`next_index`、交易日等）
3. `acct_orders_mon_read` 按索引读取快照
4. `acct_orders_mon_close` 释放资源

仓库中可运行示例：`examples/order_monitor_usage.cpp`

## 4. API 说明

### 4.1 `acct_orders_mon_open`

```c
acct_mon_error_t acct_orders_mon_open(
    const acct_orders_mon_options_t* options,
    acct_orders_mon_ctx_t* out_ctx);
```

- `options->orders_shm_name`：可空，默认 `/orders_shm`
- `options->trading_day`：可空，默认读取环境变量 `ACCT_TRADING_DAY`，否则 `19700101`
- 成功返回 `ACCT_MON_OK`

### 4.2 `acct_orders_mon_info`

```c
acct_mon_error_t acct_orders_mon_info(
    acct_orders_mon_ctx_t ctx,
    acct_orders_mon_info_t* out_info);
```

主要字段：

- `capacity`：订单池容量
- `next_index`：当前已发布上界（增量轮询游标）
- `full_reject_count`：池满拒单计数
- `trading_day`：池交易日

### 4.3 `acct_orders_mon_read`

```c
acct_mon_error_t acct_orders_mon_read(
    acct_orders_mon_ctx_t ctx,
    uint32_t index,
    acct_orders_mon_snapshot_t* out_snapshot);
```

返回语义：

- `ACCT_MON_OK`：读取成功
- `ACCT_MON_ERR_NOT_FOUND`：索引不在当前可见范围（例如 `index >= next_index`）
- `ACCT_MON_ERR_RETRY`：与写进程并发冲突，建议短暂退避后重试

### 4.4 `acct_orders_mon_strerror`

```c
const char* acct_orders_mon_strerror(acct_mon_error_t err);
```

用于日志或诊断输出。

## 5. 推荐轮询模式

监控进程建议维护本地游标 `cursor`，按 `next_index` 增量消费：

```cpp
uint32_t cursor = 0;

acct_orders_mon_info_t info{};
if (acct_orders_mon_info(ctx, &info) == ACCT_MON_OK) {
    for (uint32_t i = cursor; i < info.next_index; ++i) {
        acct_orders_mon_snapshot_t snap{};
        for (int retry = 0; retry < 16; ++retry) {
            acct_mon_error_t rc = acct_orders_mon_read(ctx, i, &snap);
            if (rc == ACCT_MON_OK) {
                // process(snap)
                break;
            }
            if (rc == ACCT_MON_ERR_RETRY) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }
    }
    cursor = info.next_index;
}
```

## 6. 字段说明与注意事项

`acct_orders_mon_snapshot_t` 不是 `order_request` 的内存镜像，而是稳定 C ABI 快照结构，包含：

- 槽位元信息：`index`、`seq`、`last_update_ns`、`stage`、`source`
- 订单核心字段：`internal_order_id`、`order_type`、`trade_side`、数量/价格/状态等

这样设计是为了避免第三方依赖内部 C++ 结构布局（`atomic`、对齐、padding）导致 ABI 兼容问题。

### 6.1 槽位元信息字段

| 字段 | 含义 | 单位/格式 |
| --- | --- | --- |
| `index` | 订单在当日订单池中的槽位索引 | `uint32_t` |
| `seq` | 槽位 seqlock 版本号（偶数稳定，奇数写入中） | `uint64_t` |
| `last_update_ns` | 槽位最近一次更新的系统时间 | Unix Epoch 纳秒 |
| `stage` | 订单在链路中的阶段 | `uint8_t`，见 6.3 |
| `source` | 订单来源 | `uint8_t`，见 6.4 |

### 6.2 订单业务字段

| 字段 | 含义 | 单位/格式 |
| --- | --- | --- |
| `internal_order_id` | 系统内部订单 ID（主键） | `uint32_t` |
| `orig_internal_order_id` | 原始订单 ID（撤单请求对应被撤单 ID） | `uint32_t` |
| `internal_security_id` | 系统内部证券 ID | `uint16_t` |
| `security_id` | 证券代码字符串 | 定长 `char[16]` |
| `broker_order_id` | 柜台订单号字符串 | 定长 `char[32]` |
| `broker_order_id_u64` | 柜台订单号数字视图 | `uint64_t` |
| `order_type` | 订单类型（新单/撤单） | `uint8_t`，见 6.5 |
| `trade_side` | 买卖方向 | `uint8_t`，见 6.6 |
| `market` | 市场编码 | `uint8_t`（SZ=1/SH=2/BJ=3/HK=4） |
| `order_status` | 订单状态机状态码 | `uint8_t`，见 6.7 |
| `volume_entrust` | 委托数量 | 股/张（整型） |
| `volume_traded` | 已成交数量 | 股/张（整型） |
| `volume_remain` | 剩余数量 | 股/张（整型） |
| `dprice_entrust` | 委托价格 | 分（`uint64_t`） |
| `dprice_traded` | 成交均价 | 分（`uint64_t`） |
| `dvalue_traded` | 已成交金额 | 分（`uint64_t`） |
| `dfee_estimate` | 预估手续费 | 分（`uint64_t`） |
| `dfee_executed` | 已发生手续费 | 分（`uint64_t`） |
| `md_time_driven` | 触发时间 | `HHMMSSmmm` |
| `md_time_entrust` | 委托时间 | `HHMMSSmmm` |
| `md_time_cancel_sent` | 撤单发送时间 | `HHMMSSmmm` |
| `md_time_cancel_done` | 撤单完成时间 | `HHMMSSmmm` |
| `md_time_broker_response` | 柜台响应时间 | `HHMMSSmmm` |
| `md_time_market_response` | 交易所响应时间 | `HHMMSSmmm` |
| `md_time_traded_first` | 首次成交时间 | `HHMMSSmmm` |
| `md_time_traded_latest` | 最近成交时间 | `HHMMSSmmm` |

### 6.3 `stage` 取值（`acct_mon_order_stage_t`）

| 数值 | 枚举 | 含义 |
| --- | --- | --- |
| `0` | `ACCT_MON_STAGE_EMPTY` | 空槽位 |
| `1` | `ACCT_MON_STAGE_RESERVED` | 已保留 |
| `2` | `ACCT_MON_STAGE_UPSTREAM_QUEUED` | 已入上游队列 |
| `3` | `ACCT_MON_STAGE_UPSTREAM_DEQUEUED` | 已从上游队列取出 |
| `4` | `ACCT_MON_STAGE_RISK_REJECTED` | 风控拒绝 |
| `5` | `ACCT_MON_STAGE_DOWNSTREAM_QUEUED` | 已入下游队列 |
| `6` | `ACCT_MON_STAGE_DOWNSTREAM_DEQUEUED` | 已被网关消费 |
| `7` | `ACCT_MON_STAGE_TERMINAL` | 终态 |
| `8` | `ACCT_MON_STAGE_QUEUE_PUSH_FAILED` | 队列推送失败 |

### 6.4 `source` 取值（`acct_mon_order_source_t`）

| 数值 | 枚举 | 含义 |
| --- | --- | --- |
| `0` | `ACCT_MON_SOURCE_UNKNOWN` | 未知来源 |
| `1` | `ACCT_MON_SOURCE_STRATEGY` | 策略/API 提交 |
| `2` | `ACCT_MON_SOURCE_ACCOUNT_INTERNAL` | 账户服务内部生成（拆单/内部撤单等） |

### 6.5 `order_type` 取值

| 数值 | 含义 |
| --- | --- |
| `0` | NotSet |
| `1` | New |
| `2` | Cancel |
| `255` | Unknown |

### 6.6 `trade_side` 取值

| 数值 | 含义 |
| --- | --- |
| `0` | NotSet |
| `1` | Buy |
| `2` | Sell |

### 6.7 `order_status` 常用取值

| 数值（十六进制） | 状态 |
| --- | --- |
| `0x12` | StrategySubmitted |
| `0x20` | RiskControllerPending |
| `0x21` | RiskControllerRejected |
| `0x22` | RiskControllerAccepted |
| `0x30` | TraderPending |
| `0x31` | TraderRejected |
| `0x32` | TraderSubmitted |
| `0x33` | TraderError |
| `0x41` | BrokerRejected |
| `0x42` | BrokerAccepted |
| `0x51` | MarketRejected |
| `0x52` | MarketAccepted |
| `0x62` | Finished |
| `0xFF` | Unknown |

## 7. 性能建议

- 使用增量扫描，避免全量重复遍历历史槽位
- `ACCT_MON_ERR_RETRY` 采用短退避（如 1ms）后重试
- 建议监控进程独立 CPU 核，避免与主交易线程竞争
- 监控端保持只读，不要对共享内存写入

## 8. 限制

- 当前快照结构未直接提供“母单/子单”关系字段
- 监控语义是“状态快照可见”，不是事件流 exactly-once

## 9. 运行示例

仓库示例：

```bash
cmake -S . -B build
cmake --build build -j --target order_monitor_usage
./build/examples/order_monitor_usage
```
