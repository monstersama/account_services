#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common/fixed_string.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"

namespace acct_service {

// 成交记录
struct alignas(64) trade_record {
    uint64_t trade_id;
    InternalOrderId order_id;
    InternalSecurityId security_id;
    TradeSide side;
    Volume volume;
    DPrice price;
    DValue value;
    DValue fee;
    MdTime trade_time;
    TimestampNs local_time;
    FixedString<32> broker_trade_id;
};

// 成交记录管理器
class trade_record_manager {
public:
    trade_record_manager() = default;
    ~trade_record_manager() = default;

    // 从数据库加载当日成交
    bool load_today_trades(const std::string& db_path, AccountId account_id);

    // 添加成交记录
    void add_trade(const trade_record& record);

    // 查询接口
    const trade_record* find_trade(uint64_t trade_id) const;
    std::vector<const trade_record*> get_trades_by_order(InternalOrderId order_id) const;
    std::vector<const trade_record*> get_trades_by_security(InternalSecurityId security_id) const;
    std::vector<const trade_record*> get_all_trades() const;

    // 统计接口
    std::size_t trade_count() const noexcept;
    DValue total_traded_value() const noexcept;
    DValue total_fee() const noexcept;

    // 持久化
    bool save_to_db(const std::string& db_path) const;

private:
    std::vector<trade_record> trades_;
    std::unordered_map<uint64_t, std::size_t> id_index_;
    std::unordered_map<InternalOrderId, std::vector<std::size_t>> order_index_;
    std::unordered_map<InternalSecurityId, std::vector<std::size_t>> security_index_;
    uint64_t next_trade_id_ = 1;
};

}  // namespace acct_service
