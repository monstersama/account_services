#pragma once

#include <string>

#include "broker_api/broker_api.hpp"

namespace acct_service::gateway {

// 已加载插件实例（包含动态库句柄和适配器对象）。
class loaded_adapter {
public:
    loaded_adapter() = default;
    ~loaded_adapter();

    loaded_adapter(const loaded_adapter&) = delete;
    loaded_adapter& operator=(const loaded_adapter&) = delete;

    loaded_adapter(loaded_adapter&& other) noexcept;
    loaded_adapter& operator=(loaded_adapter&& other) noexcept;

    broker_api::IBrokerAdapter* get() const noexcept;
    bool valid() const noexcept;

    void reset() noexcept;

private:
    friend bool load_adapter_plugin(const std::string& so_path, loaded_adapter& out, std::string& error_message);

    void* handle_ = nullptr;
    broker_api::IBrokerAdapter* adapter_ = nullptr;
    broker_api::plugin_destroy_fn_t destroy_fn_ = nullptr;
};

// 从指定 .so 加载券商适配器插件。
bool load_adapter_plugin(const std::string& so_path, loaded_adapter& out, std::string& error_message);

}  // namespace acct_service::gateway

