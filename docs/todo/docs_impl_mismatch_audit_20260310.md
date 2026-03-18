# 文档与实现不匹配审查记录

日期：2026-03-10

范围：

- `docs/` 下以 `src` 开头的模块文档
- 订单相关流程图与架构图
- 对应实现位于 `src/` 和 `gateway/`

结论：

- `docs/src_*.md` 正文大部分能对上当前实现。
- 偏差主要集中在订单流程图、架构图、错误流图，以及少数把“组件能力”写成“当前 CLI 实际行为”的描述。
- 以下记录基于静态代码审查，不包含 Mermaid 渲染验证和运行时联调验证。

## 高优先级偏差

### 1. 成交回报通道被画错成 `downstream_shm`

问题：

- 主流程图和架构图都把成交回报写回了 `downstream_shm`。
- 实现里成交回报实际走独立的 `trades_shm->response_queue`。

文档位置：

- `docs/order_flowchart.md:65-67`
- `docs/order_flowchart.md:194-198`
- `docs/order_flowchart.mmd:47-49`
- `docs/order_architecture.mmd:35-39`

实现位置：

- `src/shm/shm_layout.hpp:125-146`
- `gateway/src/gateway_loop.cpp:217-227`
- `src/core/event_loop.cpp:288-299`

说明：

- `downstream_shm` 是账户服务到交易进程的订单索引队列。
- `trades_shm` 才是交易进程回写 `TradeResponse` 的回报队列。
- 这会直接误导共享内存链路排查。

### 2. 错误处理流程图不是当前实现，而更像理想化设计

问题：

- 图里写了“记录日志 -> 回写用户指令状态 -> 解冻资金/持仓 -> 保存到数据库归档”的统一错误处理链路。
- 当前代码没有这条统一错误处理分支。

文档位置：

- `docs/order_error_flow.mmd:6-40`
- `docs/order_flowchart.md:220-266`

实现位置：

- `src/core/event_loop.cpp:353-364`
- `src/core/event_loop.cpp:137-148`
- `src/core/event_loop.cpp:497-505`
- `src/core/event_loop.cpp:513-556`
- `src/core/event_loop.cpp:395-412`
- `src/order/order_book.cpp:267-319`

说明：

- 风控拒绝当前只是更新状态与 slot stage 后直接返回。
- 当前“回写上游用户指令状态”主要依赖 `orders_shm` 镜像同步，不是单独错误通知链。
- 当前“归档”只是 `OrderBook::archive_order()` 的内存归档，不包含数据库持久化。

### 3. “风控通过后冻结资金/持仓”与实际实现不符

问题：

- 流程图和 `portfolio` 文档都把卖单持仓冻结写成当前主链路行为。
- 当前主链路只冻结买单资金，不冻结卖单仓位。

文档位置：

- `docs/order_flowchart.md:33`
- `docs/order_flowchart.md:86`
- `docs/order_flowchart.mmd:15`
- `docs/src_portfolio_module.md:211-218`

实现位置：

- `src/core/event_loop.cpp:114-135`
- `src/core/event_loop.cpp:371-380`
- `src/portfolio/position_manager.cpp:445-480`

说明：

- `EventLoop::reserve_order_resources()` 只在新单且买单时调用 `freeze_fund()`。
- `freeze_position()` 函数存在，但当前主流程没有调用点。

## 中优先级偏差

### 4. 架构图内部模块调用关系画错

问题：

- 图中表现为 `OrderBook -> RiskManager -> OrderSplitter -> OrderRouter` 的串行调用链。
- 当前实现的实际调度中心是 `EventLoop`。

文档位置：

- `docs/order_architecture.mmd:17-23`
- `docs/order_flowchart.md:176-182`

实现位置：

- `src/core/event_loop.cpp:331-384`
- `src/core/event_loop.cpp:427-493`
- `src/order/order_router.cpp:10-16`
- `src/order/order_router.cpp:27-29`

说明：

- `EventLoop::handle_order_request()` 直接调用 `OrderBook`、`RiskManager`、`order_router`。
- `order_splitter` 是 `order_router` 的内部成员，不是 `RiskManager` 直接驱动。
- 图里的 `PositionManager -> OrderBook`、`RiskManager -> OrderSplitter` 等箭头与当前代码不符。

### 5. 状态图把 `TraderPending` 画成普通新单必经状态，并漏掉撤单路径

问题：

- 文档把普通新单画成 `RiskControllerAccepted -> TraderPending -> TraderSubmitted`。
- 当前普通新单发送成功后直接进入 `TraderSubmitted`。
- 文档没有体现撤单完整路径。

文档位置：

- `docs/order_state_diagram.mmd:16-18`
- `docs/order_flowchart.md:17-92`

实现位置：

- `src/core/event_loop.cpp:366-385`
- `src/order/order_router.cpp:41-54`
- `src/order/order_router.cpp:67-199`
- `src/order/order_router.cpp:91-94`
- `src/order/order_router.cpp:151-156`
- `src/order/order_router.cpp:272-299`

说明：

- `TraderPending` 当前主要出现在内部构造的撤单请求和拆单子单初始状态里。
- 如果文档要保留“完整生命周期”这个表述，应把 cancel 路径补齐；否则应明确只描述新单主路径。

### 6. 流程图把未实现能力写成现状

问题：

- 文档把 `VWAP` 列为当前可用拆单策略。
- 文档把“超过日限额、证券不允许、账户冻结”等风控拒因写成现有实现。

文档位置：

- `docs/order_flowchart.md:83-88`
- `docs/order_flowchart.md:153`
- `docs/order_state_diagram.mmd:41-50`

实现位置：

- `src/order/order_splitter.cpp:41-50`
- `src/order/order_splitter.cpp:123-125`
- `src/risk/risk_manager.cpp:166-200`
- `src/risk/risk_checker.cpp:34-190`

说明：

- 当前拆单只支持 `FixedSize`、`Iceberg`、`TWAP`。
- 当前默认风控规则只有资金、持仓、单笔金额、单笔数量、价格限制、重复单、速率限制。

## 低优先级偏差

### 7. `src/core` 文档把“命令行覆盖项”写成当前 CLI 行为

问题：

- 文档描述 `ConfigManager` 支持命令行覆盖，容易让读者理解为主服务二进制当前已经开放这些覆盖能力。
- 当前 `main.cpp` 实际只支持 `--config` 和位置参数配置路径。

文档位置：

- `docs/src_core_module.md:19`
- `docs/src_core_module.md:44-47`

实现位置：

- `src/core/config_manager.cpp:549-660`
- `src/main.cpp:19-108`

说明：

- `ConfigManager::parse_command_line()` 的确实现了覆盖项解析能力。
- 但当前 `main.cpp` 没有接入这套解析逻辑。

### 8. `order_flowchart.md` 存在几处陈旧引用和描述

问题：

- 头文件路径写成了不存在的 `include/order/order_request.hpp`。
- 头文件路径写成了不存在的 `include/shm/shm_layout.hpp`。
- 风控被写成“并行风控检查”。
- 状态枚举类型写成了 `order_status_t`。

文档位置：

- `docs/order_flowchart.md:270-282`
- `docs/order_flowchart.md:294`
- `docs/order_flowchart.md:314`
- `docs/order_flowchart.md:319`

实现位置：

- `src/order/order_request.hpp:34-205`
- `src/shm/shm_layout.hpp:15-164`
- `src/risk/risk_manager.cpp:27-45`

说明：

- 当前风控检查是顺序短路执行，不是并行执行。
- 当前状态枚举类型名为 `OrderState`。

## 相对准确的部分

以下文档正文与实现整体较为一致：

- `docs/src_api_module.md`
- `docs/src_common_module.md`
- `docs/src_broker_api_module.md`
- `docs/src_shm_module.md`
- `docs/src_order_module.md`
- `docs/src_risk_module.md`
- `docs/src_modules_overview.md`

补充说明：

- `docs/src_order_module.md` 对 `OrderBook` 索引模型、母子单聚合、`recover_downstream_active_orders_from_shm()` 的恢复范围与步骤描述基本准确。
- `docs/src_risk_module.md` 对规则顺序、短路语义、`duplicate_order_rule` 按 `internal_order_id` 去重的描述与代码一致。
- `docs/src_core_module.md` 对初始化顺序、五类 SHM 打开、`EventLoop` 回写 `orders_shm` 镜像的描述整体准确，主要问题集中在 CLI 覆盖项表述。

## 后续修订建议

建议按以下顺序修订：

1. 先修 `docs/order_flowchart.md`
2. 同步修 `docs/order_flowchart.mmd`
3. 修 `docs/order_architecture.mmd`
4. 重写或弱化 `docs/order_error_flow.mmd`
5. 修 `docs/order_state_diagram.mmd`
6. 修 `docs/src_portfolio_module.md` 中卖单冻结描述
7. 修 `docs/src_core_module.md` 中 CLI 覆盖项表述
