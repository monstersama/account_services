#pragma once

#include "common/fixed_string.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"

#include <string>

namespace acct {

// 账户信息
struct account_info {
    account_id_t account_id;
    account_type_t account_type;
    account_state_t state;
    fixed_string<32> account_name;
    fixed_string<32> broker_account;
    fixed_string<16> broker_code;

    // 交易权限
    bool can_buy = true;
    bool can_sell = true;
    bool can_short = false;
    bool can_margin = false;

    // 费率配置
    double commission_rate = 0.0003;
    double stamp_tax_rate = 0.001;
    double transfer_fee_rate = 0.00002;
    dvalue_t min_commission = 500;  // 5元 = 500分

    // 风控参数
    dvalue_t max_single_order = 0;
    dvalue_t max_daily_amount = 0;

    // 计算手续费
    dvalue_t calculate_fee(trade_side_t side, dvalue_t traded_value) const;
};

// 账户信息管理器
class account_info_manager {
public:
    account_info_manager() = default;
    ~account_info_manager() = default;

    // 从配置加载
    bool load_from_config(const std::string& config_path);

    // 从数据库加载
    bool load_from_db(const std::string& db_path, account_id_t account_id);

    // 获取账户信息
    const account_info& info() const noexcept;
    account_info& info() noexcept;

    // 检查交易权限
    bool can_trade(trade_side_t side) const noexcept;

    // 更新账户状态
    void set_state(account_state_t state);

private:
    account_info info_;
};

}  // namespace acct
