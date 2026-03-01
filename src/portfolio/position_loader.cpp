#include "portfolio/position_loader.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "common/log.hpp"
#include "portfolio/position_manager.hpp"
#include "third_party/sqlite3/sqlite3_shim.hpp"

namespace acct_service {

namespace {

using sqlite_db_ptr = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using sqlite_stmt_ptr = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

constexpr std::string_view kFundQuerySql =
    "SELECT total_assets, available_cash, frozen_cash, position_value "
    "FROM account_info WHERE account_id = ? LIMIT 1;";
constexpr std::string_view kPositionQuerySql =
    "SELECT security_id, internal_security_id, volume_available_t0, volume_available_t1, volume_buy, dvalue_buy, "
    "volume_buy_traded, dvalue_buy_traded, volume_sell, dvalue_sell, volume_sell_traded, dvalue_sell_traded, "
    "count_order FROM positions ORDER BY ID ASC;";

// DB 行字段映射，保持与 SQL 结果列一一对应。
struct db_position_row {
    std::string security_id;
    std::string internal_security_id;
    uint64_t volume_available_t0{0};
    uint64_t volume_available_t1{0};
    uint64_t volume_buy{0};
    uint64_t dvalue_buy{0};
    uint64_t volume_buy_traded{0};
    uint64_t dvalue_buy_traded{0};
    uint64_t volume_sell{0};
    uint64_t dvalue_sell{0};
    uint64_t volume_sell_traded{0};
    uint64_t dvalue_sell_traded{0};
    uint64_t count_order{0};
};

// 去除首尾空白，统一 CSV 字段解析输入。
std::string trim_copy(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

// 将字段标准化为小写，便于做大小写无关匹配。
std::string to_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

// 按逗号切分行文本，返回未经转义处理的基础列数组。
std::vector<std::string> split_csv_columns(std::string_view line) {
    std::vector<std::string> columns;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t comma = line.find(',', begin);
        const std::size_t end = (comma == std::string_view::npos) ? line.size() : comma;
        columns.push_back(trim_copy(std::string(line.substr(begin, end - begin))));
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return columns;
}

// 解析无符号整数字段，失败时返回 false。
bool parse_u64_field(std::string_view text, uint64_t& out) {
    try {
        out = static_cast<uint64_t>(std::stoull(trim_copy(std::string(text))));
        return true;
    } catch (...) {
        return false;
    }
}

// 解析内部证券 id 前缀的市场段。
bool parse_market_prefix(std::string_view prefix, market_t& market) {
    if (prefix == "SZ") {
        market = market_t::SZ;
        return true;
    }
    if (prefix == "SH") {
        market = market_t::SH;
        return true;
    }
    if (prefix == "BJ") {
        market = market_t::BJ;
        return true;
    }
    if (prefix == "HK") {
        market = market_t::HK;
        return true;
    }
    return false;
}

// 将 "SZ.000001" 解析为 market + code，供 add_security 使用。
bool parse_internal_security_id(std::string_view internal_id, market_t& market, std::string_view& code) {
    const std::size_t dot = internal_id.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= internal_id.size()) {
        return false;
    }
    if (!parse_market_prefix(internal_id.substr(0, dot), market)) {
        return false;
    }
    code = internal_id.substr(dot + 1);
    return !code.empty();
}

// 将一行 position 快照写入持仓管理器（新增证券行并回填字段）。
bool apply_position_seed_row(const std::vector<std::string>& columns, position_manager& manager) {
    if (columns.size() < 14) {
        return false;
    }

    market_t market = market_t::NotSet;
    std::string_view code;
    if (!parse_internal_security_id(columns[1], market, code)) {
        return false;
    }

    uint64_t volume_available_t0 = 0;
    uint64_t volume_available_t1 = 0;
    uint64_t volume_buy = 0;
    uint64_t dvalue_buy = 0;
    uint64_t volume_buy_traded = 0;
    uint64_t dvalue_buy_traded = 0;
    uint64_t volume_sell = 0;
    uint64_t dvalue_sell = 0;
    uint64_t volume_sell_traded = 0;
    uint64_t dvalue_sell_traded = 0;
    uint64_t count_order = 0;
    if (!parse_u64_field(columns[3], volume_available_t0) || !parse_u64_field(columns[4], volume_available_t1) ||
        !parse_u64_field(columns[5], volume_buy) || !parse_u64_field(columns[6], dvalue_buy) ||
        !parse_u64_field(columns[7], volume_buy_traded) || !parse_u64_field(columns[8], dvalue_buy_traded) ||
        !parse_u64_field(columns[9], volume_sell) || !parse_u64_field(columns[10], dvalue_sell) ||
        !parse_u64_field(columns[11], volume_sell_traded) || !parse_u64_field(columns[12], dvalue_sell_traded) ||
        !parse_u64_field(columns[13], count_order)) {
        return false;
    }

    const std::string& internal_id = columns[1];
    const std::string_view position_name =
        columns[2].empty() ? std::string_view(internal_id) : std::string_view(columns[2]);
    const internal_security_id_t added = manager.add_security(code, position_name, market);
    if (added.empty() || added != std::string_view(internal_id)) {
        return false;
    }

    position* pos = manager.get_position_mut(added);
    if (!pos) {
        return false;
    }
    position_lock guard(*pos);
    pos->volume_available_t0 = volume_available_t0;
    pos->volume_available_t1 = volume_available_t1;
    pos->volume_buy = volume_buy;
    pos->dvalue_buy = dvalue_buy;
    pos->volume_buy_traded = volume_buy_traded;
    pos->dvalue_buy_traded = dvalue_buy_traded;
    pos->volume_sell = volume_sell;
    pos->dvalue_sell = dvalue_sell;
    pos->volume_sell_traded = volume_sell_traded;
    pos->dvalue_sell_traded = dvalue_sell_traded;
    pos->count_order = count_order;
    return true;
}

// 将一行 DB 持仓快照写入持仓管理器；重复证券按后出现行覆盖。
bool apply_position_db_row(const db_position_row& row, position_manager& manager) {
    market_t market = market_t::NotSet;
    std::string_view code;
    if (!parse_internal_security_id(row.internal_security_id, market, code)) {
        return false;
    }

    const internal_security_id_t added = manager.add_security(code, row.internal_security_id, market);
    if (added.empty() || added != std::string_view(row.internal_security_id)) {
        return false;
    }

    position* pos = manager.get_position_mut(added);
    if (!pos) {
        return false;
    }

    position_lock guard(*pos);
    pos->volume_available_t0 = row.volume_available_t0;
    pos->volume_available_t1 = row.volume_available_t1;
    pos->volume_buy = row.volume_buy;
    pos->dvalue_buy = row.dvalue_buy;
    pos->volume_buy_traded = row.volume_buy_traded;
    pos->dvalue_buy_traded = row.dvalue_buy_traded;
    pos->volume_sell = row.volume_sell;
    pos->dvalue_sell = row.dvalue_sell;
    pos->volume_sell_traded = row.volume_sell_traded;
    pos->dvalue_sell_traded = row.dvalue_sell_traded;
    pos->count_order = row.count_order;
    return true;
}

// 读取可选 CSV 快照；文件不存在视作未加载，格式错误返回失败。
bool load_positions_csv_if_exists(std::string_view path, position_manager& manager, bool& loaded) {
    loaded = false;
    if (path.empty()) {
        return true;
    }

    std::ifstream in{std::string(path)};
    if (!in.is_open()) {
        return true;
    }
    loaded = true;

    std::size_t loaded_rows = 0;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comment_pos = line.find('#');
        if (comment_pos != std::string::npos) {
            line = line.substr(0, comment_pos);
        }
        line = trim_copy(std::move(line));
        if (line.empty()) {
            continue;
        }

        const std::vector<std::string> columns = split_csv_columns(line);
        if (columns.empty()) {
            continue;
        }

        const std::string record_type = to_lower_copy(columns[0]);
        if (record_type == "record_type") {
            continue;
        }
        if (record_type != "position") {
            ACCT_LOG_ERROR("position_loader", "invalid position bootstrap csv record type");
            return false;
        }
        if (!apply_position_seed_row(columns, manager)) {
            ACCT_LOG_ERROR("position_loader", "failed to parse/apply position bootstrap csv row");
            return false;
        }
        ++loaded_rows;
    }

    if (loaded_rows > 0) {
        ACCT_LOG_INFO("position_loader", "position bootstrap csv loaded");
    }
    return true;
}

// 打开 sqlite 只读连接，失败时统一返回 false。
bool open_sqlite_readonly(std::string_view db_path, sqlite_db_ptr& db) {
    if (db_path.empty()) {
        ACCT_LOG_ERROR("position_loader", "db path is empty");
        return false;
    }

    sqlite3* raw_db = nullptr;
    const int rc = sqlite3_open_v2(std::string(db_path).c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK || raw_db == nullptr) {
        if (raw_db != nullptr) {
            sqlite3_close(raw_db);
        }
        ACCT_LOG_ERROR("position_loader", "failed to open sqlite db");
        return false;
    }

    db.reset(raw_db);
    return true;
}

// 准备 SQL 语句并交由 RAII 句柄托管。
bool prepare_statement(sqlite3* db, std::string_view sql, sqlite_stmt_ptr& stmt) {
    if (!db) {
        ACCT_LOG_ERROR("position_loader", "sqlite handle is null");
        return false;
    }

    sqlite3_stmt* raw_stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &raw_stmt, nullptr);
    if (rc != SQLITE_OK || raw_stmt == nullptr) {
        if (raw_stmt != nullptr) {
            sqlite3_finalize(raw_stmt);
        }
        ACCT_LOG_ERROR("position_loader", "failed to prepare sqlite statement");
        return false;
    }

    stmt.reset(raw_stmt);
    return true;
}

// 读取必填 TEXT 列，空值或类型不匹配视为失败。
bool read_required_text(sqlite3_stmt* stmt, int column, std::string& out) {
    if (!stmt || sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
        return false;
    }

    const unsigned char* text = sqlite3_column_text(stmt, column);
    if (!text) {
        return false;
    }

    const int bytes = sqlite3_column_bytes(stmt, column);
    if (bytes < 0) {
        return false;
    }

    out.assign(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes));
    return true;
}

// 读取必填非负整型列并映射到 uint64。
bool read_required_u64(sqlite3_stmt* stmt, int column, uint64_t& out) {
    if (!stmt || sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
        return false;
    }

    const sqlite3_int64 value = sqlite3_column_int64(stmt, column);
    if (value < 0) {
        return false;
    }

    out = static_cast<uint64_t>(value);
    return true;
}

// 从 account_info 读取 FUND 行映射字段。
bool load_fund_from_db(sqlite3* db, account_id_t account_id, fund_info& fund) {
    sqlite_stmt_ptr stmt(nullptr, sqlite3_finalize);
    if (!prepare_statement(db, kFundQuerySql, stmt)) {
        return false;
    }

    if (sqlite3_bind_int64(stmt.get(), 1, static_cast<sqlite3_int64>(account_id)) != SQLITE_OK) {
        ACCT_LOG_ERROR("position_loader", "failed to bind account_id");
        return false;
    }

    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) {
        ACCT_LOG_ERROR("position_loader", "account_info not found for account_id");
        return false;
    }
    if (rc != SQLITE_ROW) {
        ACCT_LOG_ERROR("position_loader", "failed to query account_info row");
        return false;
    }

    if (!read_required_u64(stmt.get(), 0, fund.total_asset) || !read_required_u64(stmt.get(), 1, fund.available) ||
        !read_required_u64(stmt.get(), 2, fund.frozen) || !read_required_u64(stmt.get(), 3, fund.market_value)) {
        ACCT_LOG_ERROR("position_loader", "invalid account_info column value");
        return false;
    }

    return true;
}

// 从 positions 全表读取证券持仓并回写到持仓管理器。
bool load_positions_from_db(sqlite3* db, position_manager& manager) {
    sqlite_stmt_ptr stmt(nullptr, sqlite3_finalize);
    if (!prepare_statement(db, kPositionQuerySql, stmt)) {
        return false;
    }

    std::size_t loaded_rows = 0;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            ACCT_LOG_ERROR("position_loader", "failed while iterating positions rows");
            return false;
        }

        db_position_row row;
        if (!read_required_text(stmt.get(), 0, row.security_id) ||
            !read_required_text(stmt.get(), 1, row.internal_security_id) ||
            !read_required_u64(stmt.get(), 2, row.volume_available_t0) ||
            !read_required_u64(stmt.get(), 3, row.volume_available_t1) ||
            !read_required_u64(stmt.get(), 4, row.volume_buy) || !read_required_u64(stmt.get(), 5, row.dvalue_buy) ||
            !read_required_u64(stmt.get(), 6, row.volume_buy_traded) ||
            !read_required_u64(stmt.get(), 7, row.dvalue_buy_traded) ||
            !read_required_u64(stmt.get(), 8, row.volume_sell) || !read_required_u64(stmt.get(), 9, row.dvalue_sell) ||
            !read_required_u64(stmt.get(), 10, row.volume_sell_traded) ||
            !read_required_u64(stmt.get(), 11, row.dvalue_sell_traded) ||
            !read_required_u64(stmt.get(), 12, row.count_order)) {
            ACCT_LOG_ERROR("position_loader", "invalid positions column value");
            return false;
        }

        if (!apply_position_db_row(row, manager)) {
            ACCT_LOG_ERROR("position_loader", "failed to apply positions row");
            return false;
        }
        ++loaded_rows;
    }

    if (loaded_rows > 0) {
        ACCT_LOG_INFO("position_loader", "position bootstrap db loaded");
    }
    return true;
}

}  // namespace

// 文件模式构造：从 config_file + ".positions.csv" 读取证券行。
position_loader::position_loader(file_source source)
    : source_type_(source_type::File), source_path_(std::move(source.path)) {}

// DB 模式构造：从 sqlite DB 读取 FUND + positions。
position_loader::position_loader(db_source source) : source_type_(source_type::Db), source_path_(std::move(source.path)) {}

// 按已选模式加载快照，不做 DB/File 自动回退。
bool position_loader::load(account_id_t account_id, position_manager& manager) {
    switch (source_type_) {
        case source_type::File:
            return load_from_file(manager);
        case source_type::Db:
            return load_from_db(account_id, manager);
    }
    return false;
}

// 执行文件模式加载；CSV 缺失时保持默认空仓。
bool position_loader::load_from_file(position_manager& manager) const {
    if (source_path_.empty()) {
        return true;
    }

    bool loaded = false;
    const std::string file_csv_path = source_path_ + ".positions.csv";
    return load_positions_csv_if_exists(file_csv_path, manager, loaded);
}

// 执行 DB 模式加载；先加载 FUND，再加载全量证券持仓。
bool position_loader::load_from_db(account_id_t account_id, position_manager& manager) const {
    sqlite_db_ptr db(nullptr, sqlite3_close);
    if (!open_sqlite_readonly(source_path_, db)) {
        return false;
    }

    fund_info fund;
    if (!load_fund_from_db(db.get(), account_id, fund)) {
        return false;
    }
    if (!manager.overwrite_fund_info(fund)) {
        ACCT_LOG_ERROR("position_loader", "failed to write fund snapshot into position manager");
        return false;
    }

    return load_positions_from_db(db.get(), manager);
}

}  // namespace acct_service
