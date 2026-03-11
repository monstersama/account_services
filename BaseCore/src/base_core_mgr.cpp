#include "base_core_mgr.hpp"

#include "logging/default_single_log.hpp"

BaseCoreMgr* BaseCoreMgr::instance() noexcept {
    static BaseCoreMgr mgr;
    return &mgr;
}

bool BaseCoreMgr::init() {
    auto& logger = base_core_log::DefaultSingleLog::instance();
    if (!logger.is_initialized()) {
        return logger.init();
    }
    return true;
}

void BaseCoreMgr::shutdown() noexcept {
    auto& logger = base_core_log::DefaultSingleLog::instance();
    if (logger.is_initialized()) {
        logger.shutdown();
    }
}
