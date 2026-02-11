#include <atomic>
#include <csignal>
#include <cstdio>
#include <string>

#include "common/error.hpp"
#include "gateway_config.hpp"
#include "gateway_loop.hpp"
#include "shm/shm_manager.hpp"
#include "sim_broker_adapter.hpp"

namespace {

std::atomic<acct_service::gateway::gateway_loop*> g_gateway_loop{nullptr};

void handle_signal(int) {
    acct_service::gateway::gateway_loop* loop = g_gateway_loop.load(std::memory_order_acquire);
    if (loop) {
        loop->stop();
    }
}

void install_signal_handler() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace acct_service;

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

    if (config.broker_type != "sim") {
        std::fprintf(stderr, "unsupported --broker-type: %s\n", config.broker_type.c_str());
        return 2;
    }

    SHMManager downstream_manager;
    SHMManager trades_manager;
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

    gateway::sim_broker_adapter adapter;
    broker_api::broker_runtime_config runtime_config;
    runtime_config.account_id = config.account_id;
    runtime_config.auto_fill = true;

    if (!adapter.initialize(runtime_config)) {
        std::fprintf(stderr, "failed to initialize broker adapter\n");
        return 1;
    }

    gateway::gateway_loop loop(config, downstream, trades, adapter);
    g_gateway_loop.store(&loop, std::memory_order_release);
    install_signal_handler();

    const int run_rc = loop.run();

    g_gateway_loop.store(nullptr, std::memory_order_release);
    adapter.shutdown();
    downstream_manager.close();
    trades_manager.close();

    return run_rc;
}

