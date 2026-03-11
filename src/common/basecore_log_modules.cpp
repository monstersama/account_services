#include "common/basecore_log_modules.hpp"

#include <cctype>

namespace acct_service::basecore_log_adapter {

namespace {

// Matches module aliases while ignoring case and separator differences.
bool matches_module_alias(std::string_view actual, std::string_view expected) noexcept {
    std::size_t actual_index = 0;
    std::size_t expected_index = 0;

    while (true) {
        while (actual_index < actual.size() &&
               (actual[actual_index] == '_' || actual[actual_index] == '-' || actual[actual_index] == ' ')) {
            ++actual_index;
        }
        while (
            expected_index < expected.size() &&
            (expected[expected_index] == '_' || expected[expected_index] == '-' || expected[expected_index] == ' ')) {
            ++expected_index;
        }

        const bool actual_done = actual_index >= actual.size();
        const bool expected_done = expected_index >= expected.size();
        if (actual_done || expected_done) {
            return actual_done && expected_done;
        }

        const unsigned char actual_ch = static_cast<unsigned char>(actual[actual_index]);
        const unsigned char expected_ch = static_cast<unsigned char>(expected[expected_index]);
        if (std::tolower(actual_ch) != std::tolower(expected_ch)) {
            return false;
        }

        ++actual_index;
        ++expected_index;
    }
}

}  // namespace

// Returns the stable text name used by LogReader output for a module id.
const char* to_string(ProjectLogModule module) noexcept {
    switch (module) {
        case ProjectLogModule::Unknown:
            return "unknown";
        case ProjectLogModule::AccountService:
            return "account_service";
        case ProjectLogModule::Api:
            return "api";
        case ProjectLogModule::ConfigManager:
            return "config_manager";
        case ProjectLogModule::EventLoop:
            return "event_loop";
        case ProjectLogModule::Gateway:
            return "gateway";
        case ProjectLogModule::GatewayLoop:
            return "gateway_loop";
        case ProjectLogModule::OrderApi:
            return "order_api";
        case ProjectLogModule::OrderRecovery:
            return "order_recovery";
        case ProjectLogModule::OrderRouter:
            return "order_router";
        case ProjectLogModule::OrdersShm:
            return "orders_shm";
        case ProjectLogModule::PositionLoader:
            return "position_loader";
        case ProjectLogModule::PositionManager:
            return "position_manager";
        case ProjectLogModule::ShmManager:
            return "shm_manager";
        case ProjectLogModule::Test:
            return "test";
        case ProjectLogModule::TestLogger:
            return "test_logger";
    }
    return "unknown";
}

// Resolves existing module strings to a stable project log module id.
ProjectLogModule project_log_module_from_name(std::string_view module_name) noexcept {
    if (matches_module_alias(module_name, "account_service") || matches_module_alias(module_name, "AccountService")) {
        return ProjectLogModule::AccountService;
    }
    if (matches_module_alias(module_name, "api")) {
        return ProjectLogModule::Api;
    }
    if (matches_module_alias(module_name, "config_manager") || matches_module_alias(module_name, "ConfigManager")) {
        return ProjectLogModule::ConfigManager;
    }
    if (matches_module_alias(module_name, "event_loop") || matches_module_alias(module_name, "EventLoop")) {
        return ProjectLogModule::EventLoop;
    }
    if (matches_module_alias(module_name, "gateway")) {
        return ProjectLogModule::Gateway;
    }
    if (matches_module_alias(module_name, "gateway_loop")) {
        return ProjectLogModule::GatewayLoop;
    }
    if (matches_module_alias(module_name, "order_api")) {
        return ProjectLogModule::OrderApi;
    }
    if (matches_module_alias(module_name, "order_recovery")) {
        return ProjectLogModule::OrderRecovery;
    }
    if (matches_module_alias(module_name, "order_router")) {
        return ProjectLogModule::OrderRouter;
    }
    if (matches_module_alias(module_name, "orders_shm")) {
        return ProjectLogModule::OrdersShm;
    }
    if (matches_module_alias(module_name, "position_loader")) {
        return ProjectLogModule::PositionLoader;
    }
    if (matches_module_alias(module_name, "position_manager") || matches_module_alias(module_name, "PositionManager")) {
        return ProjectLogModule::PositionManager;
    }
    if (matches_module_alias(module_name, "shm_manager")) {
        return ProjectLogModule::ShmManager;
    }
    if (matches_module_alias(module_name, "test_logger")) {
        return ProjectLogModule::TestLogger;
    }
    if (matches_module_alias(module_name, "test")) {
        return ProjectLogModule::Test;
    }
    return ProjectLogModule::Unknown;
}

// Adapts project module ids to the callback shape expected by LogReader.
const char* project_log_module_mapper(uint8_t module_id) noexcept {
    return to_string(static_cast<ProjectLogModule>(module_id));
}

}  // namespace acct_service::basecore_log_adapter
