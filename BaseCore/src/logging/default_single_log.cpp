#include "logging/default_single_log.hpp"

namespace base_core_log {

DefaultSingleLog& DefaultSingleLog::instance() noexcept {
    static DefaultSingleLog inst;
    return inst;
}

bool DefaultSingleLog::init() {
    ShmLogConfig cfg;
    // 默认路径放在 /tmp，兼容 SHM_USE_FILE=1（文件后端）与普通权限运行
    cfg.shm_name = "/tmp/basecore_default_log";
    cfg.output_path = "/tmp/basecore_default_log.txt";
    cfg.per_thread = false;
    return init(cfg);
}

bool DefaultSingleLog::init(const ShmLogConfig& config) {
    shutdown();
    config_ = config;
    if (!init_shm_logger(config_, logger_)) {
        return false;
    }
    auto* w = logger_.writer();
    return w != nullptr && w->is_attached();
}

void DefaultSingleLog::shutdown() noexcept {
    logger_.close();
}

bool DefaultSingleLog::is_initialized() const noexcept {
    return logger_.is_open();
}

LogBufferWriter* DefaultSingleLog::writer() noexcept {
    return logger_.writer();
}

bool DefaultSingleLog::log_str(LogLevel level, uint8_t module_id, const char* msg, std::size_t len) {
    if (!msg) return false;
    auto* w = writer();
    if (!w) return false;
    return log_write_str(*w, level, rdtsc(), module_id, msg, len);
}

bool DefaultSingleLog::log_str(LogLevel level, uint8_t module_id, std::string_view msg) {
    return log_str(level, module_id, msg.data(), msg.size());
}

DefaultSingleLog::~DefaultSingleLog() noexcept {
    shutdown();
}

}  // namespace base_core_log

