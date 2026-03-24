# C++ 函数签名设计规范（account_services）

本规范用于统一仓库内 C++ 函数的输入参数、返回值、错误通道和输出参数设计，减少“所有东西都塞进参数里”的接口漂移，同时保留共享内存、无锁队列和交易热路径对低延迟接口的特殊要求。

适用范围：

- 内部 C++ 实现层：`src/**`、`gateway/src/**`、`include/broker_api/*.hpp`
- 不强行套用到稳定 C ABI：`include/api/*.h`

## 1. 总体原则

1. 默认用返回类型表达主结果，不把“主结果”隐藏在输出参数里。
2. 默认把失败语义编码进类型，而不是靠调用约定猜测。
3. 热路径优先保证零额外分配、零不必要拷贝和可预测时延。
4. C ABI、插件 ABI、共享内存协议优先保证兼容性，不做形式主义改造。
5. 新代码必须遵循本规范；历史代码按“触及时增量收敛”的方式迁移，不做全仓机械重写。

## 2. 选型顺序

设计函数签名时，先按以下顺序判断：

1. 这是热路径还是冷路径。
2. 这是内部 C++ 接口，还是稳定 ABI / SHM / 队列边界。
3. 调用方需要的是“值”“缺失”“错误原因”还是“原地写入”。
4. 是否必须复用调用方提供的缓冲区或对象以避免分配。

默认选型如下：

| 场景 | 首选写法 | 说明 |
| --- | --- | --- |
| 必然成功并返回单个主结果 | `T f(...)` | 直接返回值 |
| 可能没有结果，但“没有”是正常分支 | `std::optional<T>` | 不需要错误细节 |
| 可能失败，且调用方需要失败原因 | `std::expected<T, E>` | `E` 使用轻量错误类型 |
| 返回多个紧密相关的结果 | 返回小 `struct` | 优先具名字段，少用 `pair/tuple` |
| 只表示“尝试成功/失败”，且失败原因不重要 | `bool try_xxx(...)` | 可配合一个输出参数 |
| 必须复用调用方对象或缓冲区 | `bool/status + out 参数` | 仅在确有性能/ABI 理由时使用 |

## 3. 输入参数规范

### 3.1 只读的小型值类型

以下类型默认按值传递：

- 标量：`int`、`uint32_t`、`double`
- 枚举
- 轻量 ID / 句柄 / 下标
- 明确属于“被调用方要复制保存”的小对象

示例：

```cpp
OrderIndex next_index(OrderIndex current) noexcept;
bool is_valid_state(OrderState state) noexcept;
```

### 3.2 只读的非拥有视图

字符串和连续缓冲区优先使用视图类型：

- 字符串：`std::string_view`
- 连续内存：`std::span<const T>`

适用场景：

- 解析
- 查找
- 映射
- 热路径只读访问

示例：

```cpp
std::optional<InternalSecurityId> find_security_id(std::string_view code) const;
bool decode_levels(std::span<const std::byte> bytes, MarketDataView& out_view) noexcept;
```

### 3.3 大对象或拥有类型的只读输入

以下情况优先使用 `const T&`：

- `std::string`
- `std::vector<T>`
- 配置对象
- 较大的聚合结构

仅当被调用方需要接管所有权时，才考虑按值传递再 `std::move` 落地。

### 3.4 可变引用

非 `const T&` 只能用于两类语义：

1. 该对象本身就是被操作目标，函数语义明确是“原地修改”
2. 该对象是显式输出参数，且调用方提供它有明确性能或 ABI 理由

禁止把“看起来像输入，实际偷偷被改写”的参数设计成普通非 `const` 引用。

## 4. 输出与错误通道规范

### 4.1 单一主结果

存在明确主结果时，优先直接返回：

```cpp
InternalSecurityId add_security(std::string_view code, std::string_view name, Market market);
```

不要把主结果塞进 `out_value`，再用 `bool` 充当真实返回值，除非有热路径或 ABI 理由。

### 4.2 结果可缺失

“没找到”“不适用”“输入为空”这类常见分支，优先使用 `std::optional<T>`：

```cpp
std::optional<InternalSecurityId> find_security_id(std::string_view code) const;
```

适用于：

- 查表
- 条件生成
- 可选配置
- 解析后结果可能不存在

### 4.3 结果可能失败且需要原因

冷路径、解析层、验证层、协议边界优先使用 `std::expected<T, E>`。

示例：

```cpp
enum class ParseError {
    InvalidNumber,
    OutOfRange,
    InvalidFormat,
};

std::expected<uint32_t, ParseError> parse_u32(std::string_view text);
```

要求：

- `E` 必须轻量、稳定、可枚举
- 优先使用枚举、`error_code` 风格结构或轻量字符串
- 不在热路径上把 `std::expected<T, std::string>` 当默认方案

### 4.4 多结果返回

当函数有多个天然相关的输出时，优先返回具名结构，而不是多个 `out` 参数：

```cpp
struct ParsedSecurityId {
    Market market;
    std::string_view code;
};

std::optional<ParsedSecurityId> parse_internal_security_id(std::string_view text) noexcept;
```

仅在局部私有辅助逻辑中，才允许为临时便利使用 `std::pair` / `std::tuple`。

## 5. 何时允许输出参数

输出参数不是禁用项，但必须满足以下至少一条：

1. 位于交易热路径、事件循环、共享内存访问或队列读写路径
2. 调用方明确要求复用已分配对象或缓冲区
3. 接口需要维持稳定 C ABI / 插件 ABI
4. 输出对象较大，且返回值语义会引入不必要的构造、清零或搬运
5. 该函数本身语义就是“把结果写入目标对象”

仓库内可接受的典型例子：

- `spsc_queue::try_pop(T& item)`
- `orders_shm_try_allocate(..., OrderIndex& out_index)`
- `read_market_data_view(MarketDataView& out_view)`

这类接口的约束：

1. 输出参数统一放在输入参数之后。
2. 输出参数名统一使用 `out_` 前缀。
3. 返回 `bool` 时，函数名优先使用 `try_*`、`read_*`、`parse_*`、`build_*` 这类能表达成功条件的动词。
4. 失败时不得留下“部分写入但语义未定义”的状态，除非文档明确写清。
5. 非必要不要出现两个以上输出参数；若超过一个，优先考虑返回小结构体。

## 6. 何时不该继续使用 `bool + out 参数`

以下情况默认不应再新增 `bool + out 参数` 风格：

1. 冷路径配置解析
2. 业务规则校验
3. 字符串到数值或枚举的普通解析
4. 需要把失败原因暴露给上层的接口
5. 主结果天然只有一个值的接口

例如，下列风格属于优先重构对象：

```cpp
bool parse_u32(const std::string& text, uint32_t& out);
bool parse_bool(std::string text, bool& out);
```

更合适的方向是：

- `std::optional<uint32_t>`
- `std::expected<uint32_t, ParseError>`

具体选哪一种，取决于调用方是否需要错误细节。

## 7. 热路径特别约束

`src/order/`、`src/risk/`、`src/shm/`、`src/core/`、`src/execution/`、`gateway/src/` 中的交易数据面代码默认按热路径处理，除非能证明是冷控制面逻辑。

热路径签名设计要求：

1. 优先使用 `std::string_view`、`std::span`、定长字符串和引用，避免临时拥有对象。
2. 默认不在热路径引入异常作为常规控制流。
3. 默认不为“更现代的表面写法”引入额外分配、额外复制或不可见开销。
4. 当 `out` 参数能避免对象重建、缓冲区重复申请或队列元素搬运时，保留 `out` 参数优先于形式上的现代化。
5. 热路径里的 `std::expected` 只有在确认其错误通道成本和调用方收益都合理时才使用。

## 8. ABI 与边界约束

### 8.1 C API

`include/api/*.h` 继续保持 C 风格：

- 返回错误码
- 通过指针输出结果
- 明确所有权和缓冲区大小

不得为了统一内部 C++ 风格而破坏稳定 ABI。

### 8.2 插件 ABI / 网关边界

`include/broker_api/*.hpp` 和插件导出符号优先保证兼容性与可实现性。对这类接口做签名调整时，必须先评估：

- 插件实现成本
- 二进制兼容性
- 错误传播方式是否跨边界一致

### 8.3 SHM / 队列 / 协议读写

共享内存布局、seqlock 快照、SPSC/MPSC 队列接口优先遵循“调用方提供存储，函数只负责填充”的模式。这里允许保留 `bool + out 参数`，且不要求为了统一风格而改成返回临时对象。

## 9. 命名约定

- `try_*`：快速尝试，失败是正常分支，通常不附带错误细节
- `find_*`：查找，未命中通常返回 `optional`
- `parse_*`：解析，冷路径优先 `expected` / `optional`；热路径可保留 `bool + out`
- `build_*`：构造规范化结果；若只返回单值，优先直接返回
- `read_*`：从 SHM、队列、设备或外部视图读取；可使用 `out` 参数
- `fill_*` / `write_*`：语义上明确写入目标对象，允许 `out` 参数或目标引用

如果函数的真实语义是“计算得到一个值”，不要为了沿用旧习惯命名成 `fill_*`。

## 10. 增量迁移策略

1. 新增内部 C++ 接口必须直接按本规范设计。
2. 修改历史代码时，只在当前变更范围内顺手收敛，不做全仓批量改签名。
3. 热路径、SHM、队列、ABI 边界默认保守，除非能证明改动不会带来额外时延、复制或兼容性风险。
4. 冷路径中现有 `bool + out 参数` 接口，在被修改时应优先评估是否切换到 `optional` / `expected` / 直接返回值。

## 11. 仓库内参考样例

推荐沿用或参考的风格：

- `PositionManager::find_security_id(...)`：返回 `std::optional<InternalSecurityId>`
- `PositionManager::add_security(...)`：直接返回 `InternalSecurityId`
- `spsc_queue::try_pop(T& item)`：热路径、调用方提供存储
- `orders_shm_try_allocate(..., OrderIndex& out_index)`：SHM 分配结果写入调用方变量

应优先收敛的风格：

- 配置与文本解析中的 `bool parse_xxx(..., T& out)`
- 冷路径中主结果明确但仍使用 `bool + out` 的辅助函数

## 12. 评审检查清单

提交函数签名相关改动时，至少检查以下问题：

1. 主结果是否已经由返回类型直接表达。
2. “无结果”和“失败”是否被区分清楚。
3. 使用 `out` 参数是否有性能、ABI 或原地写入的必要理由。
4. 热路径是否引入了额外分配、拷贝、异常或隐藏开销。
5. 调用方是否能从签名本身看懂所有权、生命周期和失败语义。
