# C++ 命名规范基线（account_services）

本规范用于统一本项目命名风格，并尽量减少对现有接口的破坏性变更。

## 1. 分层原则

- C++ 内部实现层（`src/**`、`include/broker_api/*.hpp`）：使用统一 C++ 命名风格。
- C API / ABI 稳定层（`include/api/*.h`、导出宏、导出符号）：保持 C 风格，不强行改成 C++ 风格。

## 2. 命名规则

1. 命名空间
- 使用 `snake_case`。
- 示例：`acct_service`、`acct_service::broker_api`。

2. 类型（`class` / `struct` / `enum` / 类型别名）
- 统一使用 `snake_case`。
- 新代码避免再引入大写缩写类名或 `I` 前缀接口名。
- 示例：`order_book`、`risk_manager`、`broker_adapter`。

3. 函数/方法
- 使用 `snake_case`，优先动词短语。
- 布尔返回/布尔属性优先 `is_*` / `has_*` / `can_*` / `should_*`。

4. 变量
- 局部变量、参数使用 `snake_case`。
- 成员变量使用 `snake_case_`（尾下划线）。
- 示例：`orders_shm_`、`last_error_`。

5. 常量
- `constexpr` / `const` 常量统一 `kXxx`。
- 仅宏（`#define`）保留全大写 `ACCT_*` / `CPU_*`。
- 示例：`kBrokerOrderIdSize`、`kSecurityIdSize`。

6. 枚举值
- 使用 `PascalCase`（与项目现状一致），避免全大写常量式枚举项。
- 示例：`Created`、`RiskControllerRejected`。

7. 文件名
- C++ 头文件使用 `.hpp`。
- `.h` 仅用于 C API/C 兼容头。
- 示例：`positions.hpp`（内部 C++），`order_api.h`（C API）。

## 3. 兼容性约束

1. 不改 ABI 符号名
- 如 `acct_create_broker_adapter`、`ACCT_*` 宏保持不变。

2. 控制源码破坏范围
- 对外 C++ 插件接口改名时，先提供别名过渡（`using old = new;` 或反向别名），再逐步迁移。

## 4. 增量落地策略

1. 新增代码必须遵循本规范，不再扩大风格混用面。
2. 历史代码按“低风险先行”批量改名（常量、文件名）。
3. 对外接口类型名改动采用“两阶段迁移”（先别名，后清理）。
