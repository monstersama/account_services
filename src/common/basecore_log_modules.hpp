#pragma once

#include <cstdint>
#include <string_view>

namespace acct_service::basecore_log_adapter {

enum class ProjectLogModule : uint8_t {
    Unknown = 0,
    AccountService = 1,
    Api = 2,
    ConfigManager = 3,
    EventLoop = 4,
    Gateway = 5,
    GatewayLoop = 6,
    OrderApi = 7,
    OrderRecovery = 8,
    OrderRouter = 9,
    OrdersShm = 10,
    PositionLoader = 11,
    PositionManager = 12,
    ShmManager = 13,
    Test = 14,
    TestLogger = 15,
};

// Returns the stable text name used by LogReader output for a module id.
const char* to_string(ProjectLogModule module) noexcept;

// Resolves existing module strings to a stable project log module id.
ProjectLogModule project_log_module_from_name(std::string_view module_name) noexcept;

// Adapts project module ids to the callback shape expected by LogReader.
const char* project_log_module_mapper(uint8_t module_id) noexcept;

}  // namespace acct_service::basecore_log_adapter
