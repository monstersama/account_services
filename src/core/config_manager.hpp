#pragma once

#include <string>

#include "common/types.hpp"
#include "order/order_splitter.hpp"
#include "risk/risk_manager.hpp"

namespace acct_service {

// 共享内存配置
struct shm_config {
    std::string upstream_shm_name = "/strategy_order_shm";
    std::string downstream_shm_name = "/downstream_order_shm";
    std::string trades_shm_name = "/trades_shm";
    std::string orders_shm_name = "/orders_shm";
    std::string positions_shm_name = "/positions_shm";
    bool create_if_not_exist = true;
};

// 事件循环配置
struct event_loop_config {
    bool busy_polling = true;
    uint32_t poll_batch_size = 64;
    uint32_t idle_sleep_us = 0;
    uint32_t stats_interval_ms = 1000;
    bool pin_cpu = false;
    int cpu_core = -1;
};

// 日志配置
struct log_config {
    std::string log_dir = "./logs";
    std::string log_level = "info";
    bool async_logging = true;
    std::size_t async_queue_size = 8192;
};

// 数据库配置
struct db_config {
    std::string db_path;
    bool enable_persistence = true;
    uint32_t sync_interval_ms = 1000;
};

// 完整配置
struct config {
    account_id_t account_id = 1;
    std::string trading_day = "19700101";
    std::string config_file;

    shm_config shm;
    event_loop_config event_loop;
    risk_config risk;
    split_config split;
    log_config log;
    db_config db;
};

// 配置管理器
class config_manager {
public:
    config_manager() = default;
    ~config_manager() = default;

    // 从YAML文件加载配置
    bool load_from_file(const std::string& config_path);

    // 从命令行参数解析
    bool parse_command_line(int argc, char* argv[]);

    // 验证配置有效性
    bool validate() const;

    // 获取配置
    const config& get() const noexcept;
    config& get() noexcept;

    // 便捷访问器
    account_id_t account_id() const noexcept;
    const shm_config& shm() const noexcept;
    const event_loop_config& event_loop() const noexcept;
    const risk_config& risk() const noexcept;
    const split_config& split() const noexcept;
    const log_config& log() const noexcept;
    const db_config& db() const noexcept;

    // 热更新支持
    bool reload();

    // 导出当前配置
    bool export_to_file(const std::string& path) const;

private:
    config config_;
    std::string config_path_;
};

}  // namespace acct_service
