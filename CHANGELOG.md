# Changelog

本文件记录 `account_services` 的仓库级重要变更，按版本倒序维护。

## 维护约定

- 未发布的改动先记录在 `[Unreleased]`。
- 通常按 `Added`、`Changed`、`Fixed`、`Removed` 分类整理。
- 版本号以仓库 Git tag 或正式打包版本为准。

## [Unreleased]

暂无未发布变更。

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
