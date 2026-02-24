#include "broker_api/broker_api.hpp"

#include "version.h"

namespace acct_service::broker_api {

// 动态库版本字符串，供外部链接方查询。
const char* broker_api_version() noexcept { return ACCT_API_VERSION; }

// ABI 版本号，供调用方做兼容检查。
uint32_t broker_api_abi_version() noexcept { return kBrokerApiAbiVersion; }

}  // namespace acct_service::broker_api
