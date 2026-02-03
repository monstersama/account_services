#pragma once

#include "common/fixed_string.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace acct {

// 委托记录（订单的持久化版本）
struct entrust_record {
    internal_order_id_t order_id;
    internal_security_id_t security_id;
    order_type_t order_type;
    trade_side_t side;
    market_t market;
    order_status_t status;
    volume_t volume_entrust;
    volume_t volume_traded;
    dprice_t price_entrust;
    dprice_t price_traded_avg;
    dvalue_t value_traded;
    dvalue_t fee;
    md_time_t time_entrust;
    md_time_t time_first_trade;
    md_time_t time_last_update;
    fixed_string<32> broker_order_id;
    fixed_string<16> security_code;

    // 从 order_request 转换
    static entrust_record from_order_request(const order_request& req);
};

// 委托记录管理器
class entrust_record_manager {
public:
    entrust_record_manager() = default;
    ~entrust_record_manager() = default;

    // 从数据库加载当日委托
    bool load_today_entrusts(const std::string& db_path, account_id_t account_id);

    // 添加/更新委托记录
    void add_or_update(const entrust_record& record);

    // 从 order_request 更新
    void update_from_order(const order_request& order);

    // 查询接口
    const entrust_record* find_entrust(internal_order_id_t order_id) const;
    std::vector<const entrust_record*> get_entrusts_by_security(
        internal_security_id_t security_id) const;
    std::vector<const entrust_record*> get_active_entrusts() const;
    std::vector<const entrust_record*> get_all_entrusts() const;

    // 统计
    std::size_t entrust_count() const noexcept;
    std::size_t active_count() const noexcept;

    // 持久化
    bool save_to_db(const std::string& db_path) const;

private:
    std::vector<entrust_record> entrusts_;
    std::unordered_map<internal_order_id_t, std::size_t> id_index_;
    std::unordered_map<internal_security_id_t, std::vector<std::size_t>>
        security_index_;
};

}  // namespace acct
