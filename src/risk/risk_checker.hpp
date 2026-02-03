#pragma once

#include "common/types.hpp"
#include "order/order_request.hpp"
#include "portfolio/position_manager.hpp"

#include <atomic>
#include <string>
#include <unordered_map>

namespace acct {

// 风控检查结果
struct risk_check_result {
    risk_result_t code = risk_result_t::Pass;
    std::string message;

    bool passed() const noexcept;
    static risk_check_result pass();
    static risk_check_result reject(risk_result_t code, std::string msg);
};

// 风控规则基类
class risk_rule {
public:
    virtual ~risk_rule() = default;
    virtual const char* name() const noexcept = 0;
    virtual risk_check_result check(const order_request& order,
                                    const position_manager& positions) = 0;
    virtual bool enabled() const noexcept;
    virtual void set_enabled(bool enabled);

protected:
    bool enabled_ = true;
};

// 资金检查
class fund_check_rule : public risk_rule {
public:
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
};

// 持仓检查
class position_check_rule : public risk_rule {
public:
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
};

// 单笔金额限制
class max_order_value_rule : public risk_rule {
public:
    explicit max_order_value_rule(dvalue_t max_value);
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
    void set_max_value(dvalue_t max_value);

private:
    dvalue_t max_value_;
};

// 单笔数量限制
class max_order_volume_rule : public risk_rule {
public:
    explicit max_order_volume_rule(volume_t max_volume);
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
    void set_max_volume(volume_t max_volume);

private:
    volume_t max_volume_;
};

// 涨跌停价格检查
class price_limit_rule : public risk_rule {
public:
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
    void set_price_limits(internal_security_id_t security_id, dprice_t limit_up,
                          dprice_t limit_down);
    void clear_price_limits();

private:
    std::unordered_map<internal_security_id_t, std::pair<dprice_t, dprice_t>>
        limits_;
};

// 重复订单检查
class duplicate_order_rule : public risk_rule {
public:
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
    void record_order(const order_request& order);
    void clear_history();
    void set_time_window_ns(timestamp_ns_t window_ns);

private:
    std::unordered_map<uint64_t, timestamp_ns_t> recent_orders_;
    timestamp_ns_t time_window_ns_ = 100'000'000;  // 100ms
};

// 流速限制
class rate_limit_rule : public risk_rule {
public:
    explicit rate_limit_rule(uint32_t max_orders_per_second);
    const char* name() const noexcept override;
    risk_check_result check(const order_request& order,
                            const position_manager& positions) override;
    void set_max_orders_per_second(uint32_t max);
    void reset_counter();

private:
    uint32_t max_orders_per_second_;
    std::atomic<uint32_t> current_second_count_{0};
    std::atomic<timestamp_ns_t> current_second_start_{0};
};

}  // namespace acct
