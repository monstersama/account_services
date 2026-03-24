#include "gateway_config.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <expected>
#include <cstdio>
#include <string>
#include <string_view>

#include <yaml-cpp/yaml.h>

namespace acct_service::gateway {

namespace {

constexpr const char* kDefaultGatewayConfigPath = "config/gateway.yaml";

enum class GatewayConfigParseError {
    InvalidBool,
    InvalidU32,
    InvalidTradingDay,
    NonPositiveValue,
    OutOfRange,
    UnknownKey,
};

const char* gateway_config_parse_error_message(GatewayConfigParseError error) noexcept {
    switch (error) {
        case GatewayConfigParseError::InvalidBool:
            return "invalid boolean value";
        case GatewayConfigParseError::InvalidU32:
            return "invalid uint32 value";
        case GatewayConfigParseError::InvalidTradingDay:
            return "invalid trading day";
        case GatewayConfigParseError::NonPositiveValue:
            return "value must be positive";
        case GatewayConfigParseError::OutOfRange:
            return "numeric value out of range";
        case GatewayConfigParseError::UnknownKey:
            return "unknown key";
    }
    return "invalid value";
}

std::string trim_copy(std::string text) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };

    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string_view trim_view(std::string_view value) {
    const auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

template <typename T>
std::expected<T, GatewayConfigParseError> parse_integral(std::string_view raw_value, GatewayConfigParseError invalid_error) {
    std::string_view value = trim_view(raw_value);
    if (value.empty()) {
        return std::unexpected(invalid_error);
    }

    if (!value.empty() && value.front() == '+') {
        value.remove_prefix(1);
    }
    if (value.empty()) {
        return std::unexpected(invalid_error);
    }

    T parsed{};
    const char* const begin = value.data();
    const char* const end = begin + value.size();
    const auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec == std::errc{} && ptr == end) {
        return parsed;
    }
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(GatewayConfigParseError::OutOfRange);
    }
    return std::unexpected(invalid_error);
}

// 解析 uint32 参数。
std::expected<uint32_t, GatewayConfigParseError> parse_u32(std::string_view text) {
    return parse_integral<uint32_t>(text, GatewayConfigParseError::InvalidU32);
}

// 解析布尔参数（支持常见文本形式）。
std::expected<bool, GatewayConfigParseError> parse_bool(std::string_view raw_text) {
    std::string text = trim_copy(std::string(raw_text));
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    return std::unexpected(GatewayConfigParseError::InvalidBool);
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

template <typename T, typename U>
std::expected<void, GatewayConfigParseError> assign_parsed(std::expected<T, GatewayConfigParseError> parsed, U& out) {
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    out = static_cast<U>(*parsed);
    return {};
}

std::string make_invalid_value_message(std::string_view key, GatewayConfigParseError error) {
    std::string message = "invalid value for ";
    message += key;
    message += ": ";
    message += gateway_config_parse_error_message(error);
    return message;
}

std::expected<void, GatewayConfigParseError> apply_config_value(
    gateway_config& config, std::string_view key, std::string_view raw_value) {
    const std::string value = trim_copy(std::string(raw_value));

    if (key == "account_id") {
        const auto parsed = parse_u32(value);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed == 0) {
            return std::unexpected(GatewayConfigParseError::NonPositiveValue);
        }
        config.account_id = static_cast<AccountId>(*parsed);
        return {};
    }

    if (key == "downstream_shm" || key == "downstream_shm_name") {
        config.downstream_shm_name = value;
        return {};
    }
    if (key == "trades_shm" || key == "trades_shm_name") {
        config.trades_shm_name = value;
        return {};
    }
    if (key == "orders_shm" || key == "orders_shm_name") {
        config.orders_shm_name = value;
        return {};
    }

    if (key == "trading_day") {
        if (!is_valid_trading_day(value)) {
            return std::unexpected(GatewayConfigParseError::InvalidTradingDay);
        }
        config.trading_day = value;
        return {};
    }

    if (key == "broker_type") {
        config.broker_type = value;
        return {};
    }
    if (key == "adapter_so" || key == "adapter_plugin_so") {
        config.adapter_plugin_so = value;
        return {};
    }

    if (key == "create_if_not_exist") {
        return assign_parsed(parse_bool(value), config.create_if_not_exist);
    }

    if (key == "poll_batch_size") {
        const auto parsed = parse_u32(value);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        if (*parsed == 0) {
            return std::unexpected(GatewayConfigParseError::NonPositiveValue);
        }
        config.poll_batch_size = *parsed;
        return {};
    }

    if (key == "idle_sleep_us") {
        return assign_parsed(parse_u32(value), config.idle_sleep_us);
    }

    if (key == "stats_interval_ms") {
        return assign_parsed(parse_u32(value), config.stats_interval_ms);
    }

    if (key == "max_retries" || key == "max_retry_attempts") {
        return assign_parsed(parse_u32(value), config.max_retry_attempts);
    }

    if (key == "retry_interval_us") {
        return assign_parsed(parse_u32(value), config.retry_interval_us);
    }

    return std::unexpected(GatewayConfigParseError::UnknownKey);
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

        const auto applied = apply_config_value(config, key, value);
        if (!applied) {
            error_message = make_invalid_value_message(key, applied.error());
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
                error_message = "duplicated Config path";
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
            error_message = std::string("duplicated Config path: ") + option;
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
