#include <sys/mman.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include "api/order_api.h"

namespace acct_service {
namespace {

// 下单命令行参数集合。
struct order_submit_cli_options {
    std::string upstream_shm_name{"/strategy_order_shm"};
    std::string orders_shm_name{"/orders_shm"};
    std::string trading_day{"19700101"};
    std::string security_id{};
    bool cleanup_shm_on_exit{true};
    uint8_t side{0};
    uint8_t market{0};
    uint64_t volume{0};
    double price{0.0};
};

// 用 RAII 托管 acct_ctx_t，确保退出路径都能释放上下文。
class acct_context_guard {
public:
    acct_context_guard() = default;
    ~acct_context_guard() { (void)reset(); }

    acct_context_guard(const acct_context_guard&) = delete;
    acct_context_guard& operator=(const acct_context_guard&) = delete;

    acct_ctx_t* out_ctx() { return &ctx_; }
    acct_ctx_t get() const noexcept { return ctx_; }
    acct_error_t reset() noexcept {
        if (ctx_ == nullptr) {
            return ACCT_OK;
        }
        const acct_error_t rc = acct_destroy(ctx_);
        ctx_ = nullptr;
        return rc;
    }

private:
    acct_ctx_t ctx_{nullptr};
};

// 打印命令行帮助，说明必填参数。
void print_usage(const char* program_name) {
    std::fprintf(stderr,
        "Usage: %s --security CODE --side buy|sell --market sz|sh|bj|hk --volume N --price P\n"
        "          [--upstream-shm NAME] [--orders-shm NAME] [--trading-day YYYYMMDD]\n"
        "          [--cleanup-shm-on-exit] [--no-cleanup-shm-on-exit]\n",
        program_name);
}

// 生成按交易日后缀的 orders 共享内存名称。
std::string make_orders_dated_shm_name(std::string_view orders_shm_name, std::string_view trading_day) {
    return std::string(orders_shm_name) + "_" + std::string(trading_day);
}

// 在 CLI 退出前解除共享内存名称，便于测试收尾清理。
bool cleanup_shared_memory(const order_submit_cli_options& options) {
    bool success = true;

    if (shm_unlink(options.upstream_shm_name.c_str()) < 0 && errno != ENOENT) {
        std::fprintf(stderr, "cleanup upstream shm failed: name=%s errno=%d\n", options.upstream_shm_name.c_str(), errno);
        success = false;
    }

    const std::string orders_dated_name = make_orders_dated_shm_name(options.orders_shm_name, options.trading_day);
    if (shm_unlink(orders_dated_name.c_str()) < 0 && errno != ENOENT) {
        std::fprintf(stderr, "cleanup orders shm failed: name=%s errno=%d\n", orders_dated_name.c_str(), errno);
        success = false;
    }

    return success;
}

// 先销毁 API 上下文，再按请求清理共享内存命名空间。
bool finalize_context_and_cleanup_shm(acct_context_guard* ctx_guard, const order_submit_cli_options& options) {
    if (ctx_guard == nullptr) {
        return false;
    }
    const acct_error_t destroy_rc = ctx_guard->reset();
    if (destroy_rc != ACCT_OK) {
        std::fprintf(stderr, "acct_destroy failed: %s\n", acct_strerror(destroy_rc));
        return false;
    }
    return cleanup_shared_memory(options);
}

// 解析买卖方向文本到 C API 枚举值。
bool parse_side(std::string_view text, uint8_t* out_side) {
    if (out_side == nullptr) {
        return false;
    }
    if (text == "buy" || text == "BUY" || text == "1") {
        *out_side = ACCT_SIDE_BUY;
        return true;
    }
    if (text == "sell" || text == "SELL" || text == "2") {
        *out_side = ACCT_SIDE_SELL;
        return true;
    }
    return false;
}

// 解析市场文本到 C API 枚举值。
bool parse_market(std::string_view text, uint8_t* out_market) {
    if (out_market == nullptr) {
        return false;
    }
    if (text == "sz" || text == "SZ" || text == "1") {
        *out_market = ACCT_MARKET_SZ;
        return true;
    }
    if (text == "sh" || text == "SH" || text == "2") {
        *out_market = ACCT_MARKET_SH;
        return true;
    }
    if (text == "bj" || text == "BJ" || text == "3") {
        *out_market = ACCT_MARKET_BJ;
        return true;
    }
    if (text == "hk" || text == "HK" || text == "4") {
        *out_market = ACCT_MARKET_HK;
        return true;
    }
    return false;
}

// 解析 uint64 参数，失败时返回 false。
bool parse_uint64(const char* text, uint64_t* out_value) {
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out_value = static_cast<uint64_t>(parsed);
    return true;
}

// 解析 double 参数，失败时返回 false。
bool parse_double(const char* text, double* out_value) {
    if (text == nullptr || out_value == nullptr) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *out_value = parsed;
    return true;
}

// 解析命令行参数并验证必填项完整性。
bool parse_cli_args(int argc, char** argv, order_submit_cli_options* out_options) {
    if (out_options == nullptr) {
        return false;
    }

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return false;
        }
        if (arg == "--cleanup-shm-on-exit") {
            out_options->cleanup_shm_on_exit = true;
            continue;
        }
        if (arg == "--no-cleanup-shm-on-exit") {
            out_options->cleanup_shm_on_exit = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "missing value for %s\n", arg.c_str());
            return false;
        }

        const char* value = argv[++i];
        if (arg == "--upstream-shm") {
            out_options->upstream_shm_name = value;
        } else if (arg == "--orders-shm") {
            out_options->orders_shm_name = value;
        } else if (arg == "--trading-day") {
            out_options->trading_day = value;
        } else if (arg == "--security") {
            out_options->security_id = value;
        } else if (arg == "--side") {
            if (!parse_side(value, &out_options->side)) {
                std::fprintf(stderr, "invalid --side value: %s\n", value);
                return false;
            }
        } else if (arg == "--market") {
            if (!parse_market(value, &out_options->market)) {
                std::fprintf(stderr, "invalid --market value: %s\n", value);
                return false;
            }
        } else if (arg == "--volume") {
            if (!parse_uint64(value, &out_options->volume)) {
                std::fprintf(stderr, "invalid --volume value: %s\n", value);
                return false;
            }
        } else if (arg == "--price") {
            if (!parse_double(value, &out_options->price)) {
                std::fprintf(stderr, "invalid --price value: %s\n", value);
                return false;
            }
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            return false;
        }
    }

    if (out_options->security_id.empty() || out_options->side == 0 || out_options->market == 0 ||
        out_options->volume == 0 || out_options->price <= 0.0) {
        std::fprintf(stderr, "missing required arguments\n");
        print_usage(argv[0]);
        return false;
    }

    return true;
}

// 判断是否仅请求帮助信息。
bool wants_help(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return true;
        }
    }
    return false;
}

}  // namespace
}  // namespace acct_service

int main(int argc, char** argv) {
    using namespace acct_service;

    // 1) 解析参数并构建初始化配置。
    if (wants_help(argc, argv)) {
        print_usage(argv[0]);
        return 0;
    }
    order_submit_cli_options options{};
    if (!parse_cli_args(argc, argv, &options)) {
        return 1;
    }

    acct_init_options_t init_options{};
    init_options.upstream_shm_name = options.upstream_shm_name.c_str();
    init_options.orders_shm_name = options.orders_shm_name.c_str();
    init_options.trading_day = options.trading_day.c_str();
    init_options.create_if_not_exist = 0;

    // 2) 初始化 API 上下文，并在作用域结束时自动释放。
    acct_context_guard ctx_guard{};
    const acct_error_t init_rc = acct_init_ex(&init_options, ctx_guard.out_ctx());
    if (init_rc != ACCT_OK) {
        std::fprintf(stderr, "acct_init_ex failed: %s\n", acct_strerror(init_rc));
        return 1;
    }

    // 3) 提交订单并将 order_id 输出到 stdout 供脚本断言。
    uint32_t order_id = 0;
    const acct_error_t submit_rc = acct_submit_order(ctx_guard.get(),
        options.security_id.c_str(),
        options.side,
        options.market,
        options.volume,
        options.price,
        0,
        &order_id);
    if (submit_rc != ACCT_OK) {
        std::fprintf(stderr, "acct_submit_order failed: %s\n", acct_strerror(submit_rc));
        if (options.cleanup_shm_on_exit && !finalize_context_and_cleanup_shm(&ctx_guard, options)) {
            std::fprintf(stderr, "cleanup shared memory failed after submit error\n");
        }
        return 1;
    }

    // 4) 可选清理：先关闭上下文，再删除测试共享内存名称。
    if (options.cleanup_shm_on_exit && !finalize_context_and_cleanup_shm(&ctx_guard, options)) {
        return 1;
    }

    std::printf("%u\n", order_id);
    return 0;
}
