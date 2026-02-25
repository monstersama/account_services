#include "gateway_config.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <string_view>

namespace acct_service::gateway {

namespace {

// 解析 uint32 参数。
bool parse_u32(const std::string& text, uint32_t& out) {
    try {
        const unsigned long long value = std::stoull(text);
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

}  // namespace

void print_usage(const char* program) {
    std::fprintf(stderr,
        "Usage: %s [options]\n"
        "  --account-id <id>            account id\n"
        "  --downstream-shm <name>      downstream order shm name\n"
        "  --trades-shm <name>          trades response shm name\n"
        "  --orders-shm <name>          shared order pool shm base name\n"
        "  --trading-day <YYYYMMDD>     trading day used for dated order shm name\n"
        "  --broker-type <sim|plugin>   broker adapter type\n"
        "  --adapter-so <path>          adapter plugin .so path (required when broker-type=plugin)\n"
        "  --create-if-not-exist <bool> open or create shm (default false)\n"
        "  --poll-batch-size <n>        max orders/events per loop (default 64)\n"
        "  --idle-sleep-us <n>          sleep in idle loop (default 50)\n"
        "  --stats-interval-ms <n>      periodic stats interval (default 1000)\n"
        "  --max-retries <n>            max retry attempts per order (default 3)\n"
        "  --retry-interval-us <n>      retry interval in microseconds (default 200)\n"
        "  -h, --help                   show this help\n",
        program ? program : "acct_broker_gateway");
}

parse_result_t parse_args(int argc, char* argv[], gateway_config& config, std::string& error_message) {
    // 逐项扫描命令行参数，解析到 config。
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }

        if (std::string(arg) == "-h" || std::string(arg) == "--help") {
            print_usage(argv[0]);
            return parse_result_t::Help;
        }

        std::string value;

        if (std::string(arg) == "--account-id") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            uint32_t parsed = 0;
            if (!parse_u32(value, parsed) || parsed == 0) {
                error_message = "invalid --account-id";
                return parse_result_t::Error;
            }
            config.account_id = static_cast<account_id_t>(parsed);
            ++i;
            continue;
        }

        if (std::string(arg) == "--downstream-shm") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            config.downstream_shm_name = value;
            ++i;
            continue;
        }

        if (std::string(arg) == "--trades-shm") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            config.trades_shm_name = value;
            ++i;
            continue;
        }

        if (std::string(arg) == "--orders-shm") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            config.orders_shm_name = value;
            ++i;
            continue;
        }

        if (std::string(arg) == "--trading-day") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            if (!is_valid_trading_day(value)) {
                error_message = "invalid --trading-day";
                return parse_result_t::Error;
            }
            config.trading_day = value;
            ++i;
            continue;
        }

        if (std::string(arg) == "--broker-type") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            config.broker_type = value;
            ++i;
            continue;
        }

        if (std::string(arg) == "--adapter-so") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            config.adapter_plugin_so = value;
            ++i;
            continue;
        }

        if (std::string(arg) == "--create-if-not-exist") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            bool parsed = false;
            if (!parse_bool(value, parsed)) {
                error_message = "invalid --create-if-not-exist";
                return parse_result_t::Error;
            }
            config.create_if_not_exist = parsed;
            ++i;
            continue;
        }

        if (std::string(arg) == "--poll-batch-size") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            if (!parse_u32(value, config.poll_batch_size) || config.poll_batch_size == 0) {
                error_message = "invalid --poll-batch-size";
                return parse_result_t::Error;
            }
            ++i;
            continue;
        }

        if (std::string(arg) == "--idle-sleep-us") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            if (!parse_u32(value, config.idle_sleep_us)) {
                error_message = "invalid --idle-sleep-us";
                return parse_result_t::Error;
            }
            ++i;
            continue;
        }

        if (std::string(arg) == "--stats-interval-ms") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            if (!parse_u32(value, config.stats_interval_ms)) {
                error_message = "invalid --stats-interval-ms";
                return parse_result_t::Error;
            }
            ++i;
            continue;
        }

        if (std::string(arg) == "--max-retries") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            if (!parse_u32(value, config.max_retry_attempts)) {
                error_message = "invalid --max-retries";
                return parse_result_t::Error;
            }
            ++i;
            continue;
        }

        if (std::string(arg) == "--retry-interval-us") {
            if (!require_value(argc, i, argv, value, error_message)) {
                return parse_result_t::Error;
            }
            if (!parse_u32(value, config.retry_interval_us)) {
                error_message = "invalid --retry-interval-us";
                return parse_result_t::Error;
            }
            ++i;
            continue;
        }

        if (std::string(arg).rfind("-", 0) == 0) {
            error_message = std::string("unknown option: ") + arg;
            return parse_result_t::Error;
        }

        error_message = std::string("unexpected positional argument: ") + arg;
        return parse_result_t::Error;
    }

    // 共享内存名称是必须项。
    if (config.downstream_shm_name.empty() || config.trades_shm_name.empty() || config.orders_shm_name.empty()) {
        error_message = "shared memory names must be non-empty";
        return parse_result_t::Error;
    }
    if (!is_valid_trading_day(config.trading_day)) {
        error_message = "trading_day must be YYYYMMDD";
        return parse_result_t::Error;
    }

    if (config.broker_type != "sim" && config.broker_type != "plugin") {
        error_message = "--broker-type must be sim or plugin";
        return parse_result_t::Error;
    }

    if (config.broker_type == "plugin" && config.adapter_plugin_so.empty()) {
        error_message = "--adapter-so is required when --broker-type=plugin";
        return parse_result_t::Error;
    }

    return parse_result_t::Ok;
}

}  // namespace acct_service::gateway
