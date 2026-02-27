#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "common/types.hpp"

namespace acct_service {

enum class order_type_t : uint8_t {
    NotSet = 0,
    New = 1,
    Cancel = 2,
    Unknown = 0xFF,
};

enum class trade_side_t : uint8_t {
    NotSet = 0,
    Buy = 1,
    Sell = 2,
};

enum class market_t : uint8_t {
    NotSet = 0,
    SZ = 1,
    SH = 2,
    BJ = 3,
    HK = 4,
    Unknown = 0xFF,
};

enum class order_status_t : uint8_t {
    NotSet = 0,
    StrategySubmitted = 0x12,
    RiskControllerPending = 0x20,
    RiskControllerRejected = 0x21,
    RiskControllerAccepted = 0x22,
    TraderPending = 0x30,
    TraderRejected = 0x31,
    TraderSubmitted = 0x32,
    TraderError = 0x33,
    BrokerRejected = 0x41,
    BrokerAccepted = 0x42,
    MarketRejected = 0x51,
    MarketAccepted = 0x52,
    Finished = 0x62,
    Unknown = 0xFF,
};

struct alignas(64) order_request {
    // cache line 0
    internal_order_id_t internal_order_id{0};  // 系统内部订单ID，唯一标识
    uint8_t padding0_0{0};
    order_type_t order_type{order_type_t::NotSet};  // 订单类型：报单、撤单
    trade_side_t trade_side{trade_side_t::NotSet};  // 买卖方向：买入、卖出
    market_t market{market_t::NotSet};              // 市场：深、沪、北、港
    volume_t volume_entrust{0};                     // 委托数量
    dprice_t dprice_entrust{0};                     // 委托价格
    internal_order_id_t orig_internal_order_id{0};  // 原始订单ID（仅撤单请求使用）
    internal_security_id_t internal_security_id{};  // 内部证券ID（格式: "SZ.000001"）
    security_id_t security_id{};                    // 证券代码字符串（如 "000001"）
    uint8_t padding0_1[4]{};

    // cache line 1
    union {
        broker_order_id_t as_str{};  // 柜台返回的订单ID（字符串）
        uint64_t as_uint;            // 柜台返回的订单ID（数字）
    } broker_order_id;
    volume_t volume_traded{0};  // 已成交数量
    volume_t volume_remain{0};  // 剩余未成交数量
    dvalue_t dvalue_traded{0};  // 已成交金额
    dprice_t dprice_traded{0};  // 成交均价

    // cache line 2
    dvalue_t dfee_estimate{0};                                         // 预估手续费
    dvalue_t dfee_executed{0};                                         // 已实际产生的手续费
    md_time_t md_time_driven{0};                                       // 触发时间
    md_time_t md_time_entrust{0};                                      // 委托结束时间
    md_time_t md_time_cancel_sent{0};                                  // 撤单发送时间（本地时间）
    md_time_t md_time_cancel_done{0};                                  // 撤单完成时间（来自柜台/市场）
    md_time_t md_time_broker_response{0};                              // 柜台响应时间
    md_time_t md_time_market_response{0};                              // 交易所响应时间
    md_time_t md_time_traded_first{0};                                 // 首次成交时间
    md_time_t md_time_traded_latest{0};                                // 最近成交时间
    std::atomic<order_status_t> order_status{order_status_t::NotSet};  // 订单当前状态
    uint8_t padding2[15]{};                                            // 填充以对齐缓存行

    order_request() = default;
    order_request(const order_request& other) {
        internal_order_id = other.internal_order_id;
        padding0_0 = other.padding0_0;
        order_type = other.order_type;
        trade_side = other.trade_side;
        market = other.market;
        volume_entrust = other.volume_entrust;
        dprice_entrust = other.dprice_entrust;
        md_time_driven = other.md_time_driven;
        md_time_entrust = other.md_time_entrust;
        orig_internal_order_id = other.orig_internal_order_id;
        md_time_cancel_sent = other.md_time_cancel_sent;
        md_time_cancel_done = other.md_time_cancel_done;
        internal_security_id = other.internal_security_id;
        security_id = other.security_id;
        broker_order_id.as_uint = other.broker_order_id.as_uint;
        volume_traded = other.volume_traded;
        volume_remain = other.volume_remain;
        dvalue_traded = other.dvalue_traded;
        dprice_traded = other.dprice_traded;
        dfee_estimate = other.dfee_estimate;
        dfee_executed = other.dfee_executed;
        md_time_broker_response = other.md_time_broker_response;
        md_time_market_response = other.md_time_market_response;
        md_time_traded_first = other.md_time_traded_first;
        md_time_traded_latest = other.md_time_traded_latest;
        order_status.store(other.order_status.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    order_request& operator=(const order_request& other) {
        if (this != &other) {
            internal_order_id = other.internal_order_id;
            padding0_0 = other.padding0_0;
            order_type = other.order_type;
            trade_side = other.trade_side;
            market = other.market;
            volume_entrust = other.volume_entrust;
            dprice_entrust = other.dprice_entrust;
            md_time_driven = other.md_time_driven;
            md_time_entrust = other.md_time_entrust;
            orig_internal_order_id = other.orig_internal_order_id;
            md_time_cancel_sent = other.md_time_cancel_sent;
            md_time_cancel_done = other.md_time_cancel_done;
            internal_security_id = other.internal_security_id;
            security_id = other.security_id;
            broker_order_id.as_uint = other.broker_order_id.as_uint;
            volume_traded = other.volume_traded;
            volume_remain = other.volume_remain;
            dvalue_traded = other.dvalue_traded;
            dprice_traded = other.dprice_traded;
            dfee_estimate = other.dfee_estimate;
            dfee_executed = other.dfee_executed;
            md_time_broker_response = other.md_time_broker_response;
            md_time_market_response = other.md_time_market_response;
            md_time_traded_first = other.md_time_traded_first;
            md_time_traded_latest = other.md_time_traded_latest;
            order_status.store(other.order_status.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    void init_new(std::string_view sec_id, internal_security_id_t internal_sec_id, internal_order_id_t internal_id,
                  trade_side_t side, market_t mkt, volume_t vol, dprice_t dpx, md_time_t md_time_driven_) {
        internal_order_id = internal_id;
        order_type = order_type_t::New;
        trade_side = side;
        market = mkt;
        volume_entrust = vol;
        dprice_entrust = dpx;
        md_time_driven = md_time_driven_;
        md_time_entrust = 0;  // filled by trader_api
        security_id.assign(sec_id);
        internal_security_id = internal_sec_id;
        md_time_cancel_sent = 0;
        md_time_cancel_done = 0;
        volume_traded = 0;
        volume_remain = vol;
        dvalue_traded = 0;
        dprice_traded = 0;
        dfee_estimate = 0;
        dfee_executed = 0;
    }

    void init_cancel(internal_order_id_t internal_id, md_time_t md_time_driven_, internal_order_id_t orig_internal_id) {
        internal_order_id = internal_id;
        order_type = order_type_t::Cancel;
        trade_side = trade_side_t::NotSet;
        market = market_t::NotSet;
        volume_entrust = 0;
        dprice_entrust = 0;
        md_time_driven = md_time_driven_;
        md_time_entrust = 0;  // filled by trader_api
        security_id.assign({});
        internal_security_id.clear();
        orig_internal_order_id = orig_internal_id;
        md_time_cancel_sent = 0;
        md_time_cancel_done = 0;

        broker_order_id.as_uint = 0;
        volume_traded = 0;
        volume_remain = 0;
        dvalue_traded = 0;
        dprice_traded = 0;
        dfee_estimate = 0;
        dfee_executed = 0;
        md_time_broker_response = 0;
        md_time_market_response = 0;
        md_time_traded_first = 0;
        md_time_traded_latest = 0;
    }
};

static_assert(sizeof(order_request) == 192, "order_request must be 192 bytes (3 cache lines)");
static_assert(alignof(order_request) == 64, "order_request must be 64-byte aligned");
static_assert(offsetof(order_request, broker_order_id) == 64, "broker_order_id must start at cache line 1");
static_assert(offsetof(order_request, dfee_estimate) == 128, "dfee_estimate must start at cache line 2");
};  // namespace acct_service
