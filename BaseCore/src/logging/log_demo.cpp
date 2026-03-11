/**
 * log_demo: 演示 LogText、LogFormatFunc、LOG_TEXT、LOG_FMT、LOG_STR
 * 写入日志后调用 LogReader 生成文件并打印
 */

#include "logging/log.hpp"
#include "logging/log_demo_format_func.hpp"
#include "logging/log_demo_module.hpp"
#include "logging/default_single_log.hpp"
#include "base_core_mgr.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// 默认模块名映射函数（仅 demo 使用，不影响系统代码）
const char* default_module_mapper(uint8_t module_id) {
    if (module_id == 0) return "unknown";
    return base_core_log::to_string(static_cast<base_core_log::Module>(module_id));
}

// 使用 DefaultSingleLog + LOG_FMT_DEFAULT / LOG_STR_DEFAULT 的演示
bool run_default_single_log_demo() {
    using namespace base_core_log;

    // 1. 通过 BaseCoreMgr 触发 DefaultSingleLog 初始化
    auto* mgr = BaseCoreMgr::instance();
    if (!mgr->init()) {
        std::cerr << "log_demo: BaseCoreMgr init failed\n";
        return false;
    }

    // 2. 使用宏 LOG_FMT_DEFAULT / LOG_STR_DEFAULT 写日志
    LOG_FMT_DEFAULT(LogLevel::info, Module::module1, LogFormatFunc::format1, 10, 20);
    LOG_STR_DEFAULT(LogLevel::info, Module::module1, "hello from LOG_STR_DEFAULT");

    // 3. 使用 LogReader 读取 DefaultSingleLog 写入的日志
    const auto& cfg = DefaultSingleLog::instance().config();
    LogReader reader(cfg.shm_name, cfg.output_path, default_module_mapper);
    if (reader.run() != 0) {
        std::cerr << "log_demo: DefaultSingleLog LogReader failed\n";
        return false;
    }

    // 4. 打印日志文本文件
    std::ifstream in(cfg.output_path);
    if (!in) {
        std::cerr << "log_demo: failed to open " << cfg.output_path << "\n";
        return false;
    }
    std::cout << "log_demo: default_single_log content:\n";
    std::cout << "---\n";
    std::string line;
    while (std::getline(in, line)) {
        std::cout << line << "\n";
    }
    std::cout << "---\n";
    return true;
}

}  // namespace

int main() {
    //1.初始化
    base_core_log::ShmLogConfig config;
    config.shm_name = "/tmp/log_shm";
    config.output_path = "./log_output.txt";
    config.per_thread = true;

    base_core_log::ShmLogger logger;
    if (!base_core_log::init_shm_logger(config, logger)) return 1;

    auto* w = logger.writer();

    //2.写日志
    using namespace base_core_log;

    // 文本：LOG_TEXT(writer, level, module_id, text_id)
    //LOG_TEXT(*w, LogLevel::info, Module::module1, LogText::Text1);

    // 格式函数：LOG_FMT(writer, level, module_id, format_name, ...)
    // 注意：这里只能传数值类型，如果传const char* 会直接拷贝指针
    LOG_FMT(*w, LogLevel::info, Module::module1, LogFormatFunc::format1, 3, 2);
    LOG_FMT(*w, LogLevel::info, Module::module2, LogFormatFunc::format2, 3);

    // 字符串：LOG_STR(writer, level, module_id, "literal") 或 log_write_str(..., string_view)
    LOG_STR(*w, LogLevel::info, Module::module1, "hello from LOG_STR");
    std::string text = "hello from LOG_STR2";
    LOG_STR(*w, LogLevel::info, Module::module1, text.c_str());
    // 长字符串（>32 字节）走变长区
    LOG_STR(*w, LogLevel::info, Module::module1,
            "This is a long message that exceeds 32 bytes to test variable-length path");
 
    logger.close();

    //3.生成日志文本文件
    LogReader reader(config.shm_name, config.output_path, default_module_mapper);
    if (reader.run() != 0) {
        std::cerr << "log_demo: LogReader failed\n";
        return 1;
    }
 
    //4.打印日志文本文件
    std::ifstream in(config.output_path);
    if (!in) {
        std::cerr << "log_demo: failed to open " << config.output_path << "\n";
        return 1;
    }
    std::cout << "log_demo: log content:\n";
    std::cout << "---\n";
    std::string line;
    while (std::getline(in, line)) {
        std::cout << line << "\n";
    }
    std::cout << "---\n";

    // 5. 运行基于 DefaultSingleLog 的 demo
    if (!run_default_single_log_demo()) {
        return 1;
    }
    return 0;
}
