#pragma once

#include "common/spinlock.hpp"
#include "common/types.hpp"
#include "order/order_request.hpp"
#include "portfolio/positions.h"
#include "shm/shm_layout.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace acct_service {

// 持仓变动记录
struct position_change {
    internal_order_id_t order_id;
    internal_security_id_t security_id;
    position_change_t change_type;
    volume_t volume;
    dprice_t price;
    dvalue_t value;
    dvalue_t fee;
    timestamp_ns_t timestamp;
};

// 持仓管理器
class position_manager {
public:
    explicit position_manager(positions_shm_layout* shm);
    ~position_manager() = default;

    // 禁止拷贝
    position_manager(const position_manager&) = delete;
    position_manager& operator=(const position_manager&) = delete;

    // 初始化（从数据库/文件加载）
    bool initialize(account_id_t account_id);

    // === 资金操作 ===

    dvalue_t get_available_fund() const noexcept;
    bool freeze_fund(dvalue_t amount, internal_order_id_t order_id);
    bool unfreeze_fund(dvalue_t amount, internal_order_id_t order_id);
    bool deduct_fund(dvalue_t amount, dvalue_t fee, internal_order_id_t order_id);
    bool add_fund(dvalue_t amount, internal_order_id_t order_id);

    // === 持仓操作 ===

    const position* get_position(internal_security_id_t security_id) const;
    position* get_position_mut(internal_security_id_t security_id);
    volume_t get_sellable_volume(internal_security_id_t security_id) const;
    bool freeze_position(internal_security_id_t security_id, volume_t volume,
                         internal_order_id_t order_id);
    bool unfreeze_position(internal_security_id_t security_id, volume_t volume,
                           internal_order_id_t order_id);
    bool deduct_position(internal_security_id_t security_id, volume_t volume,
                         dvalue_t value, internal_order_id_t order_id);
    bool add_position(internal_security_id_t security_id, volume_t volume,
                      dprice_t price, internal_order_id_t order_id);

    // === 查询接口 ===

    std::vector<const position*> get_all_positions() const;
    fund_info get_fund_info() const;
    std::size_t position_count() const noexcept;
    std::optional<internal_security_id_t> find_security_id(
        std::string_view code) const;
    internal_security_id_t add_security(std::string_view code,
                                        std::string_view name, market_t market);

private:
    position* get_or_create_position(internal_security_id_t security_id);
    void sync_fund_to_shm();

    positions_shm_layout* shm_;
    std::unordered_map<std::string, internal_security_id_t> code_to_id_;
    std::unordered_map<internal_security_id_t, std::size_t> id_to_index_;
    alignas(64) fund_info fund_cache_;
    mutable spinlock fund_lock_;
    internal_security_id_t next_security_id_ = 1;
};

}  // namespace acct
