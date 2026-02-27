#include "portfolio/trade_record.hpp"

#include <fstream>
#include <utility>

namespace acct_service {

bool trade_record_manager::load_today_trades(const std::string& db_path, account_id_t account_id) {
    (void)db_path;
    (void)account_id;
    trades_.clear();
    id_index_.clear();
    order_index_.clear();
    security_index_.clear();
    next_trade_id_ = 1;
    return true;
}

void trade_record_manager::add_trade(const trade_record& record) {
    trade_record stored = record;

    if (stored.trade_id == 0 || id_index_.find(stored.trade_id) != id_index_.end()) {
        stored.trade_id = next_trade_id_++;
    } else if (stored.trade_id >= next_trade_id_) {
        next_trade_id_ = stored.trade_id + 1;
    }

    const std::size_t index = trades_.size();
    trades_.push_back(stored);
    id_index_[stored.trade_id] = index;
    order_index_[stored.order_id].push_back(index);
    security_index_[stored.security_id].push_back(index);
}

const trade_record* trade_record_manager::find_trade(uint64_t trade_id) const {
    const auto it = id_index_.find(trade_id);
    if (it == id_index_.end()) {
        return nullptr;
    }
    return &trades_[it->second];
}

std::vector<const trade_record*> trade_record_manager::get_trades_by_order(internal_order_id_t order_id) const {
    std::vector<const trade_record*> result;
    const auto it = order_index_.find(order_id);
    if (it == order_index_.end()) {
        return result;
    }

    result.reserve(it->second.size());
    for (std::size_t index : it->second) {
        result.push_back(&trades_[index]);
    }
    return result;
}

std::vector<const trade_record*> trade_record_manager::get_trades_by_security(
    internal_security_id_t security_id) const {
    std::vector<const trade_record*> result;
    const auto it = security_index_.find(security_id);
    if (it == security_index_.end()) {
        return result;
    }

    result.reserve(it->second.size());
    for (std::size_t index : it->second) {
        result.push_back(&trades_[index]);
    }
    return result;
}

std::vector<const trade_record*> trade_record_manager::get_all_trades() const {
    std::vector<const trade_record*> result;
    result.reserve(trades_.size());
    for (const trade_record& record : trades_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t trade_record_manager::trade_count() const noexcept { return trades_.size(); }

dvalue_t trade_record_manager::total_traded_value() const noexcept {
    dvalue_t total = 0;
    for (const trade_record& record : trades_) {
        total += record.value;
    }
    return total;
}

dvalue_t trade_record_manager::total_fee() const noexcept {
    dvalue_t total = 0;
    for (const trade_record& record : trades_) {
        total += record.fee;
    }
    return total;
}

bool trade_record_manager::save_to_db(const std::string& db_path) const {
    if (db_path.empty()) {
        return false;
    }

    std::ofstream out(db_path + ".trades.csv");
    if (!out.is_open()) {
        return false;
    }

    for (const trade_record& record : trades_) {
        out << record.trade_id << ',' << record.order_id << ',' << record.security_id.view() << ','
            << static_cast<int>(record.side) << ',' << record.volume << ',' << record.price << ',' << record.value << ','
            << record.fee << ',' << record.trade_time << ',' << record.local_time << ','
            << record.broker_trade_id.c_str() << '\n';
    }

    return true;
}

}  // namespace acct_service
