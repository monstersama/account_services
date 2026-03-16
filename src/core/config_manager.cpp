#include "core/config_manager.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "common/error.hpp"
#include "common/log.hpp"

namespace acct_service {

namespace {

bool report_config_error(ErrorCode code, std::string_view message) {
    ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::config, code, "ConfigManager", message, 0);
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
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

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

bool parse_split_strategy(std::string value, SplitStrategy& out) {
    value = trim_copy(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (value == "none") {
        out = SplitStrategy::None;
        return true;
    }
    if (value == "fixed" || value == "fixed_size" || value == "fixedsize") {
        out = SplitStrategy::FixedSize;
        return true;
    }
    if (value == "twap") {
        out = SplitStrategy::TWAP;
        return true;
    }
    if (value == "vwap") {
        out = SplitStrategy::VWAP;
        return true;
    }
    if (value == "iceberg") {
        out = SplitStrategy::Iceberg;
        return true;
    }
    return false;
}

const char* split_strategy_to_string(SplitStrategy strategy) {
    switch (strategy) {
        case SplitStrategy::FixedSize:
            return "fixed_size";
        case SplitStrategy::TWAP:
            return "twap";
        case SplitStrategy::VWAP:
            return "vwap";
        case SplitStrategy::Iceberg:
            return "iceberg";
        default:
            return "none";
    }
}

std::string escape_yaml_string(std::string_view value);

std::string escape_log_value(std::string_view value);

void write_config_yaml(std::ostream& out, const Config& config) {
    out << "account_id: " << config.account_id << "\n\n";
    out << "trading_day: \"" << escape_yaml_string(config.trading_day) << "\"\n\n";

    out << "shm:\n";
    out << "  upstream_shm_name: \"" << escape_yaml_string(config.shm.upstream_shm_name) << "\"\n";
    out << "  downstream_shm_name: \"" << escape_yaml_string(config.shm.downstream_shm_name) << "\"\n";
    out << "  trades_shm_name: \"" << escape_yaml_string(config.shm.trades_shm_name) << "\"\n";
    out << "  orders_shm_name: \"" << escape_yaml_string(config.shm.orders_shm_name) << "\"\n";
    out << "  positions_shm_name: \"" << escape_yaml_string(config.shm.positions_shm_name) << "\"\n";
    out << "  create_if_not_exist: " << (config.shm.create_if_not_exist ? "true" : "false") << "\n\n";

    out << "EventLoop:\n";
    out << "  busy_polling: " << (config.EventLoop.busy_polling ? "true" : "false") << "\n";
    out << "  poll_batch_size: " << config.EventLoop.poll_batch_size << "\n";
    out << "  idle_sleep_us: " << config.EventLoop.idle_sleep_us << "\n";
    out << "  stats_interval_ms: " << config.EventLoop.stats_interval_ms << "\n";
    out << "  archive_terminal_orders: " << (config.EventLoop.archive_terminal_orders ? "true" : "false") << "\n";
    out << "  terminal_archive_delay_ms: " << config.EventLoop.terminal_archive_delay_ms << "\n";
    out << "  pin_cpu: " << (config.EventLoop.pin_cpu ? "true" : "false") << "\n";
    out << "  cpu_core: " << config.EventLoop.cpu_core << "\n\n";

    out << "market_data:\n";
    out << "  enabled: " << (config.market_data.enabled ? "true" : "false") << "\n";
    out << "  snapshot_shm_name: \"" << escape_yaml_string(config.market_data.snapshot_shm_name) << "\"\n";
    out << "  allow_order_price_fallback: " << (config.market_data.allow_order_price_fallback ? "true" : "false")
        << "\n\n";

    out << "active_strategy:\n";
    out << "  enabled: " << (config.active_strategy.enabled ? "true" : "false") << "\n";
    out << "  name: \"" << escape_yaml_string(config.active_strategy.name) << "\"\n";
    out << "  signal_threshold: " << config.active_strategy.signal_threshold << "\n\n";

    out << "risk:\n";
    out << "  max_order_value: " << config.risk.max_order_value << "\n";
    out << "  max_order_volume: " << config.risk.max_order_volume << "\n";
    out << "  max_daily_turnover: " << config.risk.max_daily_turnover << "\n";
    out << "  max_orders_per_second: " << config.risk.max_orders_per_second << "\n";
    out << "  enable_price_limit_check: " << (config.risk.enable_price_limit_check ? "true" : "false") << "\n";
    out << "  enable_duplicate_check: " << (config.risk.enable_duplicate_check ? "true" : "false") << "\n";
    out << "  enable_fund_check: " << (config.risk.enable_fund_check ? "true" : "false") << "\n";
    out << "  enable_position_check: " << (config.risk.enable_position_check ? "true" : "false") << "\n";
    out << "  duplicate_window_ns: " << config.risk.duplicate_window_ns << "\n\n";

    out << "split:\n";
    out << "  strategy: \"" << split_strategy_to_string(config.split.strategy) << "\"\n";
    out << "  max_child_volume: " << config.split.max_child_volume << "\n";
    out << "  min_child_volume: " << config.split.min_child_volume << "\n";
    out << "  max_child_count: " << config.split.max_child_count << "\n";
    out << "  interval_ms: " << config.split.interval_ms << "\n";
    out << "  randomize_factor: " << config.split.randomize_factor << "\n\n";

    out << "log:\n";
    out << "  log_dir: \"" << escape_yaml_string(config.log.log_dir) << "\"\n";
    out << "  log_level: \"" << escape_yaml_string(config.log.log_level) << "\"\n";
    out << "  async_logging: " << (config.log.async_logging ? "true" : "false") << "\n";
    out << "  async_queue_size: " << config.log.async_queue_size << "\n\n";

    out << "business_log:\n";
    out << "  enabled: " << (config.business_log.enabled ? "true" : "false") << "\n";
    out << "  output_dir: \"" << escape_yaml_string(config.business_log.output_dir) << "\"\n";
    out << "  queue_capacity: " << config.business_log.queue_capacity << "\n";
    out << "  flush_interval_ms: " << config.business_log.flush_interval_ms << "\n\n";

    out << "db:\n";
    out << "  db_path: \"" << escape_yaml_string(config.db.db_path) << "\"\n";
    out << "  enable_persistence: " << (config.db.enable_persistence ? "true" : "false") << "\n";
    out << "  sync_interval_ms: " << config.db.sync_interval_ms << "\n";
}

void write_config_log_line(std::ostream& out, std::string_view section, std::string_view key, std::string_view value) {
    out << "[config] [" << section << "] " << key << "=" << escape_log_value(value) << "\n";
}

void write_config_log_line(std::ostream& out, std::string_view section, std::string_view key,
                           const std::string& value) {
    write_config_log_line(out, section, key, std::string_view(value));
}

void write_config_log_line(std::ostream& out, std::string_view section, std::string_view key, const char* value) {
    write_config_log_line(out, section, key, std::string_view(value ? value : ""));
}

void write_config_log_line(std::ostream& out, std::string_view section, std::string_view key, bool value) {
    out << "[config] [" << section << "] " << key << "=" << (value ? "true" : "false") << "\n";
}

template <typename T>
void write_config_log_line(std::ostream& out, std::string_view section, std::string_view key, const T& value) {
    out << "[config] [" << section << "] " << key << "=" << value << "\n";
}

void write_config_log(std::ostream& out, const Config& config) {
    write_config_log_line(out, "meta", "config_file", config.config_file);

    write_config_log_line(out, "root", "account_id", config.account_id);
    write_config_log_line(out, "root", "trading_day", config.trading_day);

    write_config_log_line(out, "shm", "upstream_shm_name", config.shm.upstream_shm_name);
    write_config_log_line(out, "shm", "downstream_shm_name", config.shm.downstream_shm_name);
    write_config_log_line(out, "shm", "trades_shm_name", config.shm.trades_shm_name);
    write_config_log_line(out, "shm", "orders_shm_name", config.shm.orders_shm_name);
    write_config_log_line(out, "shm", "positions_shm_name", config.shm.positions_shm_name);
    write_config_log_line(out, "shm", "create_if_not_exist", config.shm.create_if_not_exist);

    write_config_log_line(out, "event_loop", "busy_polling", config.EventLoop.busy_polling);
    write_config_log_line(out, "event_loop", "poll_batch_size", config.EventLoop.poll_batch_size);
    write_config_log_line(out, "event_loop", "idle_sleep_us", config.EventLoop.idle_sleep_us);
    write_config_log_line(out, "event_loop", "stats_interval_ms", config.EventLoop.stats_interval_ms);
    write_config_log_line(out, "event_loop", "archive_terminal_orders", config.EventLoop.archive_terminal_orders);
    write_config_log_line(out, "event_loop", "terminal_archive_delay_ms", config.EventLoop.terminal_archive_delay_ms);
    write_config_log_line(out, "event_loop", "pin_cpu", config.EventLoop.pin_cpu);
    write_config_log_line(out, "event_loop", "cpu_core", config.EventLoop.cpu_core);

    write_config_log_line(out, "market_data", "enabled", config.market_data.enabled);
    write_config_log_line(out, "market_data", "snapshot_shm_name", config.market_data.snapshot_shm_name);
    write_config_log_line(out, "market_data", "allow_order_price_fallback",
                          config.market_data.allow_order_price_fallback);

    write_config_log_line(out, "active_strategy", "enabled", config.active_strategy.enabled);
    write_config_log_line(out, "active_strategy", "name", config.active_strategy.name);
    write_config_log_line(out, "active_strategy", "signal_threshold", config.active_strategy.signal_threshold);

    write_config_log_line(out, "risk", "max_order_value", config.risk.max_order_value);
    write_config_log_line(out, "risk", "max_order_volume", config.risk.max_order_volume);
    write_config_log_line(out, "risk", "max_daily_turnover", config.risk.max_daily_turnover);
    write_config_log_line(out, "risk", "max_orders_per_second", config.risk.max_orders_per_second);
    write_config_log_line(out, "risk", "enable_price_limit_check", config.risk.enable_price_limit_check);
    write_config_log_line(out, "risk", "enable_duplicate_check", config.risk.enable_duplicate_check);
    write_config_log_line(out, "risk", "enable_fund_check", config.risk.enable_fund_check);
    write_config_log_line(out, "risk", "enable_position_check", config.risk.enable_position_check);
    write_config_log_line(out, "risk", "duplicate_window_ns", config.risk.duplicate_window_ns);

    write_config_log_line(out, "split", "strategy", split_strategy_to_string(config.split.strategy));
    write_config_log_line(out, "split", "max_child_volume", config.split.max_child_volume);
    write_config_log_line(out, "split", "min_child_volume", config.split.min_child_volume);
    write_config_log_line(out, "split", "max_child_count", config.split.max_child_count);
    write_config_log_line(out, "split", "interval_ms", config.split.interval_ms);
    write_config_log_line(out, "split", "randomize_factor", config.split.randomize_factor);

    write_config_log_line(out, "log", "log_dir", config.log.log_dir);
    write_config_log_line(out, "log", "log_level", config.log.log_level);
    write_config_log_line(out, "log", "async_logging", config.log.async_logging);
    write_config_log_line(out, "log", "async_queue_size", config.log.async_queue_size);

    write_config_log_line(out, "business_log", "enabled", config.business_log.enabled);
    write_config_log_line(out, "business_log", "output_dir", config.business_log.output_dir);
    write_config_log_line(out, "business_log", "queue_capacity", config.business_log.queue_capacity);
    write_config_log_line(out, "business_log", "flush_interval_ms", config.business_log.flush_interval_ms);

    write_config_log_line(out, "db", "db_path", config.db.db_path);
    write_config_log_line(out, "db", "enable_persistence", config.db.enable_persistence);
    write_config_log_line(out, "db", "sync_interval_ms", config.db.sync_interval_ms);
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

std::string escape_log_value(std::string_view value) { return escape_yaml_string(value); }

bool apply_value(Config& cfg, const std::string& key, const std::string& raw_value) {
    const std::string value = trim_copy(raw_value);

    if (key == "account_id") {
        uint32_t parsed = 0;
        if (!parse_u32(value, parsed)) {
            return false;
        }
        cfg.account_id = static_cast<AccountId>(parsed);
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

    if (key == "event_loop.busy_polling" || key == "EventLoop.busy_polling") {
        return parse_bool(value, cfg.EventLoop.busy_polling);
    }
    if (key == "event_loop.poll_batch_size" || key == "EventLoop.poll_batch_size") {
        return parse_u32(value, cfg.EventLoop.poll_batch_size);
    }
    if (key == "event_loop.idle_sleep_us" || key == "EventLoop.idle_sleep_us") {
        return parse_u32(value, cfg.EventLoop.idle_sleep_us);
    }
    if (key == "event_loop.stats_interval_ms" || key == "EventLoop.stats_interval_ms") {
        return parse_u32(value, cfg.EventLoop.stats_interval_ms);
    }
    if (key == "event_loop.archive_terminal_orders" || key == "EventLoop.archive_terminal_orders") {
        return parse_bool(value, cfg.EventLoop.archive_terminal_orders);
    }
    if (key == "event_loop.terminal_archive_delay_ms" || key == "EventLoop.terminal_archive_delay_ms") {
        return parse_u32(value, cfg.EventLoop.terminal_archive_delay_ms);
    }
    if (key == "event_loop.pin_cpu" || key == "EventLoop.pin_cpu") {
        return parse_bool(value, cfg.EventLoop.pin_cpu);
    }
    if (key == "event_loop.cpu_core" || key == "EventLoop.cpu_core") {
        int parsed = -1;
        if (!parse_i32(value, parsed)) {
            return false;
        }
        cfg.EventLoop.cpu_core = parsed;
        return true;
    }

    if (key == "market_data.enabled") {
        return parse_bool(value, cfg.market_data.enabled);
    }
    if (key == "market_data.snapshot_shm_name") {
        cfg.market_data.snapshot_shm_name = value;
        return true;
    }
    if (key == "market_data.allow_order_price_fallback") {
        return parse_bool(value, cfg.market_data.allow_order_price_fallback);
    }

    if (key == "active_strategy.enabled") {
        return parse_bool(value, cfg.active_strategy.enabled);
    }
    if (key == "active_strategy.name") {
        cfg.active_strategy.name = value;
        return true;
    }
    if (key == "active_strategy.signal_threshold") {
        return parse_double(value, cfg.active_strategy.signal_threshold);
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

    if (key == "business_log.enabled") {
        return parse_bool(value, cfg.business_log.enabled);
    }
    if (key == "business_log.output_dir") {
        cfg.business_log.output_dir = value;
        return true;
    }
    if (key == "business_log.queue_capacity") {
        uint64_t parsed = 0;
        if (!parse_u64(value, parsed)) {
            return false;
        }
        cfg.business_log.queue_capacity = static_cast<std::size_t>(parsed);
        return true;
    }
    if (key == "business_log.flush_interval_ms") {
        return parse_u32(value, cfg.business_log.flush_interval_ms);
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
    return report_config_error(ErrorCode::ConfigParseFailed, message);
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
    return report_config_error(ErrorCode::ConfigParseFailed, message);
}

bool check_allowed_keys(const YAML::Node& map_node, std::string_view section_name,
                        std::initializer_list<std::string_view> allowed_keys) {
    for (const auto& entry : map_node) {
        const YAML::Node key_node = entry.first;
        if (!key_node.IsScalar()) {
            if (section_name.empty()) {
                return report_config_error(ErrorCode::ConfigParseFailed, "non-scalar root key is not allowed");
            }
            std::string message;
            message.reserve(section_name.size() + 32);
            message += "non-scalar key in section '";
            message += section_name;
            message += "' is not allowed";
            return report_config_error(ErrorCode::ConfigParseFailed, message);
        }

        std::string key;
        try {
            key = key_node.as<std::string>();
        } catch (const YAML::Exception& ex) {
            if (section_name.empty()) {
                return report_config_error(ErrorCode::ConfigParseFailed, ex.what());
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

bool parse_section(Config& cfg, const YAML::Node& root, std::string_view section_name,
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

bool ConfigManager::load_from_file(const std::string& config_path) {
    Config loaded = config_;
    loaded.config_file = config_path;

    YAML::Node root;
    try {
        root = YAML::LoadFile(config_path);
    } catch (const YAML::Exception& ex) {
        return report_config_error(ErrorCode::ConfigParseFailed, ex.what());
    }

    if (root && !root.IsNull() && !root.IsMap()) {
        return report_config_error(ErrorCode::ConfigParseFailed, "root YAML node must be a map");
    }

    if (root && root.IsMap()) {
        if (!check_allowed_keys(root, "",
                                {"account_id", "trading_day", "shm", "event_loop", "EventLoop", "market_data",
                                 "active_strategy", "risk", "split", "log", "business_log", "db"})) {
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
                           {"upstream_shm_name", "downstream_shm_name", "trades_shm_name", "orders_shm_name",
                            "positions_shm_name", "create_if_not_exist"})) {
            return false;
        }

        if (!parse_section(loaded, root, "event_loop",
                           {"busy_polling", "poll_batch_size", "idle_sleep_us", "stats_interval_ms",
                            "archive_terminal_orders", "terminal_archive_delay_ms", "pin_cpu", "cpu_core"})) {
            return false;
        }

        if (!parse_section(loaded, root, "EventLoop",
                           {"busy_polling", "poll_batch_size", "idle_sleep_us", "stats_interval_ms",
                            "archive_terminal_orders", "terminal_archive_delay_ms", "pin_cpu", "cpu_core"})) {
            return false;
        }

        if (!parse_section(loaded, root, "market_data",
                           {"enabled", "snapshot_shm_name", "allow_order_price_fallback"})) {
            return false;
        }

        if (!parse_section(loaded, root, "active_strategy", {"enabled", "name", "signal_threshold"})) {
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

        if (!parse_section(loaded, root, "business_log",
                           {"enabled", "output_dir", "queue_capacity", "flush_interval_ms"})) {
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

bool ConfigManager::parse_command_line(int argc, char* argv[]) {
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
                return report_config_error(ErrorCode::InvalidConfig, "missing value for --config");
            }
            if (!load_from_file(value)) {
                return false;
            }
            continue;
        }
        if (arg == "--account-id") {
            if (!consume_value(value) || !apply_value(config_, "account_id", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --account-id");
            }
            continue;
        }
        if (arg == "--trading-day") {
            if (!consume_value(value) || !apply_value(config_, "trading_day", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --trading-day");
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
            if (!consume_value(value) || !apply_value(config_, "EventLoop.poll_batch_size", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --poll-batch");
            }
            continue;
        }
        if (arg == "--idle-sleep-us") {
            if (!consume_value(value) || !apply_value(config_, "EventLoop.idle_sleep_us", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --idle-sleep-us");
            }
            continue;
        }
        if (arg == "--terminal-archive-delay-ms") {
            if (!consume_value(value) || !apply_value(config_, "EventLoop.terminal_archive_delay_ms", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --terminal-archive-delay-ms");
            }
            continue;
        }
        if (arg == "--archive-terminal-orders") {
            if (!consume_value(value) || !apply_value(config_, "EventLoop.archive_terminal_orders", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --archive-terminal-orders");
            }
            continue;
        }
        if (arg == "--split-strategy") {
            if (!consume_value(value) || !apply_value(config_, "split.strategy", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --split-strategy");
            }
            continue;
        }
        if (arg == "--max-child-volume") {
            if (!consume_value(value) || !apply_value(config_, "split.max_child_volume", value)) {
                return report_config_error(ErrorCode::InvalidConfig, "invalid --max-child-volume");
            }
            continue;
        }
    }

    return validate();
}

bool ConfigManager::validate() const {
    if (config_.account_id == 0) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "account_id must be non-zero");
        return false;
    }

    if (!is_valid_trading_day_value(config_.trading_day)) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "trading_day must be YYYYMMDD");
        return false;
    }

    if (config_.shm.upstream_shm_name.empty() || config_.shm.downstream_shm_name.empty() ||
        config_.shm.trades_shm_name.empty() || config_.shm.orders_shm_name.empty() ||
        config_.shm.positions_shm_name.empty()) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "shm names must be non-empty");
        return false;
    }

    if (config_.market_data.enabled && config_.market_data.snapshot_shm_name.empty()) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "market_data snapshot_shm_name must be non-empty");
        return false;
    }
    if (config_.active_strategy.enabled && !config_.market_data.enabled) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "active_strategy requires market_data.enabled=true");
        return false;
    }
    if (config_.split.strategy != SplitStrategy::None && !config_.market_data.enabled &&
        !config_.market_data.allow_order_price_fallback) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed,
                                  "managed execution requires market_data.enabled=true");
        return false;
    }

    if (config_.EventLoop.poll_batch_size == 0) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "poll_batch_size must be non-zero");
        return false;
    }

    if (config_.split.strategy != SplitStrategy::None && config_.split.max_child_count == 0) {
        (void)report_config_error(ErrorCode::ConfigValidateFailed, "split max_child_count must be non-zero");
        return false;
    }

    if (config_.business_log.enabled) {
        if (config_.business_log.output_dir.empty()) {
            (void)report_config_error(ErrorCode::ConfigValidateFailed, "business_log output_dir must be non-empty");
            return false;
        }
        if (config_.business_log.queue_capacity < 2) {
            (void)report_config_error(ErrorCode::ConfigValidateFailed, "business_log queue_capacity must be >= 2");
            return false;
        }
        if (config_.business_log.queue_capacity >= static_cast<std::size_t>(UINT32_MAX)) {
            (void)report_config_error(ErrorCode::ConfigValidateFailed,
                                      "business_log queue_capacity must fit in uint32_t");
            return false;
        }
        if (config_.business_log.flush_interval_ms == 0) {
            (void)report_config_error(ErrorCode::ConfigValidateFailed,
                                      "business_log flush_interval_ms must be non-zero");
            return false;
        }
    }

    return true;
}

const Config& ConfigManager::get() const noexcept { return config_; }

Config& ConfigManager::get() noexcept { return config_; }

AccountId ConfigManager::account_id() const noexcept { return config_.account_id; }

const SHMConfig& ConfigManager::shm() const noexcept { return config_.shm; }

const EventLoopConfig& ConfigManager::EventLoop() const noexcept { return config_.EventLoop; }

const MarketDataConfig& ConfigManager::market_data() const noexcept { return config_.market_data; }

const ActiveStrategyConfig& ConfigManager::active_strategy() const noexcept { return config_.active_strategy; }

const RiskConfig& ConfigManager::risk() const noexcept { return config_.risk; }

const split_config& ConfigManager::split() const noexcept { return config_.split; }

const LogConfig& ConfigManager::log() const noexcept { return config_.log; }

const BusinessLogConfig& ConfigManager::business_log() const noexcept { return config_.business_log; }

const DBConfig& ConfigManager::db() const noexcept { return config_.db; }

bool ConfigManager::reload() {
    if (config_path_.empty()) {
        return report_config_error(ErrorCode::InvalidState, "reload requested before load_from_file");
    }
    return load_from_file(config_path_);
}

std::string ConfigManager::to_log_string() const {
    std::ostringstream out;
    write_config_log(out, config_);
    return out.str();
}

bool ConfigManager::export_to_file(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) {
        return report_config_error(ErrorCode::InvalidConfig, "failed to open config export path");
    }

    write_config_yaml(out, config_);
    return true;
}

}  // namespace acct_service
