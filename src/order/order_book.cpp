#include "order/order_book.hpp"

#include <algorithm>
#include <limits>

#include "common/error.hpp"
#include "common/log.hpp"

namespace acct_service {

namespace {

bool is_terminal_status(order_status_t status) {
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

int status_progress_rank(order_status_t status) {
    switch (status) {
        case order_status_t::MarketAccepted:
            return 7;
        case order_status_t::BrokerAccepted:
            return 6;
        case order_status_t::TraderSubmitted:
            return 5;
        case order_status_t::TraderPending:
            return 4;
        case order_status_t::RiskControllerAccepted:
            return 3;
        case order_status_t::RiskControllerPending:
            return 2;
        case order_status_t::StrategySubmitted:
            return 1;
        default:
            return 0;
    }
}

uint64_t saturating_add(uint64_t lhs, uint64_t rhs) {
    const uint64_t limit = std::numeric_limits<uint64_t>::max();
    if (lhs > limit - rhs) {
        return limit;
    }
    return lhs + rhs;
}

}  // namespace

bool order_entry::is_active() const noexcept { return !is_terminal(); }

bool order_entry::is_terminal() const noexcept {
    return is_terminal_status(request.order_status.load(std::memory_order_acquire));
}

order_book::order_book() {
    free_slots_.reserve(kMaxActiveOrders);
    for (std::size_t i = 0; i < kMaxActiveOrders; ++i) {
        free_slots_.push_back(kMaxActiveOrders - 1 - i);
    }
}

bool order_book::add_order(const order_entry& entry) {
    const internal_order_id_t order_id = entry.request.internal_order_id;
    if (order_id == 0) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::InvalidOrderId, "order_book", "order id is zero", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    lock_guard<spinlock> guard(lock_);

    if (id_to_index_.find(order_id) != id_to_index_.end()) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::DuplicateOrder, "order_book", "duplicate order id", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    if (free_slots_.empty()) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderBookFull, "order_book", "order book free slots exhausted", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    const std::size_t index = free_slots_.back();
    free_slots_.pop_back();

    order_entry stored = entry;
    if (stored.submit_time_ns == 0) {
        stored.submit_time_ns = now_ns();
    }
    if (stored.last_update_ns == 0) {
        stored.last_update_ns = stored.submit_time_ns;
    }

    if (stored.request.order_type == order_type_t::New && stored.request.volume_remain == 0 &&
        stored.request.volume_entrust >= stored.request.volume_traded) {
        stored.request.volume_remain = stored.request.volume_entrust - stored.request.volume_traded;
    }

    orders_[index] = stored;
    id_to_index_[order_id] = index;

    if (stored.request.broker_order_id.as_uint != 0) {
        broker_id_map_[stored.request.broker_order_id.as_uint] = order_id;
    }

    if (stored.request.internal_security_id != 0) {
        security_orders_[stored.request.internal_security_id].push_back(order_id);
    }

    if (stored.is_split_child && stored.parent_order_id != 0) {
        parent_to_children_[stored.parent_order_id].push_back(order_id);
        child_to_parent_[order_id] = stored.parent_order_id;
        refresh_parent_from_children_nolock(stored.parent_order_id);
    }

    ++active_count_;
    return true;
}

order_entry* order_book::find_order(internal_order_id_t order_id) {
    lock_guard<spinlock> guard(lock_);
    return find_order_nolock(order_id);
}

const order_entry* order_book::find_order(internal_order_id_t order_id) const {
    lock_guard<spinlock> guard(lock_);
    return find_order_nolock(order_id);
}

order_entry* order_book::find_by_broker_id(uint64_t broker_order_id) {
    lock_guard<spinlock> guard(lock_);

    const auto id_it = broker_id_map_.find(broker_order_id);
    if (id_it == broker_id_map_.end()) {
        return nullptr;
    }
    return find_order_nolock(id_it->second);
}

bool order_book::update_status(internal_order_id_t order_id, order_status_t new_status) {
    lock_guard<spinlock> guard(lock_);

    order_entry* entry = find_order_nolock(order_id);
    if (!entry) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderNotFound, "order_book", "update_status order not found", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    entry->request.order_status.store(new_status, std::memory_order_release);
    entry->last_update_ns = now_ns();

    if (new_status == order_status_t::TraderError && parent_to_children_.find(order_id) != parent_to_children_.end()) {
        split_parent_error_latched_.insert(order_id);
    }

    const auto parent_it = child_to_parent_.find(order_id);
    if (parent_it != child_to_parent_.end()) {
        refresh_parent_from_children_nolock(parent_it->second);
    }

    return true;
}

bool order_book::update_trade(internal_order_id_t order_id, volume_t vol, dprice_t px, dvalue_t val, dvalue_t fee) {
    lock_guard<spinlock> guard(lock_);

    order_entry* entry = find_order_nolock(order_id);
    if (!entry) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderNotFound, "order_book", "update_trade order not found", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    order_request& request = entry->request;
    request.volume_traded = saturating_add(request.volume_traded, vol);
    if (request.volume_entrust > 0 && request.volume_traded > request.volume_entrust) {
        request.volume_traded = request.volume_entrust;
    }

    if (vol >= request.volume_remain) {
        request.volume_remain = 0;
    } else {
        request.volume_remain -= vol;
    }

    request.dvalue_traded = saturating_add(request.dvalue_traded, val);
    request.dfee_executed = saturating_add(request.dfee_executed, fee);

    if (request.volume_traded > 0) {
        if (request.dvalue_traded > 0) {
            request.dprice_traded = request.dvalue_traded / request.volume_traded;
        } else {
            request.dprice_traded = px;
        }
    }

    if (request.volume_remain == 0) {
        const order_status_t current = request.order_status.load(std::memory_order_acquire);
        if (!is_terminal_status(current)) {
            request.order_status.store(order_status_t::Finished, std::memory_order_release);
        }
    }

    entry->last_update_ns = now_ns();

    const auto parent_it = child_to_parent_.find(order_id);
    if (parent_it != child_to_parent_.end()) {
        refresh_parent_from_children_nolock(parent_it->second);
    }

    return true;
}

bool order_book::archive_order(internal_order_id_t order_id) {
    lock_guard<spinlock> guard(lock_);

    const auto it = id_to_index_.find(order_id);
    if (it == id_to_index_.end()) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderNotFound, "order_book", "archive_order order not found", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    const std::size_t index = it->second;
    const order_entry& entry = orders_[index];

    if (entry.request.broker_order_id.as_uint != 0) {
        const auto broker_it = broker_id_map_.find(entry.request.broker_order_id.as_uint);
        if (broker_it != broker_id_map_.end() && broker_it->second == order_id) {
            broker_id_map_.erase(broker_it);
        }
    }

    if (entry.request.internal_security_id != 0) {
        auto sec_it = security_orders_.find(entry.request.internal_security_id);
        if (sec_it != security_orders_.end()) {
            auto& orders = sec_it->second;
            orders.erase(std::remove(orders.begin(), orders.end(), order_id), orders.end());
            if (orders.empty()) {
                security_orders_.erase(sec_it);
            }
        }
    }

    id_to_index_.erase(it);
    orders_[index] = order_entry{};
    free_slots_.push_back(index);

    if (active_count_ > 0) {
        --active_count_;
    }

    return true;
}

std::vector<internal_order_id_t> order_book::get_active_order_ids() const {
    lock_guard<spinlock> guard(lock_);

    std::vector<internal_order_id_t> result;
    result.reserve(id_to_index_.size());
    for (const auto& kv : id_to_index_) {
        result.push_back(kv.first);
    }
    return result;
}

std::vector<internal_order_id_t> order_book::get_orders_by_security(internal_security_id_t security_id) const {
    lock_guard<spinlock> guard(lock_);

    const auto it = security_orders_.find(security_id);
    if (it == security_orders_.end()) {
        return {};
    }
    return it->second;
}

std::vector<internal_order_id_t> order_book::get_children(internal_order_id_t parent_id) const {
    lock_guard<spinlock> guard(lock_);

    const auto it = parent_to_children_.find(parent_id);
    if (it == parent_to_children_.end()) {
        return {};
    }
    return it->second;
}

bool order_book::try_get_parent(internal_order_id_t child_id, internal_order_id_t& out_parent_id) const noexcept {
    lock_guard<spinlock> guard(lock_);

    const auto it = child_to_parent_.find(child_id);
    if (it == child_to_parent_.end()) {
        return false;
    }
    out_parent_id = it->second;
    return true;
}

std::size_t order_book::active_count() const noexcept {
    lock_guard<spinlock> guard(lock_);
    return active_count_;
}

internal_order_id_t order_book::next_order_id() noexcept {
    return next_order_id_.fetch_add(1, std::memory_order_relaxed);
}

void order_book::clear() {
    lock_guard<spinlock> guard(lock_);

    id_to_index_.clear();
    broker_id_map_.clear();
    security_orders_.clear();
    parent_to_children_.clear();
    child_to_parent_.clear();
    split_parent_error_latched_.clear();

    free_slots_.clear();
    free_slots_.reserve(kMaxActiveOrders);

    for (std::size_t i = 0; i < kMaxActiveOrders; ++i) {
        orders_[i] = order_entry{};
        free_slots_.push_back(kMaxActiveOrders - 1 - i);
    }

    active_count_ = 0;
}

order_entry* order_book::find_order_nolock(internal_order_id_t order_id) {
    const auto it = id_to_index_.find(order_id);
    if (it == id_to_index_.end()) {
        return nullptr;
    }
    return &orders_[it->second];
}

const order_entry* order_book::find_order_nolock(internal_order_id_t order_id) const {
    const auto it = id_to_index_.find(order_id);
    if (it == id_to_index_.end()) {
        return nullptr;
    }
    return &orders_[it->second];
}

void order_book::refresh_parent_from_children_nolock(internal_order_id_t parent_id) {
    order_entry* parent = find_order_nolock(parent_id);
    if (!parent) {
        error_status status = ACCT_MAKE_ERROR(
            error_domain::order, error_code::OrderInvariantBroken, "order_book", "parent missing while refreshing split state", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return;
    }

    const auto children_it = parent_to_children_.find(parent_id);
    if (children_it == parent_to_children_.end()) {
        return;
    }

    volume_t total_volume_traded = 0;
    volume_t total_volume_remain = 0;
    dvalue_t total_dvalue_traded = 0;
    dvalue_t total_fee = 0;
    timestamp_ns_t latest_update_ns = parent->last_update_ns;

    bool all_terminal = true;
    order_status_t best_progress_status = order_status_t::NotSet;
    int best_progress_rank = -1;
    std::size_t new_child_count = 0;

    for (internal_order_id_t child_id : children_it->second) {
        const order_entry* child = find_order_nolock(child_id);
        if (!child) {
            continue;
        }

        if (child->request.order_type != order_type_t::New) {
            continue;
        }

        ++new_child_count;

        total_volume_traded = saturating_add(total_volume_traded, child->request.volume_traded);
        total_volume_remain = saturating_add(total_volume_remain, child->request.volume_remain);
        total_dvalue_traded = saturating_add(total_dvalue_traded, child->request.dvalue_traded);
        total_fee = saturating_add(total_fee, child->request.dfee_executed);
        latest_update_ns = std::max(latest_update_ns, child->last_update_ns);

        const order_status_t child_status = child->request.order_status.load(std::memory_order_acquire);
        if (!is_terminal_status(child_status)) {
            all_terminal = false;
        }

        const int rank = status_progress_rank(child_status);
        if (rank > best_progress_rank) {
            best_progress_rank = rank;
            best_progress_status = child_status;
        }
    }

    if (new_child_count == 0) {
        return;
    }

    parent->request.volume_traded = total_volume_traded;
    parent->request.volume_remain = total_volume_remain;
    parent->request.dvalue_traded = total_dvalue_traded;
    parent->request.dfee_executed = total_fee;

    if (total_volume_traded > 0) {
        parent->request.dprice_traded = total_dvalue_traded / total_volume_traded;
    }

    if (parent->request.volume_entrust > 0 && parent->request.volume_remain > parent->request.volume_entrust) {
        parent->request.volume_remain = parent->request.volume_entrust;
    }

    parent->last_update_ns = latest_update_ns;

    if (split_parent_error_latched_.find(parent_id) != split_parent_error_latched_.end()) {
        parent->request.order_status.store(order_status_t::TraderError, std::memory_order_release);
        return;
    }

    if (all_terminal) {
        parent->request.order_status.store(order_status_t::Finished, std::memory_order_release);
        return;
    }

    if (best_progress_status != order_status_t::NotSet) {
        parent->request.order_status.store(best_progress_status, std::memory_order_release);
    }
}

}  // namespace acct_service
