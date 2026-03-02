#include "full_chain_observer_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace acct_service {
namespace {

constexpr const char* kDefaultObserverConfigPath = "config/observer.yaml";

// 去除字符串首尾空白，统一标量解析行为。
std::string trim_copy(std::string text) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };

    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

// 解析 uint32 配置项，拒绝溢出和非法字符。
bool parse_uint32(const std::string& text, uint32_t* out_value) {
    if (out_value == nullptr) {
        return false;
    }

    try {
        const unsigned long long value = std::stoull(trim_copy(text));
        if (value > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        *out_value = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

// 校验交易日格式：固定 8 位数字。
bool is_valid_trading_day(std::string_view trading_day) {
    if (trading_day.size() != 8) {
        return false;
    }
    return std::all_of(trading_day.begin(), trading_day.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

// 读取 --config 的参数值并生成统一错误信息。
bool require_value(int argc, int i, char* argv[], std::string* out_value, std::string* out_error) {
    if (out_value == nullptr || out_error == nullptr) {
        return false;
    }
    if (i + 1 >= argc || argv[i + 1] == nullptr) {
        *out_error = std::string("missing value for ") + (argv[i] ? argv[i] : "option");
        return false;
    }
    *out_value = argv[i + 1];
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

// 应用单个 YAML 标量字段到配置对象。
bool apply_config_value(
    full_chain_observer_config* out_config, std::string_view key, const std::string& raw_value, std::string* out_error) {
    if (out_config == nullptr || out_error == nullptr) {
        return false;
    }

    const std::string value = trim_copy(raw_value);

    if (key == "orders_shm_name") {
        out_config->orders_shm_name = value;
        return true;
    }
    if (key == "trading_day") {
        out_config->trading_day = value;
        return true;
    }
    if (key == "positions_shm_name") {
        out_config->positions_shm_name = value;
        return true;
    }
    if (key == "output_dir") {
        out_config->output_dir = value;
        return true;
    }
    if (key == "poll_interval_ms") {
        uint32_t parsed_value = 0;
        if (!parse_uint32(value, &parsed_value)) {
            *out_error = "invalid value for poll_interval_ms";
            return false;
        }
        out_config->poll_interval_ms = parsed_value;
        return true;
    }
    if (key == "timeout_ms") {
        uint32_t parsed_value = 0;
        if (!parse_uint32(value, &parsed_value)) {
            *out_error = "invalid value for timeout_ms";
            return false;
        }
        out_config->timeout_ms = parsed_value;
        return true;
    }

    *out_error = std::string("unknown observer config key: ") + std::string(key);
    return false;
}

// 从 YAML 文件加载 observer 配置并执行强校验。
bool load_config_yaml(const std::string& config_path, full_chain_observer_config* out_config, std::string* out_error) {
    if (out_config == nullptr || out_error == nullptr) {
        return false;
    }
    if (config_path.empty()) {
        *out_error = "empty --config path";
        return false;
    }

    YAML::Node root;
    try {
        root = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& ex) {
        *out_error = std::string("failed to load config file: ") + ex.what();
        return false;
    }

    if (!root || root.IsNull() || !root.IsMap()) {
        *out_error = "observer config root must be a YAML map";
        return false;
    }

    static constexpr std::string_view kAllowedKeys[] = {
        "orders_shm_name",
        "trading_day",
        "positions_shm_name",
        "output_dir",
        "poll_interval_ms",
        "timeout_ms",
    };

    // 先在副本上应用，确保失败路径不会污染调用方状态。
    full_chain_observer_config loaded = *out_config;
    for (const auto& entry : root) {
        const YAML::Node key_node = entry.first;
        const YAML::Node value_node = entry.second;
        if (!key_node.IsScalar()) {
            *out_error = "observer config key must be scalar";
            return false;
        }
        if (!value_node.IsScalar()) {
            std::string key_text;
            try {
                key_text = key_node.as<std::string>();
            } catch (...) {
                key_text = "<unknown>";
            }
            *out_error = "observer config value must be scalar: " + key_text;
            return false;
        }

        std::string key;
        std::string value;
        try {
            key = key_node.as<std::string>();
            value = value_node.as<std::string>();
        } catch (const YAML::Exception& ex) {
            *out_error = std::string("failed to parse observer config field: ") + ex.what();
            return false;
        }

        if (!contains_key(key, kAllowedKeys)) {
            *out_error = "unknown observer config key: " + key;
            return false;
        }
        if (!apply_config_value(&loaded, key, value, out_error)) {
            return false;
        }
    }

    loaded.config_file = config_path;
    *out_config = std::move(loaded);
    return true;
}

// 启动前校验关键配置，避免运行期才暴露基础错误。
bool validate_config(const full_chain_observer_config& config, std::string* out_error) {
    if (out_error == nullptr) {
        return false;
    }

    if (config.orders_shm_name.empty() || config.positions_shm_name.empty()) {
        *out_error = "shared memory names must be non-empty";
        return false;
    }
    if (!is_valid_trading_day(config.trading_day)) {
        *out_error = "trading_day must be YYYYMMDD";
        return false;
    }
    if (config.output_dir.empty()) {
        *out_error = "output_dir must be non-empty";
        return false;
    }

    return true;
}

}  // namespace

// 打印 observer 命令行参数说明。
void print_usage(const char* program_name) {
    std::fprintf(stderr,
        "Usage: %s [--config <path>] [config_path]\n"
        "  --config <path>   specify observer config path (default: config/observer.yaml)\n"
        "  -h, --help        show this help\n",
        program_name ? program_name : "full_chain_observer");
}

// 解析参数并加载 observer 配置。
observer_parse_result parse_args(
    int argc, char* argv[], full_chain_observer_config* out_config, std::string* out_error) {
    if (out_config == nullptr || out_error == nullptr) {
        return observer_parse_result::Error;
    }
    out_error->clear();

    std::string config_path;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }

        const std::string option(arg);
        if (option == "-h" || option == "--help") {
            print_usage(argv[0]);
            return observer_parse_result::Help;
        }
        if (option == "--config") {
            std::string value;
            if (!require_value(argc, i, argv, &value, out_error)) {
                return observer_parse_result::Error;
            }
            ++i;
            if (!config_path.empty()) {
                *out_error = "duplicated config path";
                return observer_parse_result::Error;
            }
            config_path = std::move(value);
            continue;
        }
        if (option.rfind("-", 0) == 0) {
            *out_error = "unknown option: " + option;
            return observer_parse_result::Error;
        }
        if (!config_path.empty()) {
            *out_error = "duplicated config path: " + option;
            return observer_parse_result::Error;
        }
        config_path = option;
    }

    if (config_path.empty()) {
        config_path = kDefaultObserverConfigPath;
    }
    if (!load_config_yaml(config_path, out_config, out_error)) {
        return observer_parse_result::Error;
    }
    if (!validate_config(*out_config, out_error)) {
        return observer_parse_result::Error;
    }

    return observer_parse_result::Ok;
}

}  // namespace acct_service
