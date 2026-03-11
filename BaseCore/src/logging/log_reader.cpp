/**
 * log_reader: 从共享内存读取日志，将 TSC 转为可读时间，写入文本文件
 * 用法: log_reader [shm_name] [output_path]
 * 默认: shm_name=/log_shm, output_path=./log_output.txt
 */

#include "logging/log.hpp"
#include "logging/log_demo_format_func.hpp"
#include "logging/log_demo_module.hpp"

#include <cstring>
#include <iostream>
#include <string>

namespace {

// 默认模块名映射函数
const char* default_module_mapper(uint8_t module_id) {
    if (module_id == 0) return "unknown";
    // 使用 demo 模块的 to_string 函数
    return base_core_log::to_string(static_cast<base_core_log::Module>(module_id));
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string shm_name = "/tmp/log_shm";
    std::string output_path = "./log_output.txt";

    if (argc >= 2) {
        if (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
            std::cerr << "Usage: " << argv[0] << " [shm_name] [output_path]\n"
                      << "  shm_name:    shared memory name (default: /tmp/log_shm)\n"
                      << "  output_path: output text file (default: ./log_output.txt)\n";
            return 0;
        }
        shm_name = argv[1];
    }
    if (argc >= 3) {
        output_path = argv[2];
    }

    base_core_log::LogReader reader(shm_name, output_path, default_module_mapper);
    return reader.run();
}
