#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "market_data/market_data_service.hpp"
#include "snapshot_shm.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

// 为基于文件后端的 snapshot_reader 测试提供作用域内环境变量覆盖。
class ScopedEnvOverride {
public:
    // 设置一个环境变量，并在析构时自动恢复原值。
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

// 生成唯一文件路径，避免不同测试实例复用同一个 snapshot 文件。
std::string unique_snapshot_path(const char* prefix) {
    return std::string("/tmp/") + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(acct_service::now_ns()));
}

// 创建单标的 snapshot writer，上层测试只关心发布与回收，不暴露裸句柄。
using snapshot_writer_ptr = std::unique_ptr<void, void (*)(void*)>;

// 初始化一个只有单标的 snapshot 文件，供读端测试复用。
snapshot_writer_ptr make_writer(const std::string& snapshot_path) {
    snapshot_writer_ptr writer(snapshot_shm_writer_new(), snapshot_shm_writer_delete);
    assert(writer != nullptr);

    snapshot_shm::SnapshotSymbolDef symbol{};
    std::strncpy(symbol.symbol, "XSHE_000001", snapshot_shm::kSnapshotSymbolBytes - 1);
    assert(snapshot_shm_writer_init(writer.get(), snapshot_path.c_str(), 20260225, &symbol, 1) == 1);
    return writer;
}

// 构造最小盘口快照，保证读端能断言价量与 prediction 同时可见。
snapshot_shm::LobSnapshot make_snapshot() {
    snapshot_shm::LobSnapshot snapshot{};
    snapshot.time_of_day = 93'100'000;
    snapshot.trade_volume = 1000;
    snapshot.trade_amount = 1000000;
    snapshot.high = 1010;
    snapshot.low = 990;
    snapshot.total_bid_levels = 1;
    snapshot.total_ask_levels = 1;
    snapshot.bids[0].price = 1000;
    snapshot.bids[0].volume = 200;
    snapshot.asks[0].price = 1001;
    snapshot.asks[0].volume = 180;
    return snapshot;
}

}  // namespace

TEST(reads_fresh_prediction) {
    ScopedEnvOverride env_override("SHM_USE_FILE", "1");
    const std::string snapshot_path = unique_snapshot_path("acct_market_data_fresh");
    snapshot_writer_ptr writer = make_writer(snapshot_path);

    const snapshot_shm::LobSnapshot snapshot = make_snapshot();
    assert(snapshot_shm_writer_publish_with_state(
               writer.get(), 0, &snapshot, 1.25F,
               snapshot_shm::kSnapshotSlotFlagHasSignal | snapshot_shm::kSnapshotSlotFlagSignalFresh,
               static_cast<uint8_t>(snapshot_shm::SnapshotPredictionState::kFresh)) == 1);

    acct_service::MarketDataConfig config{};
    config.enabled = true;
    config.snapshot_shm_name = snapshot_path;

    acct_service::MarketDataService service(config);
    assert(service.initialize());

    acct_service::MarketDataView view{};
    assert(service.read("XSHE_000001", view));
    assert(view.symbol == "XSHE_000001");
    assert(view.snapshot.bids[0].price == 1000);
    assert(view.prediction.has_prediction());
    assert(view.prediction.has_fresh_prediction());
    assert(view.prediction.signal == 1.25F);

    service.close();
    assert(snapshot_shm_writer_unlink(snapshot_path.c_str()) == 1);
}

TEST(reads_carried_prediction_without_fresh_flag) {
    ScopedEnvOverride env_override("SHM_USE_FILE", "1");
    const std::string snapshot_path = unique_snapshot_path("acct_market_data_carried");
    snapshot_writer_ptr writer = make_writer(snapshot_path);

    const snapshot_shm::LobSnapshot snapshot = make_snapshot();
    assert(snapshot_shm_writer_publish_with_state(
               writer.get(), 0, &snapshot, 0.5F, snapshot_shm::kSnapshotSlotFlagHasSignal,
               static_cast<uint8_t>(snapshot_shm::SnapshotPredictionState::kCarried)) == 1);

    acct_service::MarketDataConfig config{};
    config.enabled = true;
    config.snapshot_shm_name = snapshot_path;

    acct_service::MarketDataService service(config);
    assert(service.initialize());

    acct_service::MarketDataView view{};
    assert(service.read("XSHE_000001", view));
    assert(view.prediction.has_prediction());
    assert(!view.prediction.has_fresh_prediction());
    assert(view.prediction.signal == 0.5F);

    service.close();
    assert(snapshot_shm_writer_unlink(snapshot_path.c_str()) == 1);
}

TEST(lists_symbols_from_snapshot_source) {
    ScopedEnvOverride env_override("SHM_USE_FILE", "1");
    const std::string snapshot_path = unique_snapshot_path("acct_market_data_symbols");
    snapshot_writer_ptr writer = make_writer(snapshot_path);

    acct_service::MarketDataConfig config{};
    config.enabled = true;
    config.snapshot_shm_name = snapshot_path;

    acct_service::MarketDataService service(config);
    assert(service.initialize());

    const std::vector<std::string> symbols = service.list_symbols();
    assert(symbols.size() == 1);
    assert(symbols.front() == "XSHE_000001");

    service.close();
    assert(snapshot_shm_writer_unlink(snapshot_path.c_str()) == 1);
}

int main() {
    printf("=== Market Data Service Test Suite ===\n\n");

    RUN_TEST(reads_fresh_prediction);
    RUN_TEST(reads_carried_prediction_without_fresh_flag);
    RUN_TEST(lists_symbols_from_snapshot_source);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
