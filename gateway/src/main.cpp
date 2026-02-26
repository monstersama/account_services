#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>

#include "common/error.hpp"
#include "adapter_loader.hpp"
#include "gateway_config.hpp"
#include "gateway_loop.hpp"
#include "shm/shm_manager.hpp"
#include "shm/orders_shm.hpp"
#include "sim_broker_adapter.hpp"

namespace {

// 供信号处理函数访问的全局 loop 指针。
std::atomic<acct_service::gateway::gateway_loop*> g_gateway_loop{nullptr};

// 收到退出信号后请求主循环停止。
void handle_signal(int) {
    acct_service::gateway::gateway_loop* loop = g_gateway_loop.load(std::memory_order_acquire);
    if (loop) {
        loop->stop();
    }
}

// 注册常见退出信号。
void install_signal_handler() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

}  // namespace

// 程序入口：解析参数、初始化资源、运行网关主循环并完成退出清理。
int main(int argc, char* argv[]) {
    using namespace acct_service;

    // 先解析命令行参数，处理 help / 参数错误等快速退出分支。
    gateway::gateway_config config;
    std::string error_message;
    const gateway::parse_result_t parse_result = gateway::parse_args(argc, argv, config, error_message);
    if (parse_result == gateway::parse_result_t::Help) {
        return 0;
    }
    if (parse_result == gateway::parse_result_t::Error) {
        if (!error_message.empty()) {
            std::fprintf(stderr, "%s\n", error_message.c_str());
        }
        gateway::print_usage(argv[0]);
        return 2;
    }

    // 打开共享内存：读取下游订单，写入成交回报。
    SHMManager downstream_manager;
    SHMManager trades_manager;
    SHMManager orders_manager;
    const shm_mode mode = config.create_if_not_exist ? shm_mode::OpenOrCreate : shm_mode::Open;

    downstream_shm_layout* downstream =
        downstream_manager.open_downstream(config.downstream_shm_name, mode, config.account_id);
    if (!downstream) {
        const error_status& status = latest_error();
        std::fprintf(stderr,
            "failed to open downstream shm: domain=%s code=%s msg=%s\n",
            to_string(status.domain), to_string(status.code), status.message.c_str());
        return 1;
    }

    trades_shm_layout* trades = trades_manager.open_trades(config.trades_shm_name, mode, config.account_id);
    if (!trades) {
        const error_status& status = latest_error();
        std::fprintf(stderr,
            "failed to open trades shm: domain=%s code=%s msg=%s\n",
            to_string(status.domain), to_string(status.code), status.message.c_str());
        return 1;
    }

    const std::string dated_orders_name = make_orders_shm_name(config.orders_shm_name, config.trading_day);
    orders_shm_layout* orders = orders_manager.open_orders(dated_orders_name, mode, config.account_id);
    if (!orders) {
        const error_status& status = latest_error();
        std::fprintf(stderr,
            "failed to open orders shm: domain=%s code=%s msg=%s\n",
            to_string(status.domain), to_string(status.code), status.message.c_str());
        return 1;
    }

    broker_api::IBrokerAdapter* adapter_ptr = nullptr;
    gateway::sim_broker_adapter sim_adapter;
    gateway::loaded_adapter plugin_adapter;

    // 支持两种适配器模式：内置 sim 或外部插件。
    if (config.broker_type == "sim") {
        adapter_ptr = &sim_adapter;
    } else if (config.broker_type == "plugin") {
        std::string error_message;
        if (!gateway::load_adapter_plugin(config.adapter_plugin_so, plugin_adapter, error_message)) {
            std::fprintf(stderr, "failed to load adapter plugin: %s\n", error_message.c_str());
            return 1;
        }
        adapter_ptr = plugin_adapter.get();
    } else {
        std::fprintf(stderr, "unsupported --broker-type: %s\n", config.broker_type.c_str());
        return 2;
    }

    broker_api::broker_runtime_config runtime_config;
    // 组装适配器运行时参数并完成初始化。
    runtime_config.account_id = config.account_id;
    runtime_config.auto_fill = true;

    if (!adapter_ptr || !adapter_ptr->initialize(runtime_config)) {
        std::fprintf(stderr, "failed to initialize broker adapter\n");
        return 1;
    }

    gateway::gateway_loop loop(config, downstream, trades, orders, *adapter_ptr);
    // 将 loop 指针暴露给信号处理逻辑，再进入主循环。
    g_gateway_loop.store(&loop, std::memory_order_release);
    install_signal_handler();

    // 进入主循环，直到信号或内部停止。
    const int run_rc = loop.run();

    // 无论 run 返回何值都按固定顺序回收资源。
    g_gateway_loop.store(nullptr, std::memory_order_release);
    if (adapter_ptr) {
        adapter_ptr->shutdown();
    }
    plugin_adapter.reset();
    downstream_manager.close();
    trades_manager.close();
    orders_manager.close();

    return run_rc;
}
