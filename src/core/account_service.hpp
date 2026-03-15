#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "common/error.hpp"
#include "core/config_manager.hpp"
#include "core/event_loop.hpp"
#include "execution/execution_engine.hpp"
#include "market_data/market_data_service.hpp"
#include "order/order_book.hpp"
#include "order/order_router.hpp"
#include "portfolio/account_info.hpp"
#include "portfolio/entrust_record.hpp"
#include "portfolio/position_manager.hpp"
#include "portfolio/trade_record.hpp"
#include "risk/risk_manager.hpp"
#include "shm/shm_manager.hpp"

namespace acct_service {

// 账户服务状态
enum class ServiceState {
    Created,
    Initializing,
    Ready,
    Running,
    Stopping,
    Stopped,
    Error,
};

bool should_log_startup_config() noexcept;

// 账户服务主类
class AccountService {
public:
    AccountService();
    ~AccountService();

    // 禁止拷贝
    AccountService(const AccountService&) = delete;
    AccountService& operator=(const AccountService&) = delete;

    // 初始化服务
    bool initialize(const std::string& config_path);

    // 运行服务（阻塞）
    int run();

    // 请求停止
    void stop();

    // 获取服务状态
    ServiceState state() const noexcept;

    // 获取各组件引用（用于测试）
    const ConfigManager& config() const;
    const OrderBook& orders() const;
    const PositionManager& positions() const;
    const RiskManager& risk() const;

    // 获取统计信息
    void print_stats() const;

    bool has_fatal_error() const noexcept;
    const ErrorStatus& last_error() const noexcept;

private:
    // 初始化各组件
    bool init_config(const std::string& config_path);
    bool init_shared_memory();
    bool init_portfolio();
    bool init_risk_manager();
    bool init_order_components();
    bool init_market_data();
    bool init_execution_engine();
    bool init_event_loop();

    // 加载历史数据
    bool load_account_info();
    bool load_positions();
    bool load_today_trades();
    bool load_today_entrusts();

    // 清理资源
    void cleanup();
    void print_loaded_config() const;
    void raise_service_error(const ErrorStatus& status);
    bool should_terminate_due_to_error() const noexcept;

    mutable ErrorStatus last_error_{};
    std::atomic<ErrorSeverity> shutdown_reason_{ErrorSeverity::Recoverable};

    std::atomic<ServiceState> state_{ServiceState::Created};

    // 配置
    ConfigManager config_manager_;

    // 共享内存管理
    SHMManager upstream_shm_manager_;
    SHMManager downstream_shm_manager_;
    SHMManager trades_shm_manager_;
    SHMManager orders_shm_manager_;
    SHMManager positions_shm_manager_;

    // 共享内存指针
    upstream_shm_layout* upstream_shm_ = nullptr;
    downstream_shm_layout* downstream_shm_ = nullptr;
    trades_shm_layout* trades_shm_ = nullptr;
    orders_shm_layout* orders_shm_ = nullptr;
    positions_shm_layout* positions_shm_ = nullptr;

    // 核心组件
    std::unique_ptr<account_info_manager> account_info_;
    std::unique_ptr<PositionManager> position_manager_;
    std::unique_ptr<OrderBook> order_book_;
    std::unique_ptr<order_router> order_router_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::unique_ptr<MarketDataService> market_data_service_;
    std::unique_ptr<ExecutionEngine> execution_engine_;
    std::unique_ptr<trade_record_manager> trade_records_;
    std::unique_ptr<entrust_record_manager> entrust_records_;

    // 事件循环
    std::unique_ptr<EventLoop> event_loop_;
};

}  // namespace acct_service
