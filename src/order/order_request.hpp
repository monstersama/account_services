#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "common/types.hpp"

namespace acct_service {

enum class OrderType : uint8_t {
    NotSet = 0,
    New = 1,
    Cancel = 2,
    Unknown = 0xFF,
};

enum class TradeSide : uint8_t {
    NotSet = 0,
    Buy = 1,
    Sell = 2,
};

enum class Market : uint8_t {
    NotSet = 0,
    SZ = 1,
    SH = 2,
    BJ = 3,
    HK = 4,
    Unknown = 0xFF,
};

enum class OrderState : uint8_t {
    NotSet = 0,
    UserSubmitted = 0x12,
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

enum class ExecutionState : uint8_t {
    None = 0,
    Running = 1,
    Cancelling = 2,
    Finished = 3,
    Failed = 4,
};

struct alignas(64) OrderRequest {
    // cache line 0
    InternalOrderId internal_order_id{0};                                        // 系统内部订单ID，唯一标识
    PassiveExecutionAlgo passive_execution_algo{PassiveExecutionAlgo::Default};  // 单笔订单声明的被动算法
    OrderType order_type{OrderType::NotSet};                                     // 订单类型：报单、撤单
    TradeSide trade_side{TradeSide::NotSet};                                     // 买卖方向：买入、卖出
    Market market{Market::NotSet};                                               // 市场：深、沪、北、港
    Volume volume_entrust{0};                                                    // 委托数量
    DPrice dprice_entrust{0};                                                    // 委托价格
    InternalOrderId orig_internal_order_id{0};  // 原始订单ID（仅撤单请求使用）
    InternalSecurityId internal_security_id{};  // 内部证券ID（格式: "XSHE_000001"）
    SecurityId security_id{};                   // 证券代码字符串（如 "000001"）
    uint8_t active_strategy_claimed{0};         // 1=当前订单由主动覆盖层管理/生成
    uint8_t reserved_exec_flags{0};
    uint8_t padding0_1[2]{};

    // cache line 1
    union {
        BrokerOrderId as_str{};  // 柜台返回的订单ID（字符串）
        uint64_t as_uint;        // 柜台返回的订单ID（数字）
    } broker_order_id;
    Volume volume_traded{0};  // 已成交数量
    Volume volume_remain{0};  // 剩余未成交数量
    DValue dvalue_traded{0};  // 已成交金额
    DPrice dprice_traded{0};  // 成交均价

    // cache line 2
    DValue dfee_estimate{0};                                  // 预估手续费
    DValue dfee_executed{0};                                  // 已实际产生的手续费
    MdTime md_time_driven{0};                                 // 触发时间
    MdTime md_time_entrust{0};                                // 委托结束时间
    MdTime md_time_cancel_sent{0};                            // 撤单发送时间（本地时间）
    MdTime md_time_cancel_done{0};                            // 撤单完成时间（来自柜台/市场）
    MdTime md_time_broker_response{0};                        // 柜台响应时间
    MdTime md_time_market_response{0};                        // 交易所响应时间
    MdTime md_time_traded_first{0};                           // 首次成交时间
    MdTime md_time_traded_latest{0};                          // 最近成交时间
    std::atomic<OrderState> order_state{OrderState::NotSet};  // 订单当前状态
    uint8_t padding2[15]{};                                   // 填充以对齐缓存行

    // cache line 3
    PassiveExecutionAlgo execution_algo{PassiveExecutionAlgo::None};  // 受管父单执行算法镜像
    ExecutionState execution_state{ExecutionState::None};             // 受管父单执行态镜像
    uint8_t padding3_0[6]{};
    Volume target_volume{0};       // 受管父单目标量
    Volume working_volume{0};      // 受管父单当前在途量
    Volume schedulable_volume{0};  // 受管父单当前可继续释放的预算
    uint8_t padding3_1[32]{};

    OrderRequest() = default;
    OrderRequest(const OrderRequest& other) {
        internal_order_id = other.internal_order_id;
        passive_execution_algo = other.passive_execution_algo;
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
        active_strategy_claimed = other.active_strategy_claimed;
        reserved_exec_flags = other.reserved_exec_flags;
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
        execution_algo = other.execution_algo;
        execution_state = other.execution_state;
        target_volume = other.target_volume;
        working_volume = other.working_volume;
        schedulable_volume = other.schedulable_volume;
        order_state.store(other.order_state.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    OrderRequest& operator=(const OrderRequest& other) {
        if (this != &other) {
            internal_order_id = other.internal_order_id;
            passive_execution_algo = other.passive_execution_algo;
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
            active_strategy_claimed = other.active_strategy_claimed;
            reserved_exec_flags = other.reserved_exec_flags;
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
            execution_algo = other.execution_algo;
            execution_state = other.execution_state;
            target_volume = other.target_volume;
            working_volume = other.working_volume;
            schedulable_volume = other.schedulable_volume;
            order_state.store(other.order_state.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    void init_new(std::string_view sec_id, InternalSecurityId internal_sec_id, InternalOrderId internal_id,
                  TradeSide side, Market mkt, Volume vol, DPrice dpx, MdTime md_time_driven_) {
        internal_order_id = internal_id;
        order_type = OrderType::New;
        trade_side = side;
        market = mkt;
        volume_entrust = vol;
        dprice_entrust = dpx;
        md_time_driven = md_time_driven_;
        md_time_entrust = 0;  // filled by trader_api
        passive_execution_algo = PassiveExecutionAlgo::Default;
        security_id.assign(sec_id);
        internal_security_id = internal_sec_id;
        active_strategy_claimed = 0;
        reserved_exec_flags = 0;
        md_time_cancel_sent = 0;
        md_time_cancel_done = 0;
        volume_traded = 0;
        volume_remain = vol;
        dvalue_traded = 0;
        dprice_traded = 0;
        dfee_estimate = 0;
        dfee_executed = 0;
        execution_algo = PassiveExecutionAlgo::None;
        execution_state = ExecutionState::None;
        target_volume = 0;
        working_volume = 0;
        schedulable_volume = 0;
    }

    void init_cancel(InternalOrderId internal_id, MdTime md_time_driven_, InternalOrderId orig_internal_id) {
        internal_order_id = internal_id;
        order_type = OrderType::Cancel;
        trade_side = TradeSide::NotSet;
        market = Market::NotSet;
        volume_entrust = 0;
        dprice_entrust = 0;
        md_time_driven = md_time_driven_;
        md_time_entrust = 0;  // filled by trader_api
        passive_execution_algo = PassiveExecutionAlgo::None;
        security_id.assign({});
        internal_security_id.clear();
        active_strategy_claimed = 0;
        reserved_exec_flags = 0;
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
        execution_algo = PassiveExecutionAlgo::None;
        execution_state = ExecutionState::None;
        target_volume = 0;
        working_volume = 0;
        schedulable_volume = 0;
    }
};

static_assert(sizeof(OrderRequest) == 256, "order_request must be 256 bytes (4 cache lines)");
static_assert(alignof(OrderRequest) == 64, "order_request must be 64-byte aligned");
static_assert(offsetof(OrderRequest, broker_order_id) == 64, "broker_order_id must start at cache line 1");
static_assert(offsetof(OrderRequest, dfee_estimate) == 128, "dfee_estimate must start at cache line 2");
static_assert(offsetof(OrderRequest, execution_algo) == 192, "execution_algo must start at cache line 3");
};  // namespace acct_service
