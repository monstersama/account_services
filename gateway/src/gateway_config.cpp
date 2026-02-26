#include "gateway_config.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace acct_service::gateway {

namespace {

constexpr const char* kDefaultGatewayConfigPath = "config/gateway.yaml";

std::string trim_copy(std::string text) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };

    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

// 解析 uint32 参数。
bool parse_u32(const std::string& text, uint32_t& out) {
    try {
        const unsigned long long value = std::stoull(trim_copy(text));
        if (value > UINT_MAX) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

// 解析布尔参数（支持常见文本形式）。
bool parse_bool(std::string text, bool& out) {
    text = trim_copy(std::move(text));
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

// 校验交易日格式：必须是 YYYYMMDD 八位数字。
bool is_valid_trading_day(std::string_view trading_day) {
    if (trading_day.size() != 8) {
        return false;
    }
    return std::all_of(trading_day.begin(), trading_day.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

// 读取 option 对应值（例如 --foo <value>）。
bool require_value(int argc, int i, char* argv[], std::string& value, std::string& error_message) {
    if (i + 1 >= argc || argv[i + 1] == nullptr) {
        error_message = std::string("missing value for ") + (argv[i] ? argv[i] : "option");
        return false;
    }
    value = argv[i + 1];
    return true;
}

template <std::size_t N>
bool contains_key(std::string_view key, const std::string_view (&allowed_keys)[N]) {
    for (std::string_view allowed : allowed_keys) {
        if (key == allowed) {
            return true;
        }
    }
    return false;
}

bool apply_config_value(gateway_config& config, std::string_view key, const std::string& raw_value, std::string& error_message) {
    const std::string value = trim_copy(raw_value);

    if (key == "account_id") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed) || parsed == 0) {
            error_message = "invalid value for account_id";
            return false;
        }
        config.account_id = static_cast<account_id_t>(parsed);
        return true;
    }

    if (key == "downstream_shm" || key == "downstream_shm_name") {
        config.downstream_shm_name = value;
        return true;
    }
    if (key == "trades_shm" || key == "trades_shm_name") {
        config.trades_shm_name = value;
        return true;
    }
    if (key == "orders_shm" || key == "orders_shm_name") {
        config.orders_shm_name = value;
        return true;
    }

    if (key == "trading_day") {
        if (!is_valid_trading_day(value)) {
            error_message = "invalid value for trading_day";
            return false;
        }
        config.trading_day = value;
        return true;
    }

    if (key == "broker_type") {
        config.broker_type = value;
        return true;
    }
    if (key == "adapter_so" || key == "adapter_plugin_so") {
        config.adapter_plugin_so = value;
        return true;
    }

    if (key == "create_if_not_exist") {
        bool parsed = false;
        if (!parse_bool(value, parsed)) {
            error_message = "invalid value for create_if_not_exist";
            return false;
        }
        config.create_if_not_exist = parsed;
        return true;
    }

    if (key == "poll_batch_size") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed) || parsed == 0) {
            error_message = "invalid value for poll_batch_size";
            return false;
        }
        config.poll_batch_size = parsed;
        return true;
    }

    if (key == "idle_sleep_us") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            error_message = "invalid value for idle_sleep_us";
            return false;
        }
        config.idle_sleep_us = parsed;
        return true;
    }

    if (key == "stats_interval_ms") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            error_message = "invalid value for stats_interval_ms";
            return false;
        }
        config.stats_interval_ms = parsed;
        return true;
    }

    if (key == "max_retries" || key == "max_retry_attempts") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            error_message = "invalid value for max_retries";
            return false;
        }
        config.max_retry_attempts = parsed;
        return true;
    }

    if (key == "retry_interval_us") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            error_message = "invalid value for retry_interval_us";
            return false;
        }
        config.retry_interval_us = parsed;
        return true;
    }

    error_message = std::string("unknown config key: ") + std::string(key);
    return false;
}

bool load_config_yaml(const std::string& config_path, gateway_config& config, std::string& error_message) {
    if (config_path.empty()) {
        error_message = "empty --config path";
        return false;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& ex) {
        error_message = std::string("failed to load config file: ") + ex.what();
        return false;
    }

    if (!root || root.IsNull()) {
        config.config_file = config_path;
        return true;
    }

    if (!root.IsMap()) {
        error_message = "gateway config root must be a YAML map";
        return false;
    }

    static constexpr std::string_view kAllowedKeys[] = {"account_id", "downstream_shm", "downstream_shm_name",
        "trades_shm", "trades_shm_name", "orders_shm", "orders_shm_name", "trading_day", "broker_type",
        "adapter_so", "adapter_plugin_so", "create_if_not_exist", "poll_batch_size", "idle_sleep_us",
        "stats_interval_ms", "max_retries", "max_retry_attempts", "retry_interval_us"};

    for (const auto& entry : root) {
        const YAML::Node key_node = entry.first;
        const YAML::Node value_node = entry.second;

        if (!key_node.IsScalar()) {
            error_message = "gateway config key must be scalar";
            return false;
        }
        if (!value_node.IsScalar()) {
            std::string key_text;
            try {
                key_text = key_node.as<std::string>();
            } catch (...) {
                key_text = "<unknown>";
            }
            error_message = "gateway config value must be scalar: " + key_text;
            return false;
        }

        std::string key;
        std::string value;
        try {
            key = key_node.as<std::string>();
            value = value_node.as<std::string>();
        } catch (const YAML::Exception& ex) {
            error_message = std::string("failed to parse gateway config field: ") + ex.what();
            return false;
        }

        if (!contains_key(key, kAllowedKeys)) {
            error_message = "unknown gateway config key: " + key;
            return false;
        }

        if (!apply_config_value(config, key, value, error_message)) {
            return false;
        }
    }

    config.config_file = config_path;
    return true;
}

bool validate_config(const gateway_config& config, std::string& error_message) {
    // 共享内存名称是必须项。
    if (config.downstream_shm_name.empty() || config.trades_shm_name.empty() || config.orders_shm_name.empty()) {
        error_message = "shared memory names must be non-empty";
        return false;
    }
    if (!is_valid_trading_day(config.trading_day)) {
        error_message = "trading_day must be YYYYMMDD";
        return false;
    }

    if (config.broker_type != "sim" && config.broker_type != "plugin") {
        error_message = "--broker-type must be sim or plugin";
        return false;
    }

    if (config.broker_type == "plugin" && config.adapter_plugin_so.empty()) {
        error_message = "--adapter-so is required when --broker-type=plugin";
        return false;
    }

    return true;
}

}  // namespace

// 打印网关命令行参数说明。
void print_usage(const char* program) {
    std::fprintf(stderr,
        "Usage: %s [--config <path>] [config_path]\n"
        "  --config <path>   specify gateway config path (default: config/gateway.yaml)\n"
        "  -h, --help                   show this help\n",
        program ? program : "acct_broker_gateway");
}

// 解析命令行参数并填充 gateway_config，遇到非法输入时返回 Error。
parse_result_t parse_args(int argc, char* argv[], gateway_config& config, std::string& error_message) {
    error_message.clear();
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }

        const std::string option(arg);
        if (option == "-h" || option == "--help") {
            print_usage(argv[0]);
            return parse_result_t::Help;
        }

        if (option == "--config") {
            std::string value;
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            ++i;
            if (!config_path.empty()) {
                error_message = "duplicated config path";
                return parse_result_t::Error;
            }
            config_path = value;
            continue;
        }

        if (option.rfind("-", 0) == 0) {
            error_message = std::string("unknown option: ") + option;
            return parse_result_t::Error;
        }

        if (!config_path.empty()) {
            error_message = std::string("duplicated config path: ") + option;
            return parse_result_t::Error;
        }
        config_path = option;
    }

    if (config_path.empty()) {
        config_path = kDefaultGatewayConfigPath;
    }

    if (!load_config_yaml(config_path, config, error_message)) {
        return parse_result_t::Error;
    }

    if (!validate_config(config, error_message)) {
        return parse_result_t::Error;
    }

    return parse_result_t::Ok;
}

}  // namespace acct_service::gateway
