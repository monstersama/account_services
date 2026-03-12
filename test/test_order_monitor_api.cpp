#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>

#include "api/order_api.h"
#include "api/order_monitor_api.h"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

class ScopedEnvOverride {
public:
    ScopedEnvOverride(const char* key, const char* value) : key_(key) {
        const char* existing = std::getenv(key);
        if (existing != nullptr) {
            had_old_value_ = true;
            old_value_ = existing;
        }
        (void)::setenv(key, value, 1);
    }

    ~ScopedEnvOverride() {
        if (had_old_value_) {
            (void)::setenv(key_.c_str(), old_value_.c_str(), 1);
        } else {
            (void)::unsetenv(key_.c_str());
        }
    }

private:
    std::string key_;
    std::string old_value_;
    bool had_old_value_ = false;
};

std::string unique_shm_name(const char* prefix) {
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return std::string("/") + prefix + "_" + std::to_string(static_cast<unsigned long long>(now_ns));
}

std::string make_orders_name(std::string_view base_name, std::string_view trading_day) {
    std::string name(base_name);
    name += "_";
    name.append(trading_day.data(), trading_day.size());
    return name;
}

void cleanup_shm_name(const std::string& name) {
    if (::shm_unlink(name.c_str()) < 0 && errno != ENOENT) {
        std::perror("shm_unlink");
    }
}

}  // namespace

TEST(strerror) {
    assert(std::strcmp(acct_orders_mon_strerror(ACCT_MON_OK), "Success") == 0);
    assert(std::strcmp(acct_orders_mon_strerror(ACCT_MON_ERR_NOT_FOUND), "Order index not found") == 0);
}

TEST(open_read_close) {
    constexpr const char* kTradingDay = "20260225";
    const std::string upstream_name = unique_shm_name("acct_orders_mon_upstream");
    const std::string orders_base_name = unique_shm_name("acct_orders_mon_orders");
    const std::string dated_orders_name = make_orders_name(orders_base_name, kTradingDay);
    cleanup_shm_name(upstream_name);
    cleanup_shm_name(dated_orders_name);

    acct_init_options_t init_options{};
    init_options.upstream_shm_name = upstream_name.c_str();
    init_options.orders_shm_name = orders_base_name.c_str();
    init_options.trading_day = kTradingDay;
    init_options.create_if_not_exist = 1;

    acct_ctx_t order_ctx = nullptr;
    assert(acct_init_ex(&init_options, &order_ctx) == ACCT_OK);
    assert(order_ctx != nullptr);

    uint32_t order_id = 0;
    assert(acct_submit_order(order_ctx, "000001", ACCT_SIDE_BUY, ACCT_MARKET_SZ, 100, 10.5, 0, &order_id) == ACCT_OK);
    assert(order_id > 0);

    acct_orders_mon_options_t mon_options{};
    mon_options.orders_shm_name = orders_base_name.c_str();
    mon_options.trading_day = kTradingDay;

    acct_orders_mon_ctx_t mon_ctx = nullptr;
    assert(acct_orders_mon_open(&mon_options, &mon_ctx) == ACCT_MON_OK);
    assert(mon_ctx != nullptr);

    acct_orders_mon_info_t info{};
    assert(acct_orders_mon_info(mon_ctx, &info) == ACCT_MON_OK);
    assert(info.capacity > 0);
    assert(info.next_index >= 1);
    assert(std::strcmp(info.trading_day, kTradingDay) == 0);

    acct_orders_mon_snapshot_t snapshot{};
    acct_mon_error_t rc = ACCT_MON_ERR_RETRY;
    for (int i = 0; i < 16 && rc == ACCT_MON_ERR_RETRY; ++i) {
        rc = acct_orders_mon_read(mon_ctx, 0, &snapshot);
    }
    assert(rc == ACCT_MON_OK);
    assert(snapshot.index == 0);
    assert(snapshot.internal_order_id == order_id);
    assert(snapshot.stage == ACCT_MON_STAGE_UPSTREAM_QUEUED);
    assert(snapshot.volume_entrust == 100);
    assert(std::strncmp(snapshot.security_id, "000001", 6) == 0);

    assert(acct_orders_mon_close(mon_ctx) == ACCT_MON_OK);
    assert(acct_destroy(order_ctx) == ACCT_OK);
    cleanup_shm_name(upstream_name);
    cleanup_shm_name(dated_orders_name);
}

TEST(read_not_found) {
    constexpr const char* kTradingDay = "20260225";
    const std::string upstream_name = unique_shm_name("acct_orders_mon_nf_upstream");
    const std::string orders_base_name = unique_shm_name("acct_orders_mon_nf_orders");
    const std::string dated_orders_name = make_orders_name(orders_base_name, kTradingDay);
    cleanup_shm_name(upstream_name);
    cleanup_shm_name(dated_orders_name);

    acct_init_options_t init_options{};
    init_options.upstream_shm_name = upstream_name.c_str();
    init_options.orders_shm_name = orders_base_name.c_str();
    init_options.trading_day = kTradingDay;
    init_options.create_if_not_exist = 1;

    acct_ctx_t order_ctx = nullptr;
    assert(acct_init_ex(&init_options, &order_ctx) == ACCT_OK);
    assert(order_ctx != nullptr);

    acct_orders_mon_options_t mon_options{};
    mon_options.orders_shm_name = orders_base_name.c_str();
    mon_options.trading_day = kTradingDay;

    acct_orders_mon_ctx_t mon_ctx = nullptr;
    assert(acct_orders_mon_open(&mon_options, &mon_ctx) == ACCT_MON_OK);

    acct_orders_mon_snapshot_t snapshot{};
    assert(acct_orders_mon_read(mon_ctx, 0, &snapshot) == ACCT_MON_ERR_NOT_FOUND);

    assert(acct_orders_mon_close(mon_ctx) == ACCT_MON_OK);
    assert(acct_destroy(order_ctx) == ACCT_OK);
    cleanup_shm_name(upstream_name);
    cleanup_shm_name(dated_orders_name);
}

TEST(rejects_file_backend_env) {
    ScopedEnvOverride env_override("SHM_USE_FILE", "1");

    acct_orders_mon_ctx_t mon_ctx = nullptr;
    assert(acct_orders_mon_open(nullptr, &mon_ctx) == ACCT_MON_ERR_SHM_FAILED);
    assert(mon_ctx == nullptr);
}

int main() {
    printf("=== Order Monitor API Test Suite ===\n\n");

    RUN_TEST(strerror);
    RUN_TEST(open_read_close);
    RUN_TEST(read_not_found);
    RUN_TEST(rejects_file_backend_env);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
