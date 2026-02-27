#include <cassert>
#include <cstring>
#include <cstdio>
#include <string>

#include "order_mapper.hpp"
#include "response_mapper.hpp"
#include "order/order_request.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;

// 验证新单字段映射是否完整正确。
TEST(map_new_order_request) {
    order_request request;
    request.init_new("600000", internal_security_id_t("SH.600000"), static_cast<internal_order_id_t>(1001),
        trade_side_t::Buy, market_t::SH, static_cast<volume_t>(300), static_cast<dprice_t>(1234), 93000000);
    request.md_time_entrust = 93010000;

    broker_api::broker_order_request mapped;
    assert(gateway::map_order_request_to_broker(request, mapped));

    assert(mapped.internal_order_id == static_cast<uint32_t>(1001));
    assert(std::string(mapped.internal_security_id) == "SH.600000");
    assert(mapped.type == broker_api::request_type::New);
    assert(mapped.trade_side == broker_api::side::Buy);
    assert(mapped.order_market == broker_api::market::SH);
    assert(mapped.volume == static_cast<uint64_t>(300));
    assert(mapped.price == static_cast<uint64_t>(1234));
    assert(mapped.md_time == static_cast<uint32_t>(93010000));
    assert(std::string(mapped.security_id) == "600000");
}

// 验证撤单映射保留原订单关联关系。
TEST(map_cancel_order_request) {
    order_request request;
    request.init_cancel(static_cast<internal_order_id_t>(2001), 93100000, static_cast<internal_order_id_t>(1001));

    broker_api::broker_order_request mapped;
    assert(gateway::map_order_request_to_broker(request, mapped));

    assert(mapped.internal_order_id == static_cast<uint32_t>(2001));
    assert(mapped.orig_internal_order_id == static_cast<uint32_t>(1001));
    assert(mapped.type == broker_api::request_type::Cancel);
}

// 验证成交事件映射到 trade_response 的状态与数值字段。
TEST(map_trade_event_response) {
    broker_api::broker_event event;
    event.kind = broker_api::event_kind::Trade;
    event.internal_order_id = 3001;
    event.broker_order_id = 7001;
    std::strcpy(event.internal_security_id, "SZ.000009");
    event.trade_side = broker_api::side::Sell;
    event.volume_traded = 88;
    event.price_traded = 3210;
    event.value_traded = 282480;
    event.fee = 30;
    event.md_time_traded = 100001000;

    trade_response response;
    assert(gateway::map_broker_event_to_trade_response(event, response));

    assert(response.internal_order_id == 3001);
    assert(response.broker_order_id == 7001);
    assert(response.internal_security_id == std::string_view("SZ.000009"));
    assert(response.trade_side == trade_side_t::Sell);
    assert(response.new_status == order_status_t::MarketAccepted);
    assert(response.volume_traded == 88);
    assert(response.dprice_traded == 3210);
    assert(response.dvalue_traded == 282480);
    assert(response.dfee == 30);
    assert(response.md_time_traded == 100001000);
}

}  // namespace

int main() {
    printf("=== Gateway Mapper Test Suite ===\n\n");

    RUN_TEST(map_new_order_request);
    RUN_TEST(map_cancel_order_request);
    RUN_TEST(map_trade_event_response);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
