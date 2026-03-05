#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common/fixed_string.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"

namespace acct_service {

// 委托记录（订单的持久化版本）
struct entrust_record {
    InternalOrderId order_id;
    InternalSecurityId security_id;
    order_type_t order_type;
    trade_side_t side;
    market_t market;
    OrderState status;
    Volume volume_entrust;
    Volume volume_traded;
    DPrice price_entrust;
    DPrice price_traded_avg;
    DValue value_traded;
    DValue fee;
    MdTime time_entrust;
    MdTime time_first_trade;
    MdTime time_last_update;
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
    bool load_today_entrusts(const std::string& db_path, AccountId account_id);

    // 添加/更新委托记录
    void add_or_update(const entrust_record& record);

    // 从 order_request 更新
    void update_from_order(const order_request& order);

    // 查询接口
    const entrust_record* find_entrust(InternalOrderId order_id) const;
    std::vector<const entrust_record*> get_entrusts_by_security(InternalSecurityId security_id) const;
    std::vector<const entrust_record*> get_active_entrusts() const;
    std::vector<const entrust_record*> get_all_entrusts() const;

    // 统计
    std::size_t entrust_count() const noexcept;
    std::size_t active_count() const noexcept;

    // 持久化
    bool save_to_db(const std::string& db_path) const;

private:
    std::vector<entrust_record> entrusts_;
    std::unordered_map<InternalOrderId, std::size_t> id_index_;
    std::unordered_map<InternalSecurityId, std::vector<std::size_t>> security_index_;
};

}  // namespace acct_service
