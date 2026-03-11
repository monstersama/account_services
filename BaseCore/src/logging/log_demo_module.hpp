#pragma once

#include <cstdint>

/**
 * 日志模块统一定义：Module 枚举 + logDemo Module 消息 ID 与格式串
 * 写端传 module_id + msg_id，logReader 据此查表输出可读文本
 */

namespace base_core_log {

    
// 模块 ID：logReader 据此输出模块名
enum class Module : uint8_t {
    orders_shm = 1,
    position_loader = 2,
    order_recovery = 3,
    order_book = 4,
    order_router = 5,
    event_loop = 6,
    config_manager = 7,
    shm_manager = 8,
    account_service = 9,
    api = 10,
    demo = 11,
    module1 = 12,
    module2 = 13,
    basecore_mgr = 14,
};

// module_id -> 模块名字符串（logReader 输出用）
inline const char* to_string(Module m) noexcept {
    switch (m) {
        case Module::orders_shm: return "orders_shm";
        case Module::position_loader: return "position_loader";
        case Module::order_recovery: return "order_recovery";
        case Module::order_book: return "order_book";
        case Module::order_router: return "order_router";
        case Module::event_loop: return "event_loop";
        case Module::config_manager: return "config_manager";
        case Module::shm_manager: return "shm_manager";
        case Module::account_service: return "account_service";
        case Module::api: return "api";
        case Module::demo: return "demo";
        case Module::module1: return "module1";
        case Module::module2: return "module2";
        case Module::basecore_mgr: return "basecore_mgr";
    }
    return "unknown";
}

}  // namespace base_core_log
