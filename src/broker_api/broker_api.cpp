#include "broker_api/broker_api.hpp"

#include "version.h"

namespace acct_service::broker_api {

const char* broker_api_version() noexcept { return ACCT_API_VERSION; }

uint32_t broker_api_abi_version() noexcept { return kBrokerApiAbiVersion; }

}  // namespace acct_service::broker_api

