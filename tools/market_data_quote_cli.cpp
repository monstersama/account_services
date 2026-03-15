#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "common/security_identity.hpp"
#include "core/config_manager.hpp"
#include "market_data/market_data_service.hpp"

namespace acct_service {
namespace {

// 行情 CLI 命令行参数集合。
struct market_data_quote_cli_options {
    std::string snapshot_shm_name{"/signal_xshg_v2"};
    std::string config_path{};
    std::string internal_security_id{};
    std::string security_id{};
    uint8_t market{0};
    std::size_t levels{5};
    bool list_symbols = false;
    bool snapshot_shm_explicit = false;
};

// 打印命令行帮助，说明支持的两种标的输入方式。
void print_usage(const char* program_name) {
    std::fprintf(stderr,
                 "Usage: %s [--config PATH] [--snapshot-shm NAME] [--list-symbols]\n"
                 "          [--symbol XSHE_000001 | --security 000001 --market sz|sh|bj|hk]\n"
                 "          [--levels N]\n",
                 program_name);
}

// 解析市场文本到内部 Market 枚举。
bool parse_market(std::string_view text, Market& out_market) {
    if (text == "sz" || text == "SZ" || text == "1") {
        out_market = Market::SZ;
        return true;
    }
    if (text == "sh" || text == "SH" || text == "2") {
        out_market = Market::SH;
        return true;
    }
    if (text == "bj" || text == "BJ" || text == "3") {
        out_market = Market::BJ;
        return true;
    }
    if (text == "hk" || text == "HK" || text == "4") {
        out_market = Market::HK;
        return true;
    }
    return false;
}

// 解析无符号整数参数，供 levels 这类选项复用。
bool parse_size_t(const char* text, std::size_t& out_value) {
    if (text == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    out_value = static_cast<std::size_t>(parsed);
    return true;
}

// 解析 CLI 参数，并保证标的输入方式和档位数量合法。
bool parse_cli_args(int argc, char** argv, market_data_quote_cli_options& out_options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }
        if (arg == "--list-symbols") {
            out_options.list_symbols = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", arg.c_str());
            return false;
        }
        const char* value = argv[++i];
        if (arg == "--config") {
            out_options.config_path = value;
        } else if (arg == "--snapshot-shm") {
            out_options.snapshot_shm_name = value;
            out_options.snapshot_shm_explicit = true;
        } else if (arg == "--symbol") {
            out_options.internal_security_id = value;
        } else if (arg == "--security") {
            out_options.security_id = value;
        } else if (arg == "--market") {
            Market market = Market::NotSet;
            if (!parse_market(value, market)) {
                std::fprintf(stderr, "invalid --market value: %s\n", value);
                return false;
            }
            out_options.market = static_cast<uint8_t>(market);
        } else if (arg == "--levels") {
            if (!parse_size_t(value, out_options.levels) || out_options.levels == 0 || out_options.levels > 10) {
                std::fprintf(stderr, "invalid --levels value: %s\n", value);
                return false;
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }

    if (out_options.list_symbols) {
        return true;
    }

    if (!out_options.internal_security_id.empty()) {
        return true;
    }

    if (out_options.security_id.empty() || out_options.market == 0) {
        std::fprintf(stderr, "missing symbol input; use --symbol or --security + --market\n");
        print_usage(argv[0]);
        return false;
    }
    return true;
}

// 从配置文件回填行情共享内存名，供本地调试时直接复用服务配置。
bool apply_config_defaults(market_data_quote_cli_options& options) {
    if (options.config_path.empty() || options.snapshot_shm_explicit) {
        return true;
    }

    ConfigManager config_manager;
    if (!config_manager.load_from_file(options.config_path)) {
        return false;
    }
    options.snapshot_shm_name = config_manager.market_data().snapshot_shm_name;
    return !options.snapshot_shm_name.empty();
}

// 判断是否仅请求帮助信息，便于主函数返回成功退出码。
bool wants_help(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return true;
        }
    }
    return false;
}

// 将 CLI 输入收敛成统一的 canonical internal_security_id。
bool resolve_internal_security_id(const market_data_quote_cli_options& options, InternalSecurityId& out_id) {
    if (!options.internal_security_id.empty()) {
        return normalize_internal_security_id(options.internal_security_id, out_id);
    }
    return build_internal_security_id(static_cast<Market>(options.market), options.security_id, out_id);
}

// 把 prediction 状态转成稳定文本，便于直接观察 CLI 输出。
const char* prediction_state_name(PredictionState state) noexcept {
    switch (state) {
        case PredictionState::Fresh:
            return "fresh";
        case PredictionState::Carried:
            return "carried";
        case PredictionState::None:
        default:
            return "none";
    }
}

// 把单侧 10 档盘口数组完整展开打印，便于对照共享内存布局逐字段核查。
void print_snapshot_levels(const char* side_name, const snapshot_shm::SnapshotLevel (&levels)[10]) {
    for (std::size_t i = 0; i < 10; ++i) {
        std::printf("%s[%zu].price=%u\n", side_name, i, levels[i].price);
        std::printf("%s[%zu].volume=%u\n", side_name, i, levels[i].volume);
    }
}

// 打印当前快照源中的全部可读合约，便于先确认 symbol 再查单个盘口。
void print_symbols(const std::vector<std::string>& symbols) {
    for (const std::string& symbol : symbols) {
        std::printf("%s\n", symbol.c_str());
    }
}

// 把 `MarketDataView` 和其中的完整 `LobSnapshot` 字段全部打印出来，便于逐字段核对行情共享内存内容。
void print_market_data_view(const MarketDataView& view, std::size_t levels) {
    (void)levels;

    std::printf("symbol=%s\n", view.symbol.c_str());
    std::printf("seq=%llu\n", static_cast<unsigned long long>(view.seq));
    std::printf("prediction_state=%s\n", prediction_state_name(view.prediction.state));
    std::printf("prediction_signal=%.6f\n", static_cast<double>(view.prediction.signal));
    std::printf("prediction_publish_seq_no=%llu\n",
                static_cast<unsigned long long>(view.prediction.publish_seq_no));
    std::printf("prediction_publish_mono_ns=%llu\n",
                static_cast<unsigned long long>(view.prediction.publish_mono_ns));
    std::printf("prediction_flags=%u\n", view.prediction.flags);
    std::printf("prediction_has_prediction=%d\n", view.prediction.has_prediction() ? 1 : 0);
    std::printf("prediction_has_fresh_prediction=%d\n", view.prediction.has_fresh_prediction() ? 1 : 0);

    std::printf("snapshot.trade_volume=%llu\n", static_cast<unsigned long long>(view.snapshot.trade_volume));
    std::printf("snapshot.trade_amount=%llu\n", static_cast<unsigned long long>(view.snapshot.trade_amount));
    std::printf("snapshot.high=%u\n", view.snapshot.high);
    std::printf("snapshot.low=%u\n", view.snapshot.low);
    std::printf("snapshot.total_bid_orders=%d\n", view.snapshot.total_bid_orders);
    std::printf("snapshot.total_ask_orders=%d\n", view.snapshot.total_ask_orders);
    std::printf("snapshot.total_ask_volumes=%llu\n", static_cast<unsigned long long>(view.snapshot.total_ask_volumes));
    std::printf("snapshot.total_bid_volumes=%llu\n", static_cast<unsigned long long>(view.snapshot.total_bid_volumes));
    std::printf("snapshot.time_of_day=%u\n", view.snapshot.time_of_day);
    std::printf("snapshot.total_bid_levels=%u\n", static_cast<unsigned>(view.snapshot.total_bid_levels));
    std::printf("snapshot.total_ask_levels=%u\n", static_cast<unsigned>(view.snapshot.total_ask_levels));

    print_snapshot_levels("snapshot.bids", view.snapshot.bids);
    print_snapshot_levels("snapshot.asks", view.snapshot.asks);
}

}  // namespace
}  // namespace acct_service

int main(int argc, char** argv) {
    using namespace acct_service;

    if (wants_help(argc, argv)) {
        print_usage(argv[0]);
        return 0;
    }

    market_data_quote_cli_options options{};
    if (!parse_cli_args(argc, argv, options)) {
        return 1;
    }
    if (!apply_config_defaults(options)) {
        std::fprintf(stderr, "failed to load snapshot shm name from config: %s\n", options.config_path.c_str());
        return 1;
    }

    MarketDataConfig config{};
    config.enabled = true;
    config.snapshot_shm_name = options.snapshot_shm_name;

    MarketDataService market_data_service(config);
    if (!market_data_service.initialize()) {
        std::fprintf(stderr, "failed to initialize market data reader: shm=%s\n", options.snapshot_shm_name.c_str());
        return 1;
    }

    if (options.list_symbols) {
        print_symbols(market_data_service.list_symbols());
        return 0;
    }

    InternalSecurityId internal_security_id;
    if (!resolve_internal_security_id(options, internal_security_id)) {
        std::fprintf(stderr, "failed to normalize symbol input\n");
        return 1;
    }

    MarketDataView view{};
    if (!market_data_service.read(internal_security_id.view(), view)) {
        std::fprintf(stderr, "failed to read market data for %s\n", internal_security_id.c_str());
        return 1;
    }

    print_market_data_view(view, options.levels);
    return 0;
}
