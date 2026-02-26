#include "adapter_loader.hpp"

#include <dlfcn.h>

#include <sstream>

namespace acct_service::gateway {

namespace {

std::string dlerror_message(const char* prefix) {
    std::ostringstream os;
    os << prefix;
    const char* err = dlerror();
    if (err && *err != '\0') {
        os << ": " << err;
    }
    return os.str();
}

template <typename Fn>
Fn lookup_symbol(void* handle, const char* symbol_name, std::string& error_message) {
    dlerror();
    void* symbol = dlsym(handle, symbol_name);
    if (!symbol) {
        error_message = dlerror_message("dlsym failed");
        return nullptr;
    }
    return reinterpret_cast<Fn>(symbol);
}

}  // namespace

loaded_adapter::~loaded_adapter() noexcept { reset(); }

loaded_adapter::loaded_adapter(loaded_adapter&& other) noexcept
    : handle_(other.handle_), adapter_(other.adapter_), destroy_fn_(other.destroy_fn_) {
    other.handle_ = nullptr;
    other.adapter_ = nullptr;
    other.destroy_fn_ = nullptr;
}

loaded_adapter& loaded_adapter::operator=(loaded_adapter&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        adapter_ = other.adapter_;
        destroy_fn_ = other.destroy_fn_;

        other.handle_ = nullptr;
        other.adapter_ = nullptr;
        other.destroy_fn_ = nullptr;
    }
    return *this;
}

broker_api::IBrokerAdapter* loaded_adapter::get() const noexcept { return adapter_; }

bool loaded_adapter::valid() const noexcept { return handle_ != nullptr && adapter_ != nullptr && destroy_fn_ != nullptr; }

void loaded_adapter::reset() noexcept {
    if (adapter_ && destroy_fn_) {
        destroy_fn_(adapter_);
    }
    adapter_ = nullptr;
    destroy_fn_ = nullptr;

    if (handle_) {
        dlclose(handle_);
    }
    handle_ = nullptr;
}

bool load_adapter_plugin(const std::string& so_path, loaded_adapter& out, std::string& error_message) {
    out.reset();
    error_message.clear();

    if (so_path.empty()) {
        error_message = "empty adapter plugin path";
        return false;
    }

    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        error_message = dlerror_message("dlopen failed");
        return false;
    }

    broker_api::plugin_abi_fn_t abi_fn =
        lookup_symbol<broker_api::plugin_abi_fn_t>(handle, broker_api::kPluginAbiSymbol, error_message);
    if (!abi_fn) {
        dlclose(handle);
        return false;
    }

    broker_api::plugin_create_fn_t create_fn =
        lookup_symbol<broker_api::plugin_create_fn_t>(handle, broker_api::kPluginCreateSymbol, error_message);
    if (!create_fn) {
        dlclose(handle);
        return false;
    }

    broker_api::plugin_destroy_fn_t destroy_fn =
        lookup_symbol<broker_api::plugin_destroy_fn_t>(handle, broker_api::kPluginDestroySymbol, error_message);
    if (!destroy_fn) {
        dlclose(handle);
        return false;
    }

    const uint32_t plugin_abi = abi_fn();
    if (plugin_abi != broker_api::kBrokerApiAbiVersion) {
        std::ostringstream os;
        os << "plugin abi mismatch: expected=" << broker_api::kBrokerApiAbiVersion << " got=" << plugin_abi;
        error_message = os.str();
        dlclose(handle);
        return false;
    }

    broker_api::IBrokerAdapter* adapter = create_fn();
    if (!adapter) {
        error_message = "plugin create returned null adapter";
        dlclose(handle);
        return false;
    }

    out.handle_ = handle;
    out.adapter_ = adapter;
    out.destroy_fn_ = destroy_fn;
    return true;
}

}  // namespace acct_service::gateway
