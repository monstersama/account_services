#pragma once

#include "core/config_manager.hpp"
#include "core/event_loop.hpp"
#include "order/order_book.hpp"
#include "order/order_router.hpp"
#include "portfolio/account_info.hpp"
#include "portfolio/entrust_record.hpp"
#include "portfolio/position_manager.hpp"
#include "portfolio/trade_record.hpp"
#include "risk/risk_manager.hpp"
#include "shm/shm_manager.hpp"

#include <memory>
#include <string>

namespace acct_service {

// 账户服务状态
enum class service_state_t {
    Created,
    Initializing,
    Ready,
    Running,
    Stopping,
    Stopped,
    Error,
};

// 账户服务主类
class account_service {
public:
    account_service();
    ~account_service();

    // 禁止拷贝
    account_service(const account_service&) = delete;
    account_service& operator=(const account_service&) = delete;

    // 初始化服务
    bool initialize(const std::string& config_path);
    bool initialize(const config& cfg);

    // 运行服务（阻塞）
    int run();

    // 请求停止
    void stop();

    // 获取服务状态
    service_state_t state() const noexcept;

    // 获取各组件引用（用于测试）
    const config_manager& config() const;
    const order_book& orders() const;
    const position_manager& positions() const;
    const risk_manager& risk() const;

    // 获取统计信息
    void print_stats() const;

private:
    // 初始化各组件
    bool init_config(const std::string& config_path);
    bool init_shared_memory();
    bool init_portfolio();
    bool init_risk_manager();
    bool init_order_components();
    bool init_event_loop();

    // 加载历史数据
    bool load_account_info();
    bool load_positions();
    bool load_today_trades();
    bool load_today_entrusts();

    // 清理资源
    void cleanup();

    service_state_t state_ = service_state_t::Created;

    // 配置
    config_manager config_manager_;

    // 共享内存管理
    shm_manager upstream_shm_manager_;
    shm_manager downstream_shm_manager_;
    shm_manager positions_shm_manager_;

    // 共享内存指针
    upstream_shm_layout* upstream_shm_ = nullptr;
    downstream_shm_layout* downstream_shm_ = nullptr;
    positions_shm_layout* positions_shm_ = nullptr;

    // 核心组件
    std::unique_ptr<account_info_manager> account_info_;
    std::unique_ptr<position_manager> position_manager_;
    std::unique_ptr<order_book> order_book_;
    std::unique_ptr<order_router> order_router_;
    std::unique_ptr<risk_manager> risk_manager_;
    std::unique_ptr<trade_record_manager> trade_records_;
    std::unique_ptr<entrust_record_manager> entrust_records_;

    // 事件循环
    std::unique_ptr<event_loop> event_loop_;
};

}  // namespace acct
