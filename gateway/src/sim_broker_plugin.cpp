#include "sim_broker_adapter.hpp"

namespace {

using namespace acct_service;

}  // namespace

extern "C" ACCT_BROKER_API uint32_t acct_broker_plugin_abi_version() noexcept {
    return broker_api::kBrokerApiAbiVersion;
}

extern "C" ACCT_BROKER_API broker_api::IBrokerAdapter* acct_create_broker_adapter() noexcept {
    return new gateway::sim_broker_adapter();
}

extern "C" ACCT_BROKER_API void acct_destroy_broker_adapter(broker_api::IBrokerAdapter* adapter) noexcept {
    delete adapter;
}
