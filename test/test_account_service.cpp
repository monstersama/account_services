#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include "third_party/sqlite3/sqlite3_shim.hpp"
#include <string>
#include <thread>

#include <unistd.h>

#include "core/account_service.hpp"
#include "order/order_request.hpp"
#include "shm/orders_shm.hpp"
#include "shm/shm_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

using namespace acct_service;
using sqlite_db_ptr = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

// 定位并创建仓库 data 目录，统一存放测试 CSV/SQLite/配置文件。
const std::string& test_data_dir() {
    namespace fs = std::filesystem;
    static const std::string kDataDir = []() {
        fs::path probe = fs::current_path();
        for (int depth = 0; depth < 8; ++depth) {
            if (fs::exists(probe / "CMakeLists.txt") && fs::exists(probe / "src") && fs::exists(probe / "test")) {
                const fs::path data_path = probe / "data";
                std::error_code ec;
                fs::create_directories(data_path, ec);
                return data_path.string();
            }
            if (!probe.has_parent_path()) {
                break;
            }
            const fs::path parent = probe.parent_path();
            if (parent == probe) {
                break;
            }
            probe = parent;
        }

        const fs::path fallback = fs::current_path() / "data";
        std::error_code ec;
        fs::create_directories(fallback, ec);
        return fallback.string();
    }();
    return kDataDir;
}

std::string unique_shm_name(const char* prefix) {
    return std::string("/") + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(now_ns()));
}

std::string unique_config_path(const char* prefix) {
    return test_data_dir() + "/" + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(now_ns())) + ".yaml";
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

bool write_config_file(const std::string& path, const config& cfg) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }

    out << "account_id: " << cfg.account_id << "\n";
    out << "trading_day: \"" << cfg.trading_day << "\"\n";
    out << "shm:\n";
    out << "  upstream_shm_name: \"" << cfg.shm.upstream_shm_name << "\"\n";
    out << "  downstream_shm_name: \"" << cfg.shm.downstream_shm_name << "\"\n";
    out << "  trades_shm_name: \"" << cfg.shm.trades_shm_name << "\"\n";
    out << "  orders_shm_name: \"" << cfg.shm.orders_shm_name << "\"\n";
    out << "  positions_shm_name: \"" << cfg.shm.positions_shm_name << "\"\n";
    out << "  create_if_not_exist: " << (cfg.shm.create_if_not_exist ? "true" : "false") << "\n";
    out << "event_loop:\n";
    out << "  busy_polling: " << (cfg.event_loop.busy_polling ? "true" : "false") << "\n";
    out << "  poll_batch_size: " << cfg.event_loop.poll_batch_size << "\n";
    out << "  idle_sleep_us: " << cfg.event_loop.idle_sleep_us << "\n";
    out << "  stats_interval_ms: " << cfg.event_loop.stats_interval_ms << "\n";
    out << "  pin_cpu: " << (cfg.event_loop.pin_cpu ? "true" : "false") << "\n";
    out << "  cpu_core: " << cfg.event_loop.cpu_core << "\n";
    out << "risk:\n";
    out << "  max_order_value: " << cfg.risk.max_order_value << "\n";
    out << "  max_order_volume: " << cfg.risk.max_order_volume << "\n";
    out << "  max_daily_turnover: " << cfg.risk.max_daily_turnover << "\n";
    out << "  max_orders_per_second: " << cfg.risk.max_orders_per_second << "\n";
    out << "  enable_price_limit_check: " << (cfg.risk.enable_price_limit_check ? "true" : "false") << "\n";
    out << "  enable_duplicate_check: " << (cfg.risk.enable_duplicate_check ? "true" : "false") << "\n";
    out << "  enable_fund_check: " << (cfg.risk.enable_fund_check ? "true" : "false") << "\n";
    out << "  enable_position_check: " << (cfg.risk.enable_position_check ? "true" : "false") << "\n";
    out << "  duplicate_window_ns: " << cfg.risk.duplicate_window_ns << "\n";
    out << "split:\n";
    out << "  strategy: \"" << split_strategy_to_string(cfg.split.strategy) << "\"\n";
    out << "  max_child_volume: " << cfg.split.max_child_volume << "\n";
    out << "  min_child_volume: " << cfg.split.min_child_volume << "\n";
    out << "  max_child_count: " << cfg.split.max_child_count << "\n";
    out << "  interval_ms: " << cfg.split.interval_ms << "\n";
    out << "  randomize_factor: " << cfg.split.randomize_factor << "\n";
    out << "log:\n";
    out << "  log_dir: \"" << cfg.log.log_dir << "\"\n";
    out << "  log_level: \"" << cfg.log.log_level << "\"\n";
    out << "  async_logging: " << (cfg.log.async_logging ? "true" : "false") << "\n";
    out << "  async_queue_size: " << cfg.log.async_queue_size << "\n";
    out << "db:\n";
    out << "  db_path: \"" << cfg.db.db_path << "\"\n";
    out << "  enable_persistence: " << (cfg.db.enable_persistence ? "true" : "false") << "\n";
    out << "  sync_interval_ms: " << cfg.db.sync_interval_ms << "\n";

    return true;
}

bool wait_until(const std::function<bool()>& predicate, int timeout_ms = 1000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::string unique_db_path(const char* prefix) {
    return test_data_dir() + "/" + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(now_ns())) + ".sqlite";
}

// 打开 sqlite 可读写连接，失败时返回 false。
bool open_sqlite_rw(const std::string& path, sqlite_db_ptr& db) {
    sqlite3* raw_db = nullptr;
    const int rc = sqlite3_open_v2(path.c_str(), &raw_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK || raw_db == nullptr) {
        if (raw_db != nullptr) {
            sqlite3_close(raw_db);
        }
        return false;
    }
    db.reset(raw_db);
    return true;
}

// 执行单条 SQL 语句，任何错误都视为失败。
bool exec_sql(sqlite3* db, std::string_view sql) {
    if (!db) {
        return false;
    }

    char* err_msg = nullptr;
    const int rc = sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &err_msg);
    if (err_msg != nullptr) {
        sqlite3_free(err_msg);
    }
    return rc == SQLITE_OK;
}

// 初始化 position_loader 所需 sqlite schema。
bool init_position_loader_schema(sqlite3* db) {
    return exec_sql(db,
               "CREATE TABLE account_info ("
               "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
               "account_id INTEGER NOT NULL UNIQUE,"
               "total_assets INTEGER NOT NULL DEFAULT 0,"
               "available_cash INTEGER NOT NULL DEFAULT 0,"
               "frozen_cash INTEGER NOT NULL DEFAULT 0,"
               "position_value INTEGER NOT NULL DEFAULT 0,"
               "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
               "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
               ");") &&
           exec_sql(db,
               "CREATE TABLE positions ("
               "ID INTEGER PRIMARY KEY AUTOINCREMENT,"
               "security_id TEXT NOT NULL,"
               "internal_security_id TEXT NOT NULL,"
               "volume_available_t0 INTEGER NOT NULL DEFAULT 0,"
               "volume_available_t1 INTEGER NOT NULL DEFAULT 0,"
               "volume_buy INTEGER NOT NULL DEFAULT 0,"
               "dvalue_buy INTEGER NOT NULL DEFAULT 0,"
               "volume_buy_traded INTEGER NOT NULL DEFAULT 0,"
               "dvalue_buy_traded INTEGER NOT NULL DEFAULT 0,"
               "volume_sell INTEGER NOT NULL DEFAULT 0,"
               "dvalue_sell INTEGER NOT NULL DEFAULT 0,"
               "volume_sell_traded INTEGER NOT NULL DEFAULT 0,"
               "dvalue_sell_traded INTEGER NOT NULL DEFAULT 0,"
               "count_order INTEGER NOT NULL DEFAULT 0,"
               "updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
               "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
               ");");
}

}  // namespace

TEST(initialize_and_run_processes_orders) {
    config cfg;
    cfg.account_id = 101;
    cfg.shm.upstream_shm_name = unique_shm_name("acct_upstream");
    cfg.shm.downstream_shm_name = unique_shm_name("acct_downstream");
    cfg.shm.trades_shm_name = unique_shm_name("acct_trades");
    cfg.shm.orders_shm_name = unique_shm_name("acct_orders");
    cfg.shm.positions_shm_name = unique_shm_name("acct_positions");
    cfg.shm.create_if_not_exist = true;
    cfg.trading_day = "20260225";

    cfg.event_loop.busy_polling = false;
    cfg.event_loop.idle_sleep_us = 50;
    cfg.event_loop.poll_batch_size = 32;
    cfg.event_loop.stats_interval_ms = 0;

    cfg.risk.enable_position_check = false;
    cfg.risk.enable_duplicate_check = false;
    cfg.risk.enable_price_limit_check = false;

    cfg.split.strategy = split_strategy_t::None;

    cfg.db.enable_persistence = false;
    cfg.db.db_path.clear();

    const std::string config_path = unique_config_path("acct_service_cfg");
    assert(write_config_file(config_path, cfg));

    account_service service;
    assert(service.initialize(config_path));
    assert(service.state() == service_state_t::Ready);

    SHMManager upstream_manager;
    SHMManager downstream_manager;
    SHMManager trades_manager;
    SHMManager orders_manager;

    upstream_shm_layout* upstream =
        upstream_manager.open_upstream(cfg.shm.upstream_shm_name, shm_mode::Open, cfg.account_id);
    downstream_shm_layout* downstream =
        downstream_manager.open_downstream(cfg.shm.downstream_shm_name, shm_mode::Open, cfg.account_id);
    trades_shm_layout* trades = trades_manager.open_trades(cfg.shm.trades_shm_name, shm_mode::Open, cfg.account_id);
    const std::string dated_orders_name = make_orders_shm_name(cfg.shm.orders_shm_name, cfg.trading_day);
    orders_shm_layout* orders = orders_manager.open_orders(dated_orders_name, shm_mode::Open, cfg.account_id);

    assert(upstream != nullptr);
    assert(downstream != nullptr);
    assert(trades != nullptr);
    assert(orders != nullptr);

    int run_rc = -1;
    std::thread worker([&service, &run_rc]() { run_rc = service.run(); });

    order_request req;
    req.init_new("000001", internal_security_id_t("SZ.000001"), static_cast<internal_order_id_t>(5001),
        trade_side_t::Buy, market_t::SZ, static_cast<volume_t>(100), static_cast<dprice_t>(1000), 93000000);
    req.order_status.store(order_status_t::StrategySubmitted, std::memory_order_relaxed);

    order_index_t upstream_index = kInvalidOrderIndex;
    assert(orders_shm_append(
        orders, req, order_slot_stage_t::UpstreamQueued, order_slot_source_t::Strategy, now_ns(), upstream_index));
    assert(upstream->strategy_order_queue.try_push(upstream_index));

    assert(wait_until([downstream]() { return downstream->order_queue.size() > 0; }));

    order_index_t downstream_index = kInvalidOrderIndex;
    assert(downstream->order_queue.try_pop(downstream_index));
    order_slot_snapshot sent_snapshot;
    assert(orders_shm_read_snapshot(orders, downstream_index, sent_snapshot));
    assert(sent_snapshot.request.order_type == order_type_t::New);
    assert(sent_snapshot.request.security_id.view() == "000001");

    trade_response rsp{};
    rsp.internal_order_id = static_cast<internal_order_id_t>(5001);
    rsp.internal_security_id = internal_security_id_t("SZ.000001");
    rsp.trade_side = trade_side_t::Buy;
    rsp.new_status = order_status_t::MarketAccepted;
    rsp.volume_traded = static_cast<volume_t>(50);
    rsp.dprice_traded = static_cast<dprice_t>(1000);
    rsp.dvalue_traded = static_cast<dvalue_t>(50000);
    rsp.dfee = static_cast<dvalue_t>(10);
    rsp.md_time_traded = 93100000;
    rsp.recv_time_ns = now_ns();

    assert(trades->response_queue.try_push(rsp));

    assert(wait_until([&service]() {
        const order_entry* order = service.orders().find_order(static_cast<internal_order_id_t>(5001));
        return order != nullptr && order->request.volume_traded == static_cast<volume_t>(50);
    }));

    const order_entry* order = service.orders().find_order(static_cast<internal_order_id_t>(5001));
    assert(order != nullptr);
    assert(order->request.volume_traded == static_cast<volume_t>(50));
    assert(order->request.order_status.load(std::memory_order_acquire) == order_status_t::MarketAccepted);

    service.stop();
    worker.join();

    assert(run_rc == 0);
    assert(service.state() == service_state_t::Stopped);

    upstream_manager.close();
    downstream_manager.close();
    trades_manager.close();
    orders_manager.close();

    (void)SHMManager::unlink(cfg.shm.upstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.downstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.trades_shm_name);
    (void)SHMManager::unlink(dated_orders_name);
    (void)SHMManager::unlink(cfg.shm.positions_shm_name);
    std::remove(config_path.c_str());
}

TEST(initialize_rejects_invalid_config) {
    config cfg;
    cfg.account_id = 0;

    const std::string config_path = unique_config_path("acct_service_cfg_invalid");
    assert(write_config_file(config_path, cfg));

    account_service service;
    assert(!service.initialize(config_path));
    std::remove(config_path.c_str());
}

TEST(position_loader_file_mode_only_on_fresh_shm) {
    using namespace acct_service;

    config cfg;
    cfg.account_id = 102;
    cfg.shm.upstream_shm_name = unique_shm_name("acct_upstream_pos_boot");
    cfg.shm.downstream_shm_name = unique_shm_name("acct_downstream_pos_boot");
    cfg.shm.trades_shm_name = unique_shm_name("acct_trades_pos_boot");
    cfg.shm.orders_shm_name = unique_shm_name("acct_orders_pos_boot");
    cfg.shm.positions_shm_name = unique_shm_name("acct_positions_pos_boot");
    cfg.shm.create_if_not_exist = true;
    cfg.trading_day = "20260225";

    cfg.event_loop.busy_polling = false;
    cfg.event_loop.idle_sleep_us = 50;
    cfg.event_loop.poll_batch_size = 32;
    cfg.event_loop.stats_interval_ms = 0;

    cfg.risk.enable_position_check = false;
    cfg.risk.enable_duplicate_check = false;
    cfg.risk.enable_price_limit_check = false;

    cfg.split.strategy = split_strategy_t::None;

    cfg.db.enable_persistence = false;
    cfg.db.db_path.clear();

    const std::string config_path = unique_config_path("acct_service_cfg_pos_boot");
    assert(write_config_file(config_path, cfg));
    const std::string bootstrap_file = config_path + ".positions.csv";

    {
        std::ofstream out(bootstrap_file);
        assert(out.is_open());
        out << "record_type,internal_security_id,name,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
               "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order\n";
        out << "position,SZ.000001,PingAn,123,0,0,0,0,0,0,0,0,0,0\n";
    }

    {
        account_service first_start;
        assert(first_start.initialize(config_path));
        const position* loaded = first_start.positions().get_position(internal_security_id_t("SZ.000001"));
        assert(loaded != nullptr);
        assert(loaded->volume_available_t0 == 123);
        assert(first_start.positions().position_count() == 1);
    }

    {
        std::ofstream out(bootstrap_file, std::ios::trunc);
        assert(out.is_open());
        out << "record_type,internal_security_id,name,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
               "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order\n";
        out << "position,SZ.000002,Vanke,999,0,0,0,0,0,0,0,0,0,0\n";
    }

    {
        account_service second_start;
        assert(second_start.initialize(config_path));
        const position* old_pos = second_start.positions().get_position(internal_security_id_t("SZ.000001"));
        const position* new_pos = second_start.positions().get_position(internal_security_id_t("SZ.000002"));
        assert(old_pos != nullptr);
        assert(old_pos->volume_available_t0 == 123);
        assert(new_pos == nullptr);
        assert(second_start.positions().position_count() == 1);
    }

    const std::string dated_orders_name = make_orders_shm_name(cfg.shm.orders_shm_name, cfg.trading_day);
    (void)SHMManager::unlink(cfg.shm.upstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.downstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.trades_shm_name);
    (void)SHMManager::unlink(dated_orders_name);
    (void)SHMManager::unlink(cfg.shm.positions_shm_name);
    std::remove(config_path.c_str());
    std::remove(bootstrap_file.c_str());
}

TEST(position_loader_db_mode_only_on_fresh_shm) {
    using namespace acct_service;

    config cfg;
    cfg.account_id = 103;
    cfg.shm.upstream_shm_name = unique_shm_name("acct_upstream_pos_boot_db");
    cfg.shm.downstream_shm_name = unique_shm_name("acct_downstream_pos_boot_db");
    cfg.shm.trades_shm_name = unique_shm_name("acct_trades_pos_boot_db");
    cfg.shm.orders_shm_name = unique_shm_name("acct_orders_pos_boot_db");
    cfg.shm.positions_shm_name = unique_shm_name("acct_positions_pos_boot_db");
    cfg.shm.create_if_not_exist = true;
    cfg.trading_day = "20260225";

    cfg.event_loop.busy_polling = false;
    cfg.event_loop.idle_sleep_us = 50;
    cfg.event_loop.poll_batch_size = 32;
    cfg.event_loop.stats_interval_ms = 0;

    cfg.risk.enable_position_check = false;
    cfg.risk.enable_duplicate_check = false;
    cfg.risk.enable_price_limit_check = false;

    cfg.split.strategy = split_strategy_t::None;

    cfg.db.enable_persistence = true;
    cfg.db.db_path = unique_db_path("acct_service_pos_boot");

    const std::string config_path = unique_config_path("acct_service_cfg_pos_boot_db");
    assert(write_config_file(config_path, cfg));

    sqlite_db_ptr db(nullptr, sqlite3_close);
    assert(open_sqlite_rw(cfg.db.db_path, db));
    assert(init_position_loader_schema(db.get()));
    assert(exec_sql(db.get(),
        "INSERT INTO account_info(account_id,total_assets,available_cash,frozen_cash,position_value) "
        "VALUES (103,300000000,280000000,10000000,10000000);"));
    assert(exec_sql(db.get(),
        "INSERT INTO positions("
        "security_id,internal_security_id,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
        "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order"
        ") VALUES ('000001','SZ.000001',456,0,0,0,0,0,0,0,0,0,0);"));
    db.reset();

    {
        account_service first_start;
        assert(first_start.initialize(config_path));
        const position* loaded = first_start.positions().get_position(internal_security_id_t("SZ.000001"));
        assert(loaded != nullptr);
        assert(loaded->volume_available_t0 == 456);
        assert(first_start.positions().position_count() == 1);
    }

    assert(open_sqlite_rw(cfg.db.db_path, db));
    assert(exec_sql(db.get(), "DELETE FROM positions;"));
    assert(exec_sql(db.get(),
        "INSERT INTO positions("
        "security_id,internal_security_id,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
        "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order"
        ") VALUES ('000002','SZ.000002',999,0,0,0,0,0,0,0,0,0,0);"));
    db.reset();

    {
        account_service second_start;
        assert(second_start.initialize(config_path));
        const position* old_pos = second_start.positions().get_position(internal_security_id_t("SZ.000001"));
        const position* new_pos = second_start.positions().get_position(internal_security_id_t("SZ.000002"));
        assert(old_pos != nullptr);
        assert(old_pos->volume_available_t0 == 456);
        assert(new_pos == nullptr);
        assert(second_start.positions().position_count() == 1);
    }

    const std::string dated_orders_name = make_orders_shm_name(cfg.shm.orders_shm_name, cfg.trading_day);
    (void)SHMManager::unlink(cfg.shm.upstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.downstream_shm_name);
    (void)SHMManager::unlink(cfg.shm.trades_shm_name);
    (void)SHMManager::unlink(dated_orders_name);
    (void)SHMManager::unlink(cfg.shm.positions_shm_name);
    std::remove(config_path.c_str());
    std::remove(cfg.db.db_path.c_str());
}

int main() {
    printf("=== Account Service Test Suite ===\n\n");

    RUN_TEST(initialize_and_run_processes_orders);
    RUN_TEST(initialize_rejects_invalid_config);
    RUN_TEST(position_loader_file_mode_only_on_fresh_shm);
    RUN_TEST(position_loader_db_mode_only_on_fresh_shm);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
