# Changelog

本文件记录 `account_services` 的仓库级重要变更，按版本倒序维护。

## 维护约定

- 未发布的改动先记录在 `[Unreleased]`。
- 通常按 `Added`、`Changed`、`Fixed`、`Removed` 分类整理。
- 版本号以仓库 Git tag 或正式打包版本为准。

## [Unreleased]

### Added

- 行情模块`market_data`：从共享内存接入行情快照。
- 执行模块`execution`：管理拆单策略，为每个交易指令创建独立的会话，管理被动拆单以及策略
- 策略模块`strategy`：使用策略模块创建主动执行的拆单算法，通过配置文件配置。

### Changed

- 适配交易所名称协议`ISO10383_MIC`

### Removed

- `order_splitter`: 移除原来的拆单模块，不再使用该模块进行一次性的拆单，被动拆单执行全部收敛到新的执行模块里`execution`。

## [1.0.11] - 2026-03-15

### Added

- add: CHANGELOG.md

## [1.0.10] - 2026-03-12

无行情、无策略版本稳定版本。

### Note
