#pragma once

#include <string>
#include <string_view>

#include "common/types.hpp"
#include "shm/shm_layout.hpp"

namespace acct_service {

// 共享内存访问模式
enum class shm_mode {
    Create,        // 创建新的共享内存
    Open,          // 打开已存在的共享内存
    OpenOrCreate,  // 打开或创建
};

// 共享内存管理器
class SHMManager {
public:
    SHMManager();
    ~SHMManager();

    // 禁止拷贝
    SHMManager(const SHMManager&) = delete;
    SHMManager& operator=(const SHMManager&) = delete;

    // 移动构造/赋值
    SHMManager(SHMManager&& other) noexcept;
    SHMManager& operator=(SHMManager&& other) noexcept;

    // 创建/打开上游共享内存
    upstream_shm_layout* open_upstream(std::string_view name, shm_mode mode, account_id_t account_id);

    // 创建/打开下游共享内存
    downstream_shm_layout* open_downstream(std::string_view name, shm_mode mode, account_id_t account_id);

    // 创建/打开成交回报共享内存
    trades_shm_layout* open_trades(std::string_view name, shm_mode mode, account_id_t account_id);

    // 创建/打开持仓共享内存
    positions_shm_layout* open_positions(std::string_view name, shm_mode mode, account_id_t account_id);

    // 关闭并解除映射
    void close();

    // 删除共享内存对象
    static bool unlink(std::string_view name);

    // 检查是否已打开
    bool is_open() const noexcept;

    // 获取共享内存名称
    const std::string& name() const noexcept;

private:
    // 内部实现：打开或创建共享内存
    void* open_impl(std::string_view name, std::size_t size, shm_mode mode);

    // 初始化共享内存头部(暂时不用account_id,只做预留)
    void init_header(shm_header* header, account_id_t account_id);

    // 验证共享内存头部
    bool validate_header(const shm_header* header);

    std::string name_;
    void* ptr_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
    bool last_open_is_new_ = false;
};

// 便捷函数：生成共享内存名称
inline std::string make_shm_name(std::string_view prefix, account_id_t account_id) {
    return std::string("/") + std::string(prefix) + "_" + std::to_string(account_id);
}

}  // namespace acct_service
