#pragma once

#include <cstddef>
#include <string>

namespace shm {
namespace internal {

// ============ 配置 ============

// 检查是否使用文件后端（通过环境变量 SHM_USE_FILE=1 控制）
bool use_file_backend();

// ============ 创建共享内存 ============

// 创建共享内存底层实现
// fail_if_exists: true=仅创建新的，已存在则失败；false=幂等，已存在则验证大小
bool create_shm_impl(const std::string& name, std::size_t size, bool fail_if_exists);

// ============ 打开并映射共享内存 ============

// 以读写方式打开并映射共享内存
// 成功返回映射指针，失败返回 nullptr
void* open_shm_write_impl(const std::string& name, std::size_t size);

// 以只读方式打开并映射共享内存
// 成功返回映射指针，失败返回 nullptr
const void* open_shm_read_impl(const std::string& name, std::size_t size);

// ============ 日志回调（可选）===========

// 设置错误日志回调函数，如果不设置则使用默认（输出到stderr）
using LogErrorFunc = void (*)(const std::string& msg);
void set_log_error_func(LogErrorFunc func);

// 内部使用的日志函数
void log_error(const std::string& msg) noexcept;

} // namespace internal
} // namespace shm
