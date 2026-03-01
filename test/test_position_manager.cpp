#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include "third_party/sqlite3/sqlite3_shim.hpp"
#include <string>
#include <string_view>

#include <unistd.h>

#include "common/constants.hpp"
#include "portfolio/position_manager.hpp"

#define TEST(name) static void test_##name()
#define RUN_TEST(name)                   \
    do {                                 \
        printf("Running %s... ", #name); \
        test_##name();                   \
        printf("PASSED\n");              \
    } while (0)

namespace {

constexpr acct_service::dvalue_t kExpectedInitialFund = 100000000;
using sqlite_db_ptr = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;

// 定位并创建仓库 data 目录，统一存放测试 CSV/SQLite 文件。
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

void setup_header(acct_service::positions_shm_layout& shm, uint32_t init_state) {
    shm.header.magic = acct_service::positions_header::kMagic;
    shm.header.version = acct_service::positions_header::kVersion;
    shm.header.header_size = static_cast<uint32_t>(sizeof(acct_service::positions_header));
    shm.header.total_size = static_cast<uint32_t>(sizeof(acct_service::positions_shm_layout));
    shm.header.capacity = static_cast<uint32_t>(acct_service::kMaxPositions);
    shm.header.init_state = init_state;
    shm.header.create_time = 0;
    shm.header.last_update = 0;
    shm.header.id.store(1, std::memory_order_relaxed);
    shm.position_count.store(0, std::memory_order_relaxed);
}

std::unique_ptr<acct_service::positions_shm_layout> make_shm(uint32_t init_state) {
    auto shm = std::make_unique<acct_service::positions_shm_layout>();
    setup_header(*shm, init_state);
    return shm;
}

void assert_fund_row_fields(const position& fund_row, const fund_info& fund) {
    assert(fund_total_asset_field(fund_row) == fund.total_asset);
    assert(fund_available_field(fund_row) == fund.available);
    assert(fund_frozen_field(fund_row) == fund.frozen);
    assert(fund_market_value_field(fund_row) == fund.market_value);
}

// 生成临时持仓快照文件路径，避免并发测试互相覆盖。
std::string unique_seed_path(const char* prefix) {
    return test_data_dir() + "/" + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(acct_service::now_ns()));
}

// 写入最小可解析的持仓 CSV（header + single row）。
bool write_seed_csv(const std::string& path, std::string_view row) {
    std::ofstream out(path);
    if (!out.is_open()) {
        return false;
    }
    out << "record_type,internal_security_id,name,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
           "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order\n";
    out << row << '\n';
    return true;
}

// 生成临时 sqlite 文件路径，避免并发测试互相覆盖。
std::string unique_db_path(const char* prefix) {
    return test_data_dir() + "/" + prefix + "_" + std::to_string(static_cast<unsigned long long>(getpid())) + "_" +
           std::to_string(static_cast<unsigned long long>(acct_service::now_ns())) + ".sqlite";
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

TEST(initialize_sets_fund_row) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());

    const bool ok = manager.initialize(1);
    assert(ok);
    assert(manager.position_count() == 0);

    const position& fund_row = shm->positions[kFundPositionIndex];
    assert(fund_row.id == std::string_view(kFundPositionId));
    assert(fund_row.name == std::string_view(kFundPositionId));

    const fund_info fund = manager.get_fund_info();
    assert(fund.total_asset == kExpectedInitialFund);
    assert(fund.available == kExpectedInitialFund);
    assert(fund.frozen == 0);
    assert(fund.market_value == 0);
    assert_fund_row_fields(fund_row, fund);
}

TEST(add_security_uses_internal_key_and_excludes_fund_row) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    const internal_security_id_t first = manager.add_security("000001", "PingAn", market_t::SZ);
    const internal_security_id_t second = manager.add_security("600000", "PuFa", market_t::SH);
    assert(first == std::string_view("SZ.000001"));
    assert(second == std::string_view("SH.600000"));
    assert(manager.position_count() == 2);

    assert(shm->positions[1].id == std::string_view("SZ.000001"));
    assert(shm->positions[2].id == std::string_view("SH.600000"));
    assert(manager.find_security_id("SZ.000001").value_or(internal_security_id_t()) == std::string_view("SZ.000001"));
    assert(manager.find_security_id("SH.600000").value_or(internal_security_id_t()) == std::string_view("SH.600000"));

    const auto all_positions = manager.get_all_positions();
    assert(all_positions.size() == 2);
    assert(all_positions[0]->id == std::string_view("SZ.000001"));
    assert(all_positions[1]->id == std::string_view("SH.600000"));
}

TEST(fund_ops_write_into_fund_row) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    assert(manager.freeze_fund(100, 1));
    assert(manager.unfreeze_fund(40, 1));
    assert(manager.deduct_fund(50, 10, 1));
    assert(manager.add_fund(20, 1));
    assert(!manager.deduct_fund(1, 0, 2));

    const fund_info fund = manager.get_fund_info();
    assert(fund.total_asset == 100000010);
    assert(fund.available == 99999960);
    assert(fund.frozen == 0);
    assert(fund.market_value == 50);

    const position& fund_row = shm->positions[kFundPositionIndex];
    assert_fund_row_fields(fund_row, fund);
}

TEST(add_position_requires_registered_security) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    assert(!manager.add_position(internal_security_id_t("SZ.000001"), 100, 123, 1));

    const internal_security_id_t sec_id = manager.add_security("000001", "PingAn", market_t::SZ);
    assert(sec_id == std::string_view("SZ.000001"));
    assert(manager.add_position(sec_id, 100, 123, 2));

    const position* pos = manager.get_position(sec_id);
    assert(pos != nullptr);
    assert(pos->volume_buy == 100);
    assert(pos->dvalue_buy == 12300);
    assert(pos->volume_available_t1 == 100);
}

// 验证当日可卖、冻结与兼容扣减路径均只使用 t0 数量，不透支 t1。
TEST(sellable_volume_uses_t0_only) {
    using namespace acct_service;

    auto shm = make_shm(0);
    position_manager manager(shm.get());
    assert(manager.initialize(1));

    const internal_security_id_t sec_id = manager.add_security("000001", "PingAn", market_t::SZ);
    assert(sec_id == std::string_view("SZ.000001"));

    position* pos = manager.get_position_mut(sec_id);
    assert(pos != nullptr);

    {
        position_lock guard(*pos);
        pos->volume_available_t0 = 100;
        pos->volume_available_t1 = 200;
        pos->volume_sell = 0;
        pos->volume_sell_traded = 0;
        pos->dvalue_sell_traded = 0;
    }

    // 当日可卖只返回 t0。
    assert(manager.get_sellable_volume(sec_id) == 100);
    assert(!manager.freeze_position(sec_id, 150, 1));
    assert(manager.freeze_position(sec_id, 80, 2));

    {
        position_lock guard(*pos);
        assert(pos->volume_available_t0 == 20);
        assert(pos->volume_available_t1 == 200);
        assert(pos->volume_sell == 80);
    }

    assert(manager.unfreeze_position(sec_id, 80, 3));
    {
        position_lock guard(*pos);
        assert(pos->volume_available_t0 == 100);
        assert(pos->volume_available_t1 == 200);
        assert(pos->volume_sell == 0);
        pos->volume_available_t0 = 50;
        pos->volume_available_t1 = 300;
    }

    // 未冻结直扣的兼容路径也必须只扣减 t0。
    assert(!manager.deduct_position(sec_id, 60, 6000, 4));
    assert(manager.deduct_position(sec_id, 40, 4000, 5));

    {
        position_lock guard(*pos);
        assert(pos->volume_available_t0 == 10);
        assert(pos->volume_available_t1 == 300);
        assert(pos->volume_sell == 0);
        assert(pos->volume_sell_traded == 40);
        assert(pos->dvalue_sell_traded == 4000);
    }
}

TEST(initialize_rebuilds_code_map_from_existing_rows) {
    using namespace acct_service;

    auto shm = make_shm(1);
    shm->position_count.store(2, std::memory_order_relaxed);
    shm->positions[1].id.assign("SZ.000001");
    shm->positions[1].name.assign("PingAn");
    shm->positions[2].id.assign("SH.600000");
    shm->positions[2].name.assign("PuFa");

    position_manager manager(shm.get());
    assert(manager.initialize(1));

    assert(shm->positions[kFundPositionIndex].id == std::string_view(kFundPositionId));
    assert(manager.position_count() == 2);
    assert(manager.find_security_id("SZ.000001").value_or(internal_security_id_t()) == std::string_view("SZ.000001"));
    assert(manager.find_security_id("SH.600000").value_or(internal_security_id_t()) == std::string_view("SH.600000"));
}

TEST(initialize_uses_loader_only_for_uninitialized_shm) {
    using namespace acct_service;

    const std::string seed_base_path = unique_seed_path("acct_pos_seed");
    const std::string seed_csv_path = seed_base_path + ".positions.csv";
    assert(write_seed_csv(seed_csv_path, "position,SZ.000001,PingAn,321,0,0,0,0,0,0,0,0,0,0"));

    auto fresh_shm = make_shm(0);
    position_manager fresh_manager(fresh_shm.get(), seed_base_path, "", false);
    assert(fresh_manager.initialize(1));
    assert(fresh_manager.position_count() == 1);
    assert(fresh_manager.get_sellable_volume(internal_security_id_t("SZ.000001")) == 321);

    auto existing_shm = make_shm(1);
    existing_shm->position_count.store(1, std::memory_order_relaxed);
    existing_shm->positions[1].id.assign("SZ.000002");
    existing_shm->positions[1].name.assign("Vanke");
    existing_shm->positions[1].volume_available_t0 = 222;

    position_manager existing_manager(existing_shm.get(), seed_base_path, "", false);
    assert(existing_manager.initialize(1));
    assert(existing_manager.position_count() == 1);
    assert(existing_manager.get_sellable_volume(internal_security_id_t("SZ.000002")) == 222);
    assert(existing_manager.get_position(internal_security_id_t("SZ.000001")) == nullptr);

    std::remove(seed_csv_path.c_str());
}

TEST(initialize_fails_when_loader_fails_on_uninitialized_shm) {
    using namespace acct_service;

    const std::string seed_base_path = unique_seed_path("acct_pos_seed_invalid");
    const std::string seed_csv_path = seed_base_path + ".positions.csv";
    assert(write_seed_csv(seed_csv_path, "bad_record,SZ.000001,PingAn,1,0,0,0,0,0,0,0,0,0,0"));

    auto shm = make_shm(0);
    position_manager manager(shm.get(), seed_base_path, "", false);
    assert(!manager.initialize(1));
    assert(shm->header.init_state == 0);

    std::remove(seed_csv_path.c_str());
}

TEST(initialize_loads_from_sqlite_when_db_enabled) {
    using namespace acct_service;

    const std::string db_path = unique_db_path("acct_pos_seed_db");
    sqlite_db_ptr db(nullptr, sqlite3_close);
    assert(open_sqlite_rw(db_path, db));
    assert(init_position_loader_schema(db.get()));
    assert(exec_sql(db.get(),
        "INSERT INTO account_info(account_id,total_assets,available_cash,frozen_cash,position_value) "
        "VALUES (1,200000000,150000000,10000000,40000000);"));
    assert(exec_sql(db.get(),
        "INSERT INTO positions("
        "security_id,internal_security_id,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
        "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order"
        ") VALUES ('000001','SZ.000001',321,7,11,1100,9,900,2,200,1,100,3);"));
    assert(exec_sql(db.get(),
        "INSERT INTO positions("
        "security_id,internal_security_id,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
        "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order"
        ") VALUES ('000001','SZ.000001',999,8,12,1200,10,1000,3,300,2,200,4);"));
    assert(exec_sql(db.get(),
        "INSERT INTO positions("
        "security_id,internal_security_id,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
        "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order"
        ") VALUES ('600000','SH.600000',55,6,13,1300,11,1100,4,400,3,300,5);"));
    db.reset();

    auto shm = make_shm(0);
    position_manager manager(shm.get(), "", db_path, true);
    assert(manager.initialize(1));
    assert(manager.position_count() == 2);

    const fund_info fund = manager.get_fund_info();
    assert(fund.total_asset == 200000000);
    assert(fund.available == 150000000);
    assert(fund.frozen == 10000000);
    assert(fund.market_value == 40000000);

    const position* sz_pos = manager.get_position(internal_security_id_t("SZ.000001"));
    assert(sz_pos != nullptr);
    assert(sz_pos->volume_available_t0 == 999);
    assert(sz_pos->volume_available_t1 == 8);
    assert(sz_pos->volume_buy == 12);
    assert(sz_pos->dvalue_buy == 1200);
    assert(sz_pos->count_order == 4);

    const position* sh_pos = manager.get_position(internal_security_id_t("SH.600000"));
    assert(sh_pos != nullptr);
    assert(sh_pos->volume_available_t0 == 55);
    assert(sh_pos->volume_available_t1 == 6);
    assert(sh_pos->volume_buy == 13);
    assert(sh_pos->dvalue_buy == 1300);
    assert(sh_pos->count_order == 5);

    std::remove(db_path.c_str());
}

TEST(initialize_fails_when_account_row_missing_in_db) {
    using namespace acct_service;

    const std::string db_path = unique_db_path("acct_pos_seed_db_no_account");
    sqlite_db_ptr db(nullptr, sqlite3_close);
    assert(open_sqlite_rw(db_path, db));
    assert(init_position_loader_schema(db.get()));
    assert(exec_sql(db.get(),
        "INSERT INTO account_info(account_id,total_assets,available_cash,frozen_cash,position_value) "
        "VALUES (2,100,90,10,0);"));
    db.reset();

    auto shm = make_shm(0);
    position_manager manager(shm.get(), "", db_path, true);
    assert(!manager.initialize(1));
    assert(shm->header.init_state == 0);

    std::remove(db_path.c_str());
}

TEST(initialize_fails_when_internal_security_id_invalid_in_db) {
    using namespace acct_service;

    const std::string db_path = unique_db_path("acct_pos_seed_db_bad_internal");
    sqlite_db_ptr db(nullptr, sqlite3_close);
    assert(open_sqlite_rw(db_path, db));
    assert(init_position_loader_schema(db.get()));
    assert(exec_sql(db.get(),
        "INSERT INTO account_info(account_id,total_assets,available_cash,frozen_cash,position_value) "
        "VALUES (1,100,90,10,0);"));
    assert(exec_sql(db.get(),
        "INSERT INTO positions("
        "security_id,internal_security_id,volume_available_t0,volume_available_t1,volume_buy,dvalue_buy,"
        "volume_buy_traded,dvalue_buy_traded,volume_sell,dvalue_sell,volume_sell_traded,dvalue_sell_traded,count_order"
        ") VALUES ('000001','INVALID_ID',1,0,0,0,0,0,0,0,0,0,0);"));
    db.reset();

    auto shm = make_shm(0);
    position_manager manager(shm.get(), "", db_path, true);
    assert(!manager.initialize(1));
    assert(shm->header.init_state == 0);

    std::remove(db_path.c_str());
}

int main() {
    printf("=== Position Manager Test Suite ===\n\n");

    RUN_TEST(initialize_sets_fund_row);
    RUN_TEST(add_security_uses_internal_key_and_excludes_fund_row);
    RUN_TEST(fund_ops_write_into_fund_row);
    RUN_TEST(add_position_requires_registered_security);
    RUN_TEST(sellable_volume_uses_t0_only);
    RUN_TEST(initialize_rebuilds_code_map_from_existing_rows);
    RUN_TEST(initialize_uses_loader_only_for_uninitialized_shm);
    RUN_TEST(initialize_fails_when_loader_fails_on_uninitialized_shm);
    RUN_TEST(initialize_loads_from_sqlite_when_db_enabled);
    RUN_TEST(initialize_fails_when_account_row_missing_in_db);
    RUN_TEST(initialize_fails_when_internal_security_id_invalid_in_db);

    printf("\n=== All tests passed! ===\n");
    return 0;
}
