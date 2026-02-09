#include "core/config_manager.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <fstream>
#include <string>

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

    return true;
}

void write_strategy(std::ofstream& out, split_strategy_t strategy) {
    switch (strategy) {
        case split_strategy_t::FixedSize:
            out << "fixed_size";
            break;
        case split_strategy_t::TWAP:
            out << "twap";
            break;
        case split_strategy_t::VWAP:
            out << "vwap";
            break;
        case split_strategy_t::Iceberg:
            out << "iceberg";
            break;
        default:
            out << "none";
            break;
    }
}

}  // namespace

bool config_manager::load_from_file(const std::string& config_path) {
    std::ifstream in(config_path);
    if (!in.is_open()) {
        return report_config_error(error_code::ConfigParseFailed, "failed to open config file");
    }

    config loaded = config_;
    loaded.config_file = config_path;

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment_pos = line.find_first_of("#;");
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }

        line = trim_copy(std::move(line));
        if (line.empty()) {
            continue;
        }

        if (line.front() == static_cast<char>(91) && line.back() == static_cast<char>(93)) {
            section = trim_copy(line.substr(1, line.size() - 2));
            continue;
        }

        const std::size_t equal_pos = line.find("=");
        if (equal_pos == std::string::npos) {
            continue;
        }

        std::string key = trim_copy(line.substr(0, equal_pos));
        std::string value = trim_copy(line.substr(equal_pos + 1));

        if (value.size() >= 2 && value.front() == static_cast<char>(34) && value.back() == static_cast<char>(34)) {
            value = value.substr(1, value.size() - 2);
        }

        const std::string full_key = section.empty() ? key : section + "." + key;
        if (!apply_value(loaded, full_key, value)) {
            return report_config_error(error_code::ConfigParseFailed, "failed to parse config key");
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

    if (config_.shm.upstream_shm_name.empty() || config_.shm.downstream_shm_name.empty() ||
        config_.shm.trades_shm_name.empty() || config_.shm.positions_shm_name.empty()) {
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

    out << "account_id=" << config_.account_id << "\n\n";

    out << "[shm]\n";
    out << "upstream_shm_name=\"" << config_.shm.upstream_shm_name << "\"\n";
    out << "downstream_shm_name=\"" << config_.shm.downstream_shm_name << "\"\n";
    out << "trades_shm_name=\"" << config_.shm.trades_shm_name << "\"\n";
    out << "positions_shm_name=\"" << config_.shm.positions_shm_name << "\"\n";
    out << "create_if_not_exist=" << (config_.shm.create_if_not_exist ? "true" : "false") << "\n\n";

    out << "[event_loop]\n";
    out << "busy_polling=" << (config_.event_loop.busy_polling ? "true" : "false") << "\n";
    out << "poll_batch_size=" << config_.event_loop.poll_batch_size << "\n";
    out << "idle_sleep_us=" << config_.event_loop.idle_sleep_us << "\n";
    out << "stats_interval_ms=" << config_.event_loop.stats_interval_ms << "\n";
    out << "pin_cpu=" << (config_.event_loop.pin_cpu ? "true" : "false") << "\n";
    out << "cpu_core=" << config_.event_loop.cpu_core << "\n\n";

    out << "[risk]\n";
    out << "max_order_value=" << config_.risk.max_order_value << "\n";
    out << "max_order_volume=" << config_.risk.max_order_volume << "\n";
    out << "max_daily_turnover=" << config_.risk.max_daily_turnover << "\n";
    out << "max_orders_per_second=" << config_.risk.max_orders_per_second << "\n";
    out << "enable_price_limit_check=" << (config_.risk.enable_price_limit_check ? "true" : "false") << "\n";
    out << "enable_duplicate_check=" << (config_.risk.enable_duplicate_check ? "true" : "false") << "\n";
    out << "enable_fund_check=" << (config_.risk.enable_fund_check ? "true" : "false") << "\n";
    out << "enable_position_check=" << (config_.risk.enable_position_check ? "true" : "false") << "\n";
    out << "duplicate_window_ns=" << config_.risk.duplicate_window_ns << "\n\n";

    out << "[split]\n";
    out << "strategy=\"";
    write_strategy(out, config_.split.strategy);
    out << "\"\n";
    out << "max_child_volume=" << config_.split.max_child_volume << "\n";
    out << "min_child_volume=" << config_.split.min_child_volume << "\n";
    out << "max_child_count=" << config_.split.max_child_count << "\n";
    out << "interval_ms=" << config_.split.interval_ms << "\n";
    out << "randomize_factor=" << config_.split.randomize_factor << "\n\n";

    out << "[log]\n";
    out << "log_dir=\"" << config_.log.log_dir << "\"\n";
    out << "log_level=\"" << config_.log.log_level << "\"\n";
    out << "async_logging=" << (config_.log.async_logging ? "true" : "false") << "\n";
    out << "async_queue_size=" << config_.log.async_queue_size << "\n\n";

    out << "[db]\n";
    out << "db_path=\"" << config_.db.db_path << "\"\n";
    out << "enable_persistence=" << (config_.db.enable_persistence ? "true" : "false") << "\n";
    out << "sync_interval_ms=" << config_.db.sync_interval_ms << "\n";

    return true;
}

}  // namespace acct_service
