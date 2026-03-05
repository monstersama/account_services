#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/fixed_string.hpp"
#include "common/spinlock.hpp"
#include "common/types.hpp"

namespace acct_service {

enum class ErrorDomain : uint8_t {
    none = 0,    // 未指定模块
    config,      // 配置加载/校验模块
    shm,         // 共享内存模块
    core,        // 核心服务编排与事件循环
    order,       // 订单簿/路由/拆单链路
    risk,        // 风控规则与管理
    portfolio,   // 资金/持仓/账户记录
    api,         // 对外 C API 适配层
};

enum class ErrorCode : uint16_t {
    Ok = 0,  // 成功，无错误

    // [1000, 1099] 通用/核心基础错误码
    InvalidParam = 1000,      // 参数非法（接口入参）
    InvalidState = 1001,      // 当前状态不允许执行该操作
    ComponentUnavailable = 1002,  // 核心组件不可用
    HealthCheckFailed = 1003,  // 健康检查失败
    InternalError = 1099,      // 未分类内部错误（默认按高风险处理）

    // [2000, 2099] 配置相关错误码
    InvalidConfig = 2000,         // 配置值非法（语义级）
    ConfigParseFailed = 2001,     // 配置解析失败（语法/格式）
    ConfigValidateFailed = 2002,  // 配置校验失败（业务规则）

    // [3000, 3099] 共享内存相关错误码
    ShmOpenFailed = 3000,       // 共享内存打开/创建失败
    ShmFstatFailed = 3001,      // 共享内存元数据获取失败
    ShmResizeFailed = 3002,     // 共享内存扩容/尺寸不匹配
    ShmMmapFailed = 3003,       // 共享内存映射失败
    ShmHeaderInvalid = 3004,    // 共享内存头非法（可判为关键错误）
    ShmHeaderCorrupted = 3005,  // 共享内存头损坏（通常致命）

    // [4000, 4099] 订单链路相关错误码
    InvalidOrderId = 4000,      // 订单ID非法（如0或越界）
    DuplicateOrder = 4001,      // 重复订单
    OrderBookFull = 4002,       // 订单簿容量耗尽
    OrderNotFound = 4003,       // 订单不存在
    QueueFull = 4004,           // 队列已满（通用）
    QueuePushFailed = 4005,     // 入队失败
    QueuePopFailed = 4006,      // 出队失败
    RouteFailed = 4007,         // 路由失败
    SplitFailed = 4008,         // 拆单失败
    OrderInvariantBroken = 4009,  // 订单簿关键不变量破坏（高风险）
    OrderPoolFull = 4010,       // 订单池容量耗尽

    // [6000, 6099] 资金/持仓相关错误码
    PositionUpdateFailed = 6000,  // 持仓/资金更新失败（高风险）

    // [7000, 7099] 日志设施相关错误码
    LoggerInitFailed = 7000,    // 日志模块初始化失败
    LoggerThreadFailed = 7001,  // 日志线程异常/不可用
    LoggerQueueFull = 7002,     // 日志队列已满（可降级）
};

enum class ErrorSeverity : uint8_t {
    Recoverable = 0,  // 可恢复：记录后继续运行
    Critical = 1,     // 关键：应停服并退出主进程
    Fatal = 2,        // 致命：状态不可信，最高优先级停服退出
};

struct ErrorPolicy {
    ErrorSeverity severity = ErrorSeverity::Recoverable;
    bool stop_service = false;
    bool exit_process = false;
};

struct ErrorStatus {
    ErrorDomain domain = ErrorDomain::none;
    ErrorCode code = ErrorCode::Ok;
    int sys_errno = 0;
    TimestampNs ts_ns = 0;
    uint32_t line = 0;
    FixedString<24> module{};
    FixedString<96> file{};
    FixedString<192> message{};

    bool ok() const noexcept;
};

const char* to_string(ErrorDomain domain) noexcept;
const char* to_string(ErrorCode code) noexcept;
const char* to_string(ErrorSeverity severity) noexcept;
const ErrorPolicy& classify(ErrorDomain domain, ErrorCode code) noexcept;

ErrorStatus make_error_status(ErrorDomain domain, ErrorCode code, std::string_view module, std::string_view file,
    uint32_t line, std::string_view message, int sys_errno = 0);

class ErrorRegistry {
public:
    static constexpr std::size_t kHistoryCapacity = 4096;

    void record(const ErrorStatus& status);
    uint64_t count(ErrorCode code) const;
    std::vector<ErrorStatus> recent_errors() const;
    void reset();

private:
    struct ErrorCodeHash {
        std::size_t operator()(ErrorCode code) const noexcept { return static_cast<std::size_t>(code); }
    };

    mutable SpinLock lock_;
    std::unordered_map<ErrorCode, uint64_t, ErrorCodeHash> counters_;
    std::array<ErrorStatus, kHistoryCapacity> history_{};
    std::size_t history_pos_ = 0;
    std::size_t history_size_ = 0;
};

ErrorRegistry& global_error_registry();
void record_error(const ErrorStatus& status);
const ErrorStatus& last_error() noexcept;
const ErrorStatus& latest_error() noexcept;
void clear_last_error() noexcept;

void request_shutdown(ErrorSeverity severity) noexcept;
ErrorSeverity shutdown_reason() noexcept;
void clear_shutdown_reason() noexcept;
bool should_stop_service() noexcept;
bool should_exit_process() noexcept;

}  // namespace acct_service

#define ACCT_MAKE_ERROR(domain, code, module, message, sys_errno) \
    ::acct_service::make_error_status(                            \
        (domain), (code), (module), __FILE__, static_cast<uint32_t>(__LINE__), (message), (sys_errno))
