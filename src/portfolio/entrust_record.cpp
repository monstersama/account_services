#include "portfolio/entrust_record.hpp"

#include <atomic>
#include <fstream>

namespace acct_service {

namespace {

bool is_terminal(order_status_t status) {
    switch (status) {
        case order_status_t::RiskControllerRejected:
        case order_status_t::TraderRejected:
        case order_status_t::TraderError:
        case order_status_t::BrokerRejected:
        case order_status_t::MarketRejected:
        case order_status_t::Finished:
        case order_status_t::Unknown:
            return true;
        default:
            return false;
    }
}

}  // namespace

entrust_record entrust_record::from_order_request(const order_request& req) {
    entrust_record record{};
    record.order_id = req.internal_order_id;
    record.security_id = req.internal_security_id;
    record.order_type = req.order_type;
    record.side = req.trade_side;
    record.market = req.market;
    record.status = req.order_status.load(std::memory_order_relaxed);
    record.volume_entrust = req.volume_entrust;
    record.volume_traded = req.volume_traded;
    record.price_entrust = req.dprice_entrust;
    record.price_traded_avg = req.dprice_traded;
    record.value_traded = req.dvalue_traded;
    record.fee = req.dfee_executed;
    record.time_entrust = req.md_time_entrust;
    record.time_first_trade = req.md_time_traded_first;
    record.time_last_update = (req.md_time_market_response != 0) ? req.md_time_market_response : req.md_time_broker_response;
    record.broker_order_id = req.broker_order_id.as_str;
    record.security_code = req.security_id;
    return record;
}

bool entrust_record_manager::load_today_entrusts(const std::string& db_path, account_id_t account_id) {
    (void)db_path;
    (void)account_id;
    entrusts_.clear();
    id_index_.clear();
    security_index_.clear();
    return true;
}

void entrust_record_manager::add_or_update(const entrust_record& record) {
    const auto it = id_index_.find(record.order_id);
    if (it != id_index_.end()) {
        entrusts_[it->second] = record;
        return;
    }

    const std::size_t index = entrusts_.size();
    entrusts_.push_back(record);
    id_index_[record.order_id] = index;
    security_index_[record.security_id].push_back(index);
}

void entrust_record_manager::update_from_order(const order_request& order) {
    add_or_update(entrust_record::from_order_request(order));
}

const entrust_record* entrust_record_manager::find_entrust(internal_order_id_t order_id) const {
    const auto it = id_index_.find(order_id);
    if (it == id_index_.end()) {
        return nullptr;
    }
    return &entrusts_[it->second];
}

std::vector<const entrust_record*> entrust_record_manager::get_entrusts_by_security(
    internal_security_id_t security_id) const {
    std::vector<const entrust_record*> result;
    const auto it = security_index_.find(security_id);
    if (it == security_index_.end()) {
        return result;
    }

    result.reserve(it->second.size());
    for (std::size_t index : it->second) {
        result.push_back(&entrusts_[index]);
    }
    return result;
}

std::vector<const entrust_record*> entrust_record_manager::get_active_entrusts() const {
    std::vector<const entrust_record*> result;
    for (const entrust_record& record : entrusts_) {
        if (!is_terminal(record.status)) {
            result.push_back(&record);
        }
    }
    return result;
}

std::vector<const entrust_record*> entrust_record_manager::get_all_entrusts() const {
    std::vector<const entrust_record*> result;
    result.reserve(entrusts_.size());
    for (const entrust_record& record : entrusts_) {
        result.push_back(&record);
    }
    return result;
}

std::size_t entrust_record_manager::entrust_count() const noexcept { return entrusts_.size(); }

std::size_t entrust_record_manager::active_count() const noexcept {
    std::size_t count = 0;
    for (const entrust_record& record : entrusts_) {
        if (!is_terminal(record.status)) {
            ++count;
        }
    }
    return count;
}

bool entrust_record_manager::save_to_db(const std::string& db_path) const {
    if (db_path.empty()) {
        return false;
    }

    std::ofstream out(db_path + ".entrusts.csv");
    if (!out.is_open()) {
        return false;
    }

    for (const entrust_record& record : entrusts_) {
        out << record.order_id << ',' << record.security_id.view() << ',' << static_cast<int>(record.order_type) << ','
            << static_cast<int>(record.side) << ',' << static_cast<int>(record.market) << ','
            << static_cast<int>(record.status) << ',' << record.volume_entrust << ',' << record.volume_traded << ','
            << record.price_entrust << ',' << record.price_traded_avg << ',' << record.value_traded << ',' << record.fee
            << ',' << record.time_entrust << ',' << record.time_first_trade << ',' << record.time_last_update << ','
            << record.broker_order_id.c_str() << ',' << record.security_code.c_str() << '\n';
    }

    return true;
}

}  // namespace acct_service
