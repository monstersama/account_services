#pragma once

#include "common/types.hpp"
#include "order/order_request.hpp"

#include <functional>
#include <string>
#include <vector>

namespace acct_service {

// 拆单配置
struct split_config {
    split_strategy_t strategy = split_strategy_t::None;
    volume_t max_child_volume = 0;
    volume_t min_child_volume = 100;
    uint32_t max_child_count = 100;
    uint32_t interval_ms = 0;
    double randomize_factor = 0.0;
};

// 拆单结果
struct split_result {
    bool success = false;
    std::vector<order_request> child_orders;
    std::string error_msg;
};

// 拆单器
class order_splitter {
public:
    explicit order_splitter(const split_config& config);
    ~order_splitter() = default;

    // 设置订单ID生成器
    using order_id_generator_t = std::function<internal_order_id_t()>;
    void set_order_id_generator(order_id_generator_t gen);

    // 执行拆单
    split_result split(const order_request& parent_order);

    // 判断是否需要拆单
    bool should_split(const order_request& order) const;

    // 更新拆单配置
    void update_config(const split_config& config);

    const split_config& config() const noexcept;

private:
    split_result split_fixed_size(const order_request& parent);
    split_result split_iceberg(const order_request& parent);
    split_result split_twap(const order_request& parent);

    split_config config_;
    order_id_generator_t id_generator_;
};

}  // namespace acct
