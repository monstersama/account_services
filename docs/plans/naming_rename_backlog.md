# 命名改造清单（按优先级）

本清单基于当前仓库实际扫描结果，目标是先提升一致性，再处理可能影响外部适配器的改名。

## P0（低风险，建议优先）
2. 内部 C++ 头扩展名统一（`.h` -> `.hpp`）
- 现状：
  - 内部头 `src/portfolio/positions.h` 与项目多数 `.hpp` 混用。
- 目标：
  - `src/portfolio/positions.hpp`
- 位置：
  - 文件定义：`src/portfolio/positions.h`
  - 引用：`src/portfolio/position_manager.hpp:11`
  - 引用：`src/shm/shm_layout.hpp:10`
- 影响评估：
  - 仅 include 路径改动，风险低到中。

## P1（中风险，可做但建议分阶段）

1. `SHMManager` 与主风格统一
- 现状：
  - 项目主风格是 `snake_case` 类型名，`SHMManager` 为少数例外。
- 目标：
  - `shm_manager`
- 位置（示例）：
  - `src/shm/shm_manager.hpp:19`
  - `src/core/account_service.hpp:95`
  - `src/api/order_api.cpp:97`
  - `test/test_shm_manager.cpp:50`
- 影响评估：
  - 引用点较多（核心模块+测试），但主要是源码级影响。
- 建议策略：
  - 第 1 阶段：新增别名 `using SHMManager = shm_manager;`（或反向）。
  - 第 2 阶段：批量替换调用点。
  - 第 3 阶段：移除过渡别名。

## P2（高影响或外部兼容相关，谨慎）

1. `IBrokerAdapter` 接口命名去前缀
- 现状：
  - `IBrokerAdapter` 使用 `I` 前缀，与项目主风格不一致。
- 目标：
  - `broker_adapter`（或团队确认的新名）
- 位置：
  - `include/broker_api/broker_api.hpp:93`
  - `include/broker_api/broker_api.hpp:110`
  - `include/broker_api/broker_api.hpp:111`
  - `include/broker_api/broker_api.hpp:123`
  - `include/broker_api/broker_api.hpp:124`
  - `test/test_gateway_plugin_mode.cpp:115`
- 影响评估：
  - 对外插件实现方需同步改源码，存在生态兼容风险。
- 建议策略：
  - 保留导出 C 符号不变。
  - 先做类型别名过渡，给插件方留迁移窗口。

2. `_t` 后缀长期治理（仅新代码先行）
- 现状：
  - 内部类型别名与枚举大量使用 `_t`。
  - 例如：`src/common/types.hpp:9`、`src/core/account_service.hpp:22`、`src/order/order_request.hpp:12`
- 目标：
  - 新增 C++ 内部类型逐步不再引入新的 `_t`。
- 影响评估：
  - 全量改历史命名成本高，短期不建议一次性替换。

## 执行建议

1. 先做 P0，快速获得一致性收益。
2. P1 使用“别名过渡 + 批量替换”降低回归风险。
3. P2 仅在规划插件接口版本窗口时推进。
