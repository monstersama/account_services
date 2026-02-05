#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "common/types.hpp"
#include "order/order_request.hpp"
#include "portfolio/position_manager.hpp"
#include "risk/risk_checker.hpp"

namespace acct_service {

// 风控配置
struct risk_config {
    dvalue_t max_order_value = 0;
    volume_t max_order_volume = 0;
    dvalue_t max_daily_turnover = 0;
    uint32_t max_orders_per_second = 0;
    bool enable_price_limit_check = true;
    bool enable_duplicate_check = true;
    bool enable_fund_check = true;
    bool enable_position_check = true;
    timestamp_ns_t duplicate_window_ns = 100'000'000;
};

// 风控统计
struct risk_stats {
    uint64_t total_checks = 0;
    uint64_t passed = 0;
    uint64_t rejected = 0;
    uint64_t rejected_fund = 0;
    uint64_t rejected_position = 0;
    uint64_t rejected_price = 0;
    uint64_t rejected_value = 0;
    uint64_t rejected_volume = 0;
    uint64_t rejected_duplicate = 0;
    uint64_t rejected_rate_limit = 0;
    timestamp_ns_t last_check_time = 0;

    void reset();
};

// 风控管理器
class risk_manager {
public:
    risk_manager(position_manager& positions, const risk_config& config);
    ~risk_manager() = default;

    // 禁止拷贝
    risk_manager(const risk_manager&) = delete;
    risk_manager& operator=(const risk_manager&) = delete;

    // 风控检查
    risk_check_result check_order(const order_request& order);
    std::vector<risk_check_result> check_orders(const std::vector<order_request>& orders);

    // 回调
    using post_check_callback_t = std::function<void(const order_request&, const risk_check_result&)>;
    void set_post_check_callback(post_check_callback_t callback);

    // 规则管理
    void add_rule(std::unique_ptr<risk_rule> rule);
    bool remove_rule(const char* name);
    bool enable_rule(const char* name, bool enabled);
    risk_rule* get_rule(const char* name);
    const risk_rule* get_rule(const char* name) const;

    // 涨跌停价格
    void update_price_limits(internal_security_id_t security_id, dprice_t limit_up, dprice_t limit_down);
    void clear_price_limits();

    // 配置
    void update_config(const risk_config& config);
    const risk_config& config() const noexcept;

    // 统计
    const risk_stats& stats() const noexcept;
    void reset_stats() noexcept;

private:
    void initialize_default_rules();
    void update_stats(const risk_check_result& result);

    position_manager& positions_;
    risk_config config_;
    std::vector<std::unique_ptr<risk_rule>> rules_;
    risk_stats stats_;
    post_check_callback_t post_check_callback_;
    price_limit_rule* price_limit_rule_ = nullptr;
    duplicate_order_rule* duplicate_rule_ = nullptr;
    rate_limit_rule* rate_limit_rule_ = nullptr;
};

}  // namespace acct_service
