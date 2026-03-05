#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/types.hpp"
#include "order/order_request.hpp"
#include "portfolio/positions.h"
#include "shm/shm_layout.hpp"

namespace acct_service {

// 持仓变动记录
struct PositionChangeRecord {
    InternalOrderId order_id;
    InternalSecurityId security_id;
    PositionChange change_type;
    Volume volume;
    DPrice price;
    DValue value;
    DValue fee;
    TimestampNs timestamp;
};

// 持仓管理器
class PositionManager {
public:
    explicit PositionManager(
        positions_shm_layout* shm, std::string config_file_path = {}, std::string db_path = {}, bool db_enabled = false);
    ~PositionManager() = default;

    // 禁止拷贝
    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    // 初始化（新建/未完成初始化 SHM 时内部触发 loader 从外部加载）
    bool initialize(AccountId account_id);

    // === 资金操作 ===

    DValue get_available_fund() const noexcept;
    bool freeze_fund(DValue amount, InternalOrderId order_id);
    bool unfreeze_fund(DValue amount, InternalOrderId order_id);
    bool deduct_fund(DValue amount, DValue fee, InternalOrderId order_id);
    bool add_fund(DValue amount, InternalOrderId order_id);
    // 按成交结果更新买入资金（available/total_asset/market_value）。
    bool apply_buy_trade_fund(DValue amount, DValue fee, InternalOrderId order_id);
    // 按成交结果更新卖出资金（available/total_asset/market_value）。
    bool apply_sell_trade_fund(DValue amount, DValue fee, InternalOrderId order_id);

    // === 持仓操作 ===

    const position* get_position(InternalSecurityId security_id) const;
    position* get_position_mut(InternalSecurityId security_id);
    Volume get_sellable_volume(InternalSecurityId security_id) const;
    bool freeze_position(InternalSecurityId security_id, Volume volume, InternalOrderId order_id);
    bool unfreeze_position(InternalSecurityId security_id, Volume volume, InternalOrderId order_id);
    bool deduct_position(
        InternalSecurityId security_id, Volume volume, DValue value, InternalOrderId order_id);
    bool add_position(
        InternalSecurityId security_id, Volume volume, DPrice price, InternalOrderId order_id);

    // === 查询接口 ===
    std::vector<const position*> get_all_positions() const;
    fund_info get_fund_info() const;
    // 覆盖 FUND 行快照（仅用于初始化加载）。
    bool overwrite_fund_info(const fund_info& fund);
    std::size_t position_count() const noexcept;
    std::optional<InternalSecurityId> find_security_id(std::string_view code) const;
    InternalSecurityId add_security(std::string_view code, std::string_view name, Market market);

private:
    positions_shm_layout* shm_;
    std::unordered_map<InternalSecurityId, std::size_t> security_to_row_;
    std::string config_file_path_;
    std::string db_path_;
    bool db_enabled_{false};
};

}  // namespace acct_service
