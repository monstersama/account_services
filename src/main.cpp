#include <cstdio>
#include <cstring>
#include <string>

#include "common/error.hpp"
#include "common/log.hpp"
#include "core/account_service.hpp"

namespace {

constexpr const char* kDefaultConfigPath = "config/default.yaml";

enum class parse_result_t {
    Ok,
    Help,
    Error,
};

void print_usage(const char* program) {
    std::fprintf(stderr,
        "Usage: %s [--config <path>] [config_path]\n"
        "  --config <path>   指定配置文件路径 (默认: config/default.yaml)\n"
        "  -h, --help        显示帮助\n",
        program ? program : "account_service");
}

parse_result_t parse_args(int argc, char* argv[], std::string& config_path) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }

        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return parse_result_t::Help;
        }

        if (std::strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc || argv[i + 1] == nullptr) {
                std::fprintf(stderr, "missing value for --config\n");
                print_usage(argv[0]);
                return parse_result_t::Error;
            }
            config_path = argv[++i];
            continue;
        }

        if (arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg);
            print_usage(argv[0]);
            return parse_result_t::Error;
        }

        if (!config_path.empty()) {
            std::fprintf(stderr, "duplicated config path: %s\n", arg);
            print_usage(argv[0]);
            return parse_result_t::Error;
        }

        config_path = arg;
    }

    return parse_result_t::Ok;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string config_path;
    const parse_result_t args = parse_args(argc, argv, config_path);
    if (args == parse_result_t::Help) {
        return 0;
    }
    if (args == parse_result_t::Error) {
        return 2;
    }

    if (config_path.empty()) {
        config_path = kDefaultConfigPath;
    }

    acct_service::account_service service;
    if (!service.initialize(config_path)) {
        const acct_service::error_status& status = service.last_error();
        std::fprintf(stderr,
            "failed to initialize account_service with config '%s': severity=%s domain=%s code=%s msg=%s\n",
            config_path.c_str(),
            acct_service::to_string(acct_service::classify(status.domain, status.code).severity),
            acct_service::to_string(status.domain), acct_service::to_string(status.code), status.message.c_str());
        return 1;
    }

    const int run_rc = service.run();
    service.print_stats();

    const acct_service::error_severity reason = acct_service::shutdown_reason();
    if (reason >= acct_service::error_severity::Critical) {
        (void)acct_service::flush_logger(200);
        const acct_service::error_status& status = service.last_error();
        std::fprintf(stderr,
            "account_service terminated by policy: severity=%s domain=%s code=%s msg=%s\n",
            acct_service::to_string(acct_service::classify(status.domain, status.code).severity),
            acct_service::to_string(status.domain), acct_service::to_string(status.code), status.message.c_str());
        return 1;
    }

    return (run_rc == 0) ? 0 : 1;
}
