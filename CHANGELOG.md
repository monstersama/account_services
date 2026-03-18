# Changelog

本文件记录 `account_services` 的仓库级重要变更，按版本倒序维护。

## 维护约定

- 未发布的改动先记录在 `[Unreleased]`。
- 通常按 `Added`、`Changed`、`Fixed`、`Removed` 分类整理。
- 版本号以仓库 Git tag 或正式打包版本为准。

## [Unreleased]

## [1.1.3] - 2026-03-18

### Changed

- 将初始订单状态 `StrategySubmitted` 重命名为 `UserSubmitted`，并把事件文本 `strategy_submitted` 同步改为 `user_submitted`，避免与内部 `strategy` 语义混淆。
- 将上游订单来源 `Strategy` 重命名为 `User`，同步更新监控 API 常量 `ACCT_MON_SOURCE_USER`、相关测试和文档，保持状态码与来源码数值不变。

## [1.1.2] - 2026-03-17

### Fixed

- 修复行情价格没有正确显示在实际发送的子单里。

## [1.1.1] - 2026-03-17

### Added

- 新增 `market_data.allow_order_price_fallback` 调试开关，支持在行情缺失或盘口无效时回退使用父单委托价发出托管子单。

### Changed

- 账户服务启动时新增配置项输出，使用 `[config][section]key=value` 扁平格式打印，并且仅在 `Debug` 编译产物中启用。
- 补全 `config/acct.dev.yaml` 中缺失的 `event_loop` 配置项，避免开发配置回退到隐式默认值。
- 为账户服务内置配置文件增加显式覆盖检查，防止后续新增配置键未同步写入仓库配置。
- 订单业务日志从通用技术日志分流为独立 recorder：生产默认异步落独立文件，`Debug` 构建额外输出父子单与执行会话详细 trace。
- 托管执行子单在有效盘口下改为直接使用盘口顶档价格：买单优先卖一、对手档无效时回退到买一；卖单优先买一、对手档无效时回退到卖一，不再用父单委托价做盘口有效场景下的夹价边界。
- `order_events_*` 与 `order_debug_*` 的日志语义进一步拆分：业务日志继续保留订单快照事实字段，调试日志改为 `trace=` 视角并按事件类型裁剪输出字段，避免不同流之间重复输出误导性的默认值。

### Fixed

- 托管执行会话在拆单运行时约束不满足时，不再把拒单原因记录为 `invalid_session`，而是输出明确的不可拆单语义。
- `TWAP` 托管父单在视图未变化时不再重复触发 `parent_refreshed`，避免 `order_events_*` 日志被事件循环频率刷屏。
- 修复显式父单 `internal_order_id` 未抬升本地 ID 生成器的问题，避免托管子单撞号后表现为 `route_failed`。
- 统一父单、执行引擎内部子单和内部撤单子单的订单号分配到 `upstream_shm->header.next_order_id`，修复托管子单先占用本地号段后，后续父单可能复用旧 `internal_order_id` 并触发 `DuplicateOrder` 的问题。
- 修复 `order_debug_*` 中 `shm_order_index` 与结果字段长期落默认值的问题：`child_submit_result` / `child_finalized` 现在会回填真实槽位索引，且 `success`、`cancel_requested`、`reason`、`result` 等字段只在对应 trace 类型中出现。

## [1.1.0] - 2026-03-15

### Added

- 新增 `market_data` 模块，从共享内存接入行情快照并提供行情服务。
- 新增 `execution` 模块，统一托管执行运行时，为每个交易指令创建独立会话并承载被动拆单流程。
- 新增 `strategy` 模块，用于挂接主动执行策略，并支持通过配置文件启用策略实现。
- 新增 `tools/bootstrap_third_party.sh` 和 `third_party/repos.lock`，用于按固定提交拉取第三方依赖。
- 新增 `tools/market_data_quote_cli.cpp`，用于行情快照调试与验证。

### Changed

- 内部证券标识统一为 `ISO10383_MIC` 交易所编码格式。
- 上游订单共享内存命名统一，相关配置和文档同步更新。
- 订单拆单与执行运行时收敛到新的 `execution` 模块，相关 API、配置和测试一并调整。

### Removed

- 移除旧的 `order_splitter` 模块，一次性拆单逻辑不再单独维护。
- 移除未使用的依赖，第三方组件接入改为通过 `third_party/` 清单统一管理。

## [1.0.11] - 2026-03-15

### Added

- 新增仓库级 `CHANGELOG.md`，用于维护版本变更记录。

## [1.0.10] - 2026-03-12

### Notes

- 无行情、无策略模块的稳定版本。
