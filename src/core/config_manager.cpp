#include "core/config_manager.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

#include <yaml-cpp/yaml.h>

#include "common/error.hpp"
#include "common/log.hpp"

namespace acct_service {

namespace {

bool report_config_error(error_code code, std::string_view message) {
    error_status status = ACCT_MAKE_ERROR(error_domain::config, code, "config_manager", message, 0);
    record_error(status);
    ACCT_LOG_ERROR_STATUS(status);
    return false;
}

std::string trim_copy(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };

    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool parse_bool(std::string value, bool& out) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_u32(const std::string& value, uint32_t& out) {
    try {
        const unsigned long long parsed = std::stoull(trim_copy(value));
        if (parsed > UINT32_MAX) {
            return false;
        }
        out = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_u64(const std::string& value, uint64_t& out) {
    try {
        out = static_cast<uint64_t>(std::stoull(trim_copy(value)));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_i32(const std::string& value, int& out) {
    try {
        const long long parsed = std::stoll(trim_copy(value));
        if (parsed < static_cast<long long>(INT_MIN) || parsed > static_cast<long long>(INT_MAX)) {
            return false;
        }
        out = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_double(const std::string& value, double& out) {
    try {
        out = std::stod(trim_copy(value));
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_split_strategy(std::string value, split_strategy_t& out) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (value == "none") {
        out = split_strategy_t::None;
        return true;
    }
    if (value == "fixed" || value == "fixed_size" || value == "fixedsize") {
        out = split_strategy_t::FixedSize;
        return true;
    }
    if (value == "twap") {
        out = split_strategy_t::TWAP;
        return true;
    }
    if (value == "vwap") {
        out = split_strategy_t::VWAP;
        return true;
    }
    if (value == "iceberg") {
        out = split_strategy_t::Iceberg;
        return true;
    }
    return false;
}

const char* split_strategy_to_string(split_strategy_t strategy) {
    switch (strategy) {
        case split_strategy_t::FixedSize:
            return "fixed_size";
        case split_strategy_t::TWAP:
            return "twap";
        case split_strategy_t::VWAP:
            return "vwap";
        case split_strategy_t::Iceberg:
            return "iceberg";
        default:
            return "none";
    }
}

bool is_valid_trading_day_value(std::string_view trading_day) {
    if (trading_day.size() != 8) {
        return false;
    }

    for (char ch : trading_day) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

std::string escape_yaml_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

bool apply_value(config& cfg, const std::string& key, const std::string& raw_value) {
    const std::string value = trim_copy(raw_value);

    if (key == "account_id") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            return false;
        }
        cfg.account_id = static_cast<account_id_t>(parsed);
        return true;
    }
    if (key == "trading_day") {
        if (!is_valid_trading_day_value(value)) {
            return false;
        }
        cfg.trading_day = value;
        return true;
    }

    if (key == "shm.upstream_shm_name") {
        cfg.shm.upstream_shm_name = value;
        return true;
    }
    if (key == "shm.downstream_shm_name") {
        cfg.shm.downstream_shm_name = value;
        return true;
    }
    if (key == "shm.trades_shm_name") {
        cfg.shm.trades_shm_name = value;
        return true;
    }
    if (key == "shm.orders_shm_name") {
        cfg.shm.orders_shm_name = value;
        return true;
    }
    if (key == "shm.positions_shm_name") {
        cfg.shm.positions_shm_name = value;
        return true;
    }
    if (key == "shm.create_if_not_exist") {
        return parse_bool(value, cfg.shm.create_if_not_exist);
    }

    if (key == "event_loop.busy_polling") {
        return parse_bool(value, cfg.event_loop.busy_polling);
    }
    if (key == "event_loop.poll_batch_size") {
        return parse_u32(value, cfg.event_loop.poll_batch_size);
    }
    if (key == "event_loop.idle_sleep_us") {
        return parse_u32(value, cfg.event_loop.idle_sleep_us);
    }
    if (key == "event_loop.stats_interval_ms") {
        return parse_u32(value, cfg.event_loop.stats_interval_ms);
    }
    if (key == "event_loop.pin_cpu") {
        return parse_bool(value, cfg.event_loop.pin_cpu);
    }
    if (key == "event_loop.cpu_core") {
        int parsed = -1;
        if (!parse_i32(value, parsed)) {
            return false;
        }
        cfg.event_loop.cpu_core = parsed;
        return true;
    }

    if (key == "risk.max_order_value") {
        return parse_u64(value, cfg.risk.max_order_value);
    }
    if (key == "risk.max_order_volume") {
        return parse_u64(value, cfg.risk.max_order_volume);
    }
    if (key == "risk.max_daily_turnover") {
        return parse_u64(value, cfg.risk.max_daily_turnover);
    }
    if (key == "risk.max_orders_per_second") {
        return parse_u32(value, cfg.risk.max_orders_per_second);
    }
    if (key == "risk.enable_price_limit_check") {
        return parse_bool(value, cfg.risk.enable_price_limit_check);
    }
    if (key == "risk.enable_duplicate_check") {
        return parse_bool(value, cfg.risk.enable_duplicate_check);
    }
    if (key == "risk.enable_fund_check") {
        return parse_bool(value, cfg.risk.enable_fund_check);
    }
    if (key == "risk.enable_position_check") {
        return parse_bool(value, cfg.risk.enable_position_check);
    }
    if (key == "risk.duplicate_window_ns") {
        return parse_u64(value, cfg.risk.duplicate_window_ns);
    }

    if (key == "split.strategy") {
        return parse_split_strategy(value, cfg.split.strategy);
    }
    if (key == "split.max_child_volume") {
        return parse_u64(value, cfg.split.max_child_volume);
    }
    if (key == "split.min_child_volume") {
        return parse_u64(value, cfg.split.min_child_volume);
    }
    if (key == "split.max_child_count") {
        return parse_u32(value, cfg.split.max_child_count);
    }
    if (key == "split.interval_ms") {
        return parse_u32(value, cfg.split.interval_ms);
    }
    if (key == "split.randomize_factor") {
        return parse_double(value, cfg.split.randomize_factor);
    }

    if (key == "log.log_dir") {
        cfg.log.log_dir = value;
        return true;
    }
    if (key == "log.log_level") {
        cfg.log.log_level = value;
        return true;
    }
    if (key == "log.async_logging") {
        return parse_bool(value, cfg.log.async_logging);
    }
    if (key == "log.async_queue_size") {
        uint64_t parsed = 0;
        if (!parse_u64(value, parsed)) {
            return false;
        }
        cfg.log.async_queue_size = static_cast<std::size_t>(parsed);
        return true;
    }

    if (key == "db.db_path") {
        cfg.db.db_path = value;
        return true;
    }
    if (key == "db.enable_persistence") {
        return parse_bool(value, cfg.db.enable_persistence);
    }
    if (key == "db.sync_interval_ms") {
        return parse_u32(value, cfg.db.sync_interval_ms);
    }

    return false;
}

bool contains_key(std::string_view key, std::initializer_list<std::string_view> allowed_keys) {
    for (std::string_view allowed : allowed_keys) {
        if (key == allowed) {
            return true;
        }
    }
    return false;
}

bool report_yaml_parse_error(std::string_view path, std::string_view reason) {
    std::string message;
    message.reserve(path.size() + reason.size() + 32);
    message += "invalid config key '";
    message += path;
    message += "': ";
    message += reason;
    return report_config_error(error_code::ConfigParseFailed, message);
}

bool read_scalar_string(const YAML::Node& node, std::string_view path, std::string& out) {
    if (!node.IsScalar()) {
        return report_yaml_parse_error(path, "value must be scalar");
    }

    try {
        out = node.as<std::string>();
    } catch (const YAML::Exception& ex) {
        return report_yaml_parse_error(path, ex.what());
    }
    return true;
}

bool ensure_mapping(const YAML::Node& node, std::string_view section_name) {
    if (node.IsMap()) {
        return true;
    }

    std::string message;
    message.reserve(section_name.size() + 24);
    message += "section '";
    message += section_name;
    message += "' must be a map";
    return report_config_error(error_code::ConfigParseFailed, message);
}

bool check_allowed_keys(const YAML::Node& map_node, std::string_view section_name,
    std::initializer_list<std::string_view> allowed_keys) {
    for (const auto& entry : map_node) {
        const YAML::Node key_node = entry.first;
        if (!key_node.IsScalar()) {
            if (section_name.empty()) {
                return report_config_error(error_code::ConfigParseFailed, "non-scalar root key is not allowed");
            }
            std::string message;
            message.reserve(section_name.size() + 32);
            message += "non-scalar key in section '";
            message += section_name;
            message += "' is not allowed";
            return report_config_error(error_code::ConfigParseFailed, message);
        }

        std::string key;
        try {
            key = key_node.as<std::string>();
        } catch (const YAML::Exception& ex) {
            if (section_name.empty()) {
                return report_config_error(error_code::ConfigParseFailed, ex.what());
            }
            return report_yaml_parse_error(section_name, ex.what());
        }

        if (!contains_key(key, allowed_keys)) {
            if (section_name.empty()) {
                return report_yaml_parse_error(key, "unknown key");
            }
            std::string path;
            path.reserve(section_name.size() + key.size() + 1);
            path += section_name;
            path += '.';
            path += key;
            return report_yaml_parse_error(path, "unknown key");
        }
    }

    return true;
}

bool parse_section(config& cfg, const YAML::Node& root, std::string_view section_name,
    std::initializer_list<std::string_view> allowed_keys) {
    const YAML::Node section = root[std::string(section_name)];
    if (!section) {
        return true;
    }

    if (!ensure_mapping(section, section_name)) {
        return false;
    }

    if (!check_allowed_keys(section, section_name, allowed_keys)) {
        return false;
    }

    for (const auto& entry : section) {
        const std::string key = entry.first.as<std::string>();

        std::string value;
        std::string path;
        path.reserve(section_name.size() + key.size() + 1);
        path += section_name;
        path += '.';
        path += key;

        if (!read_scalar_string(entry.second, path, value)) {
            return false;
        }

        if (!apply_value(cfg, path, value)) {
            return report_yaml_parse_error(path, "invalid value");
        }
    }

    return true;
}

}  // namespace

bool config_manager::load_from_file(const std::string& config_path) {
    config loaded = config_;
    loaded.config_file = config_path;

    YAML::Node root;
    try {
        root = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& ex) {
        return report_config_error(error_code::ConfigParseFailed, ex.what());
    }

    if (root && !root.IsNull() && !root.IsMap()) {
        return report_config_error(error_code::ConfigParseFailed, "root YAML node must be a map");
    }

    if (root && root.IsMap()) {
        if (!check_allowed_keys(root, "", {"account_id", "trading_day", "shm", "event_loop", "risk", "split", "log", "db"})) {
            return false;
        }

        const YAML::Node account_id_node = root["account_id"];
        if (account_id_node) {
            std::string value;
            if (!read_scalar_string(account_id_node, "account_id", value)) {
                return false;
            }
            if (!apply_value(loaded, "account_id", value)) {
                return report_yaml_parse_error("account_id", "invalid value");
            }
        }

        const YAML::Node trading_day_node = root["trading_day"];
        if (trading_day_node) {
            std::string value;
            if (!read_scalar_string(trading_day_node, "trading_day", value)) {
                return false;
            }
            if (!apply_value(loaded, "trading_day", value)) {
                return report_yaml_parse_error("trading_day", "invalid value");
            }
        }

        if (!parse_section(loaded, root, "shm",
                {"upstream_shm_name", "downstream_shm_name", "trades_shm_name", "orders_shm_name", "positions_shm_name",
                    "create_if_not_exist"})) {
            return false;
        }

        if (!parse_section(loaded, root, "event_loop",
                {"busy_polling", "poll_batch_size", "idle_sleep_us", "stats_interval_ms", "pin_cpu", "cpu_core"})) {
            return false;
        }

        if (!parse_section(loaded, root, "risk",
                {"max_order_value", "max_order_volume", "max_daily_turnover", "max_orders_per_second",
                    "enable_price_limit_check", "enable_duplicate_check", "enable_fund_check",
                    "enable_position_check", "duplicate_window_ns"})) {
            return false;
        }

        if (!parse_section(loaded, root, "split",
                {"strategy", "max_child_volume", "min_child_volume", "max_child_count", "interval_ms",
                    "randomize_factor"})) {
            return false;
        }

        if (!parse_section(loaded, root, "log", {"log_dir", "log_level", "async_logging", "async_queue_size"})) {
            return false;
        }

        if (!parse_section(loaded, root, "db", {"db_path", "enable_persistence", "sync_interval_ms"})) {
            return false;
        }
    }

    config_ = loaded;
    config_path_ = config_path;
    return validate();
}

bool config_manager::parse_command_line(int argc, char* argv[]) {
    if (argc <= 1 || !argv) {
        return validate();
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        auto consume_value = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };

        std::string value;
        if (arg == "--config") {
            if (!consume_value(value)) {
                return report_config_error(error_code::InvalidConfig, "missing value for --config");
            }
            if (!load_from_file(value)) {
                return false;
            }
            continue;
        }
        if (arg == "--account-id") {
            if (!consume_value(value) || !apply_value(config_, "account_id", value)) {
                return report_config_error(error_code::InvalidConfig, "invalid --account-id");
            }
            continue;
        }
        if (arg == "--trading-day") {
            if (!consume_value(value) || !apply_value(config_, "trading_day", value)) {
                return report_config_error(error_code::InvalidConfig, "invalid --trading-day");
            }
            continue;
        }
        if (arg == "--upstream-shm") {
            if (!consume_value(value)) {
                return false;
            }
            config_.shm.upstream_shm_name = value;
            continue;
        }
        if (arg == "--downstream-shm") {
            if (!consume_value(value)) {
                return false;
            }
            config_.shm.downstream_shm_name = value;
            continue;
        }
        if (arg == "--positions-shm") {
            if (!consume_value(value)) {
                return false;
            }
            config_.shm.positions_shm_name = value;
            continue;
        }
        if (arg == "--trades-shm") {
            if (!consume_value(value)) {
                return false;
            }
            config_.shm.trades_shm_name = value;
            continue;
        }
        if (arg == "--orders-shm") {
            if (!consume_value(value)) {
                return false;
            }
            config_.shm.orders_shm_name = value;
            continue;
        }
        if (arg == "--poll-batch") {
            if (!consume_value(value) || !apply_value(config_, "event_loop.poll_batch_size", value)) {
                return report_config_error(error_code::InvalidConfig, "invalid --poll-batch");
            }
            continue;
        }
        if (arg == "--idle-sleep-us") {
            if (!consume_value(value) || !apply_value(config_, "event_loop.idle_sleep_us", value)) {
                return report_config_error(error_code::InvalidConfig, "invalid --idle-sleep-us");
            }
            continue;
        }
        if (arg == "--split-strategy") {
            if (!consume_value(value) || !apply_value(config_, "split.strategy", value)) {
                return report_config_error(error_code::InvalidConfig, "invalid --split-strategy");
            }
            continue;
        }
        if (arg == "--max-child-volume") {
            if (!consume_value(value) || !apply_value(config_, "split.max_child_volume", value)) {
                return report_config_error(error_code::InvalidConfig, "invalid --max-child-volume");
            }
            continue;
        }
    }

    return validate();
}

bool config_manager::validate() const {
    if (config_.account_id == 0) {
        (void)report_config_error(error_code::ConfigValidateFailed, "account_id must be non-zero");
        return false;
    }

    if (!is_valid_trading_day_value(config_.trading_day)) {
        (void)report_config_error(error_code::ConfigValidateFailed, "trading_day must be YYYYMMDD");
        return false;
    }

    if (config_.shm.upstream_shm_name.empty() || config_.shm.downstream_shm_name.empty() ||
        config_.shm.trades_shm_name.empty() || config_.shm.orders_shm_name.empty() ||
        config_.shm.positions_shm_name.empty()) {
        (void)report_config_error(error_code::ConfigValidateFailed, "shm names must be non-empty");
        return false;
    }

    if (config_.event_loop.poll_batch_size == 0) {
        (void)report_config_error(error_code::ConfigValidateFailed, "poll_batch_size must be non-zero");
        return false;
    }

    if (config_.split.strategy != split_strategy_t::None && config_.split.max_child_count == 0) {
        (void)report_config_error(error_code::ConfigValidateFailed, "split max_child_count must be non-zero");
        return false;
    }

    return true;
}

const config& config_manager::get() const noexcept { return config_; }

config& config_manager::get() noexcept { return config_; }

account_id_t config_manager::account_id() const noexcept { return config_.account_id; }

const shm_config& config_manager::shm() const noexcept { return config_.shm; }

const event_loop_config& config_manager::event_loop() const noexcept { return config_.event_loop; }

const risk_config& config_manager::risk() const noexcept { return config_.risk; }

const split_config& config_manager::split() const noexcept { return config_.split; }

const log_config& config_manager::log() const noexcept { return config_.log; }

const db_config& config_manager::db() const noexcept { return config_.db; }

bool config_manager::reload() {
    if (config_path_.empty()) {
        return report_config_error(error_code::InvalidState, "reload requested before load_from_file");
    }
    return load_from_file(config_path_);
}

bool config_manager::export_to_file(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) {
        return report_config_error(error_code::InvalidConfig, "failed to open config export path");
    }

    out << "account_id: " << config_.account_id << "\n\n";
    out << "trading_day: \"" << escape_yaml_string(config_.trading_day) << "\"\n\n";

    out << "shm:\n";
    out << "  upstream_shm_name: \"" << escape_yaml_string(config_.shm.upstream_shm_name) << "\"\n";
    out << "  downstream_shm_name: \"" << escape_yaml_string(config_.shm.downstream_shm_name) << "\"\n";
    out << "  trades_shm_name: \"" << escape_yaml_string(config_.shm.trades_shm_name) << "\"\n";
    out << "  orders_shm_name: \"" << escape_yaml_string(config_.shm.orders_shm_name) << "\"\n";
    out << "  positions_shm_name: \"" << escape_yaml_string(config_.shm.positions_shm_name) << "\"\n";
    out << "  create_if_not_exist: " << (config_.shm.create_if_not_exist ? "true" : "false") << "\n\n";

    out << "event_loop:\n";
    out << "  busy_polling: " << (config_.event_loop.busy_polling ? "true" : "false") << "\n";
    out << "  poll_batch_size: " << config_.event_loop.poll_batch_size << "\n";
    out << "  idle_sleep_us: " << config_.event_loop.idle_sleep_us << "\n";
    out << "  stats_interval_ms: " << config_.event_loop.stats_interval_ms << "\n";
    out << "  pin_cpu: " << (config_.event_loop.pin_cpu ? "true" : "false") << "\n";
    out << "  cpu_core: " << config_.event_loop.cpu_core << "\n\n";

    out << "risk:\n";
    out << "  max_order_value: " << config_.risk.max_order_value << "\n";
    out << "  max_order_volume: " << config_.risk.max_order_volume << "\n";
    out << "  max_daily_turnover: " << config_.risk.max_daily_turnover << "\n";
    out << "  max_orders_per_second: " << config_.risk.max_orders_per_second << "\n";
    out << "  enable_price_limit_check: " << (config_.risk.enable_price_limit_check ? "true" : "false") << "\n";
    out << "  enable_duplicate_check: " << (config_.risk.enable_duplicate_check ? "true" : "false") << "\n";
    out << "  enable_fund_check: " << (config_.risk.enable_fund_check ? "true" : "false") << "\n";
    out << "  enable_position_check: " << (config_.risk.enable_position_check ? "true" : "false") << "\n";
    out << "  duplicate_window_ns: " << config_.risk.duplicate_window_ns << "\n\n";

    out << "split:\n";
    out << "  strategy: \"" << split_strategy_to_string(config_.split.strategy) << "\"\n";
    out << "  max_child_volume: " << config_.split.max_child_volume << "\n";
    out << "  min_child_volume: " << config_.split.min_child_volume << "\n";
    out << "  max_child_count: " << config_.split.max_child_count << "\n";
    out << "  interval_ms: " << config_.split.interval_ms << "\n";
    out << "  randomize_factor: " << config_.split.randomize_factor << "\n\n";

    out << "log:\n";
    out << "  log_dir: \"" << escape_yaml_string(config_.log.log_dir) << "\"\n";
    out << "  log_level: \"" << escape_yaml_string(config_.log.log_level) << "\"\n";
    out << "  async_logging: " << (config_.log.async_logging ? "true" : "false") << "\n";
    out << "  async_queue_size: " << config_.log.async_queue_size << "\n\n";

    out << "db:\n";
    out << "  db_path: \"" << escape_yaml_string(config_.db.db_path) << "\"\n";
    out << "  enable_persistence: " << (config_.db.enable_persistence ? "true" : "false") << "\n";
    out << "  sync_interval_ms: " << config_.db.sync_interval_ms << "\n";

    return true;
}

}  // namespace acct_service
