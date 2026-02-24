#pragma once

#include <cstddef>
#include <cstdint>

#include "broker_api/export.h"

namespace acct_service::broker_api {

// broker_api ABI 版本，用于调用方做兼容性检查。
inline constexpr uint32_t kBrokerApiAbiVersion = 1;
inline constexpr std::size_t kSecurityIdSize = 16;
inline constexpr std::size_t kBrokerOrderIdSize = 32;

enum class request_type : uint8_t {
    Unknown = 0,
    New = 1,
    Cancel = 2,
};

enum class side : uint8_t {
    Unknown = 0,
    Buy = 1,
    Sell = 2,
};

enum class market : uint8_t {
    Unknown = 0,
    SZ = 1,
    SH = 2,
    BJ = 3,
    HK = 4,
};

enum class event_kind : uint8_t {
    None = 0,
    BrokerAccepted = 1,
    BrokerRejected = 2,
    MarketRejected = 3,
    Trade = 4,
    Finished = 5,
};

// 适配器运行参数（由 gateway 启动时下发）。
struct broker_runtime_config {
    uint32_t account_id = 1;
    bool auto_fill = true;
};

// gateway 发给券商适配器的统一订单请求。
struct broker_order_request {
    uint32_t internal_order_id = 0;
    uint32_t orig_internal_order_id = 0;
    uint16_t internal_security_id = 0;
    request_type type = request_type::Unknown;
    side trade_side = side::Unknown;
    market order_market = market::Unknown;
    uint64_t volume = 0;
    uint64_t price = 0;
    uint32_t md_time = 0;
    char security_id[kSecurityIdSize]{};
};

// 适配器 submit 返回结果：成功、可重试失败、不可重试失败。
struct send_result {
    bool accepted = false;
    bool retryable = false;
    int32_t error_code = 0;

    static send_result ok() noexcept { return send_result{true, false, 0}; }

    static send_result retryable_error(int32_t code) noexcept { return send_result{false, true, code}; }

    static send_result fatal_error(int32_t code) noexcept { return send_result{false, false, code}; }
};

// 券商适配器回报事件，gateway 会将其转换为 trade_response。
struct broker_event {
    event_kind kind = event_kind::None;
    uint32_t internal_order_id = 0;
    uint32_t broker_order_id = 0;
    uint16_t internal_security_id = 0;
    side trade_side = side::Unknown;
    uint64_t volume_traded = 0;
    uint64_t price_traded = 0;
    uint64_t value_traded = 0;
    uint64_t fee = 0;
    uint32_t md_time_traded = 0;
    uint64_t recv_time_ns = 0;
};

// 券商适配器抽象接口：外部券商实现只需实现这个接口。
class IBrokerAdapter {
public:
    virtual ~IBrokerAdapter() = default;

    virtual bool initialize(const broker_runtime_config& config) = 0;
    virtual send_result submit(const broker_order_request& request) = 0;
    virtual std::size_t poll_events(broker_event* out_events, std::size_t max_events) = 0;
    virtual void shutdown() noexcept = 0;
};

// 插件导出符号约定（用于 dlsym 查找）。
inline constexpr const char* kPluginAbiSymbol = "acct_broker_plugin_abi_version";
inline constexpr const char* kPluginCreateSymbol = "acct_create_broker_adapter";
inline constexpr const char* kPluginDestroySymbol = "acct_destroy_broker_adapter";

// 插件函数签名类型定义。
using plugin_abi_fn_t = uint32_t (*)();
using plugin_create_fn_t = IBrokerAdapter* (*)();
using plugin_destroy_fn_t = void (*)(IBrokerAdapter*);

// 返回 broker_api 动态库版本号（随项目版本变化）。
ACCT_BROKER_API const char* broker_api_version() noexcept;
// 返回 ABI 版本号（接口/结构兼容性标识）。
ACCT_BROKER_API uint32_t broker_api_abi_version() noexcept;

}  // namespace acct_service::broker_api

// 插件实现方应导出以下 C 符号（名称与 kPlugin*Symbol 常量保持一致）。
extern "C" {
ACCT_BROKER_API uint32_t acct_broker_plugin_abi_version() noexcept;
ACCT_BROKER_API acct_service::broker_api::IBrokerAdapter* acct_create_broker_adapter() noexcept;
ACCT_BROKER_API void acct_destroy_broker_adapter(acct_service::broker_api::IBrokerAdapter* adapter) noexcept;
}
