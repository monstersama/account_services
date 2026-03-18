#include "order/order_book.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include "common/error.hpp"
#include "common/log.hpp"
#include "common/security_identity.hpp"

namespace acct_service {

namespace {

InternalOrderId saturated_next_order_id(InternalOrderId order_id) noexcept {
    if (order_id == std::numeric_limits<InternalOrderId>::max()) {
        return order_id;
    }
    return order_id + 1;
}

bool is_terminal_state(OrderState status) {
    switch (status) {
        case OrderState::RiskControllerRejected:
        case OrderState::TraderRejected:
        case OrderState::TraderError:
        case OrderState::BrokerRejected:
        case OrderState::MarketRejected:
        case OrderState::Finished:
        case OrderState::Unknown:
            return true;
        default:
            return false;
    }
}

int status_progress_rank(OrderState status) {
    switch (status) {
        case OrderState::MarketAccepted:
            return 7;
        case OrderState::BrokerAccepted:
            return 6;
        case OrderState::TraderSubmitted:
            return 5;
        case OrderState::TraderPending:
            return 4;
        case OrderState::RiskControllerAccepted:
            return 3;
        case OrderState::RiskControllerPending:
            return 2;
        case OrderState::UserSubmitted:
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

// 把订单上的内部证券键收敛成 canonical MIC，避免旧键和新键在索引中分裂。
bool normalize_order_security_key(std::string_view security_key, InternalSecurityId& out_key) {
    if (security_key.empty()) {
        return false;
    }
    return normalize_internal_security_id(security_key, out_key);
}

}  // namespace

bool OrderEntry::is_active() const noexcept { return !is_terminal(); }

bool OrderEntry::is_terminal() const noexcept {
    return is_terminal_state(request.order_state.load(std::memory_order_acquire));
}

OrderBook::OrderBook() {
    free_slots_.reserve(kMaxActiveOrders);
    for (std::size_t i = 0; i < kMaxActiveOrders; ++i) {
        free_slots_.push_back(kMaxActiveOrders - 1 - i);
    }
}

bool OrderBook::add_order(const OrderEntry& entry) {
    const InternalOrderId order_id = entry.request.internal_order_id;
    if (order_id == 0) {
        ErrorStatus status =
            ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::InvalidOrderId, "OrderBook", "order id is zero", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    order_change_callback_t callback;
    OrderEntry snapshot{};
    {
        LockGuard<SpinLock> guard(lock_);

        if (id_to_index_.find(order_id) != id_to_index_.end()) {
            ErrorStatus status =
                ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::DuplicateOrder, "OrderBook", "duplicate order id", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        if (free_slots_.empty()) {
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderBookFull, "OrderBook",
                                                 "order book free slots exhausted", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        const std::size_t index = free_slots_.back();
        free_slots_.pop_back();

        OrderEntry stored = entry;
        if (stored.submit_time_ns == 0) {
            stored.submit_time_ns = now_ns();
        }
        if (stored.last_update_ns == 0) {
            stored.last_update_ns = stored.submit_time_ns;
        }

        if (stored.request.order_type == OrderType::New && stored.request.volume_remain == 0 &&
            stored.request.volume_entrust >= stored.request.volume_traded) {
            stored.request.volume_remain = stored.request.volume_entrust - stored.request.volume_traded;
        }

        orders_[index] = stored;
        id_to_index_[order_id] = index;
        ensure_next_order_id_at_least(saturated_next_order_id(order_id));

        if (stored.request.broker_order_id.as_uint != 0) {
            broker_id_map_[stored.request.broker_order_id.as_uint] = order_id;
        }

        if (!stored.request.internal_security_id.empty()) {
            InternalSecurityId security_id;
            if (normalize_order_security_key(stored.request.internal_security_id.view(), security_id)) {
                stored.request.internal_security_id = security_id;
                security_orders_[security_id].push_back(order_id);
            }
        }

        if (stored.is_split_child && stored.parent_order_id != 0) {
            parent_to_children_[stored.parent_order_id].push_back(order_id);
            child_to_parent_[order_id] = stored.parent_order_id;
            if (!is_managed_parent_nolock(stored.parent_order_id)) {
                refresh_parent_from_children_nolock(stored.parent_order_id);
            }
        }

        ++active_count_;
        snapshot = orders_[index];
        callback = change_callback_;
    }

    if (callback) {
        callback(snapshot, order_book_event_t::Added);
    }
    return true;
}

OrderEntry* OrderBook::find_order(InternalOrderId order_id) {
    LockGuard<SpinLock> guard(lock_);
    return find_order_nolock(order_id);
}

const OrderEntry* OrderBook::find_order(InternalOrderId order_id) const {
    LockGuard<SpinLock> guard(lock_);
    return find_order_nolock(order_id);
}

OrderEntry* OrderBook::find_by_broker_id(uint64_t broker_order_id) {
    LockGuard<SpinLock> guard(lock_);

    const auto id_it = broker_id_map_.find(broker_order_id);
    if (id_it == broker_id_map_.end()) {
        return nullptr;
    }
    return find_order_nolock(id_it->second);
}

bool OrderBook::update_state(InternalOrderId order_id, OrderState new_state) {
    order_change_callback_t callback;
    OrderEntry snapshot{};
    {
        LockGuard<SpinLock> guard(lock_);

        OrderEntry* entry = find_order_nolock(order_id);
        if (!entry) {
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "OrderBook",
                                                 "update_state order not found", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        entry->request.order_state.store(new_state, std::memory_order_release);
        entry->last_update_ns = now_ns();

        if (new_state == OrderState::TraderError && parent_to_children_.find(order_id) != parent_to_children_.end() &&
            !is_managed_parent_nolock(order_id)) {
            split_parent_error_latched_.insert(order_id);
        }

        const auto parent_it = child_to_parent_.find(order_id);
        if (parent_it != child_to_parent_.end() && !is_managed_parent_nolock(parent_it->second)) {
            refresh_parent_from_children_nolock(parent_it->second);
        }

        snapshot = *entry;
        callback = change_callback_;
    }

    if (callback) {
        callback(snapshot, order_book_event_t::StatusUpdated);
    }
    return true;
}

bool OrderBook::update_trade(InternalOrderId order_id, Volume vol, DPrice px, DValue val, DValue fee) {
    order_change_callback_t callback;
    OrderEntry snapshot{};
    {
        LockGuard<SpinLock> guard(lock_);

        OrderEntry* entry = find_order_nolock(order_id);
        if (!entry) {
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "OrderBook",
                                                 "update_trade order not found", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        OrderRequest& request = entry->request;
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
            const OrderState current = request.order_state.load(std::memory_order_acquire);
            if (!is_terminal_state(current)) {
                request.order_state.store(OrderState::Finished, std::memory_order_release);
            }
        }

        entry->last_update_ns = now_ns();

        const auto parent_it = child_to_parent_.find(order_id);
        if (parent_it != child_to_parent_.end() && !is_managed_parent_nolock(parent_it->second)) {
            refresh_parent_from_children_nolock(parent_it->second);
        }

        snapshot = *entry;
        callback = change_callback_;
    }

    if (callback) {
        callback(snapshot, order_book_event_t::TradeUpdated);
    }
    return true;
}

// 标记执行引擎托管的父单，后续子单变更不再触发旧聚合刷新。
bool OrderBook::mark_managed_parent(InternalOrderId parent_id) {
    LockGuard<SpinLock> guard(lock_);

    OrderEntry* parent = find_order_nolock(parent_id);
    if (!parent) {
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "OrderBook",
                                             "mark_managed_parent parent not found", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return false;
    }

    managed_parent_ids_.insert(parent_id);
    return true;
}

// 用执行引擎的派生视图覆盖受管父单镜像，避免被旧的 child volume_remain 语义污染。
bool OrderBook::sync_managed_parent_view(InternalOrderId parent_id, const ManagedParentView& view,
                                         OrderState parent_state) {
    order_change_callback_t callback;
    OrderEntry snapshot{};
    {
        LockGuard<SpinLock> guard(lock_);

        OrderEntry* parent = find_order_nolock(parent_id);
        if (!parent) {
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "OrderBook",
                                                 "sync_managed_parent_view parent not found", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        managed_parent_ids_.insert(parent_id);
        const Volume next_volume_remain = (view.target_volume >= view.confirmed_traded_volume)
                                              ? (view.target_volume - view.confirmed_traded_volume)
                                              : 0;
        const DPrice next_dprice_traded =
            (view.confirmed_traded_volume > 0) ? (view.confirmed_traded_value / view.confirmed_traded_volume) : 0;
        const OrderState current_state = parent->request.order_state.load(std::memory_order_acquire);

        // 父单镜像没有任何对外可见变化时，不重复触发 ParentRefreshed 和业务日志。
        if (parent->request.execution_algo == view.execution_algo &&
            parent->request.execution_state == view.execution_state &&
            parent->request.target_volume == view.target_volume &&
            parent->request.working_volume == view.working_volume &&
            parent->request.schedulable_volume == view.schedulable_volume &&
            parent->request.volume_entrust == view.target_volume &&
            parent->request.volume_traded == view.confirmed_traded_volume &&
            parent->request.volume_remain == next_volume_remain &&
            parent->request.dvalue_traded == view.confirmed_traded_value &&
            parent->request.dfee_executed == view.confirmed_fee &&
            parent->request.dprice_traded == next_dprice_traded && current_state == parent_state) {
            return true;
        }

        parent->request.execution_algo = view.execution_algo;
        parent->request.execution_state = view.execution_state;
        parent->request.target_volume = view.target_volume;
        parent->request.working_volume = view.working_volume;
        parent->request.schedulable_volume = view.schedulable_volume;
        parent->request.volume_entrust = view.target_volume;
        parent->request.volume_traded = view.confirmed_traded_volume;
        parent->request.volume_remain = next_volume_remain;
        parent->request.dvalue_traded = view.confirmed_traded_value;
        parent->request.dfee_executed = view.confirmed_fee;
        parent->request.dprice_traded = next_dprice_traded;
        parent->request.order_state.store(parent_state, std::memory_order_release);
        parent->last_update_ns = now_ns();

        snapshot = *parent;
        callback = change_callback_;
    }

    if (callback) {
        callback(snapshot, order_book_event_t::ParentRefreshed);
    }
    return true;
}

bool OrderBook::archive_order(InternalOrderId order_id) {
    order_change_callback_t callback;
    OrderEntry snapshot{};
    {
        LockGuard<SpinLock> guard(lock_);

        const auto it = id_to_index_.find(order_id);
        if (it == id_to_index_.end()) {
            ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderNotFound, "OrderBook",
                                                 "archive_order order not found", 0);
            record_error(status);
            ACCT_LOG_ERROR_STATUS(status);
            return false;
        }

        const std::size_t index = it->second;
        const OrderEntry& entry = orders_[index];
        snapshot = entry;

        if (entry.request.broker_order_id.as_uint != 0) {
            const auto broker_it = broker_id_map_.find(entry.request.broker_order_id.as_uint);
            if (broker_it != broker_id_map_.end() && broker_it->second == order_id) {
                broker_id_map_.erase(broker_it);
            }
        }

        if (!entry.request.internal_security_id.empty()) {
            InternalSecurityId security_id;
            const bool has_security_id =
                normalize_order_security_key(entry.request.internal_security_id.view(), security_id);
            auto sec_it = has_security_id ? security_orders_.find(security_id) : security_orders_.end();
            if (sec_it != security_orders_.end()) {
                auto& orders = sec_it->second;
                orders.erase(std::remove(orders.begin(), orders.end(), order_id), orders.end());
                if (orders.empty()) {
                    security_orders_.erase(sec_it);
                }
            }
        }

        id_to_index_.erase(it);
        managed_parent_ids_.erase(order_id);
        split_parent_error_latched_.erase(order_id);
        orders_[index] = OrderEntry{};
        free_slots_.push_back(index);

        if (active_count_ > 0) {
            --active_count_;
        }

        callback = change_callback_;
    }

    if (callback) {
        callback(snapshot, order_book_event_t::Archived);
    }
    return true;
}

std::vector<InternalOrderId> OrderBook::get_active_order_ids() const {
    LockGuard<SpinLock> guard(lock_);

    std::vector<InternalOrderId> result;
    result.reserve(id_to_index_.size());
    for (const auto& kv : id_to_index_) {
        result.push_back(kv.first);
    }
    return result;
}

std::vector<InternalOrderId> OrderBook::get_orders_by_security(InternalSecurityId security_id) const {
    LockGuard<SpinLock> guard(lock_);

    InternalSecurityId normalized_security_id;
    if (!normalize_order_security_key(security_id.view(), normalized_security_id)) {
        return {};
    }

    const auto it = security_orders_.find(normalized_security_id);
    if (it == security_orders_.end()) {
        return {};
    }
    return it->second;
}

std::vector<InternalOrderId> OrderBook::get_children(InternalOrderId parent_id) const {
    LockGuard<SpinLock> guard(lock_);

    const auto it = parent_to_children_.find(parent_id);
    if (it == parent_to_children_.end()) {
        return {};
    }
    return it->second;
}

bool OrderBook::try_get_parent(InternalOrderId child_id, InternalOrderId& out_parent_id) const noexcept {
    LockGuard<SpinLock> guard(lock_);

    const auto it = child_to_parent_.find(child_id);
    if (it == child_to_parent_.end()) {
        return false;
    }
    out_parent_id = it->second;
    return true;
}

std::size_t OrderBook::active_count() const noexcept {
    LockGuard<SpinLock> guard(lock_);
    return active_count_;
}

InternalOrderId OrderBook::next_order_id() noexcept { return next_order_id_.fetch_add(1, std::memory_order_relaxed); }

void OrderBook::ensure_next_order_id_at_least(InternalOrderId next_id) noexcept {
    InternalOrderId current = next_order_id_.load(std::memory_order_acquire);
    while (current < next_id) {
        if (next_order_id_.compare_exchange_weak(current, next_id, std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            break;
        }
    }
}

void OrderBook::clear() {
    LockGuard<SpinLock> guard(lock_);

    id_to_index_.clear();
    broker_id_map_.clear();
    security_orders_.clear();
    parent_to_children_.clear();
    child_to_parent_.clear();
    managed_parent_ids_.clear();
    split_parent_error_latched_.clear();

    free_slots_.clear();
    free_slots_.reserve(kMaxActiveOrders);

    for (std::size_t i = 0; i < kMaxActiveOrders; ++i) {
        orders_[i] = OrderEntry{};
        free_slots_.push_back(kMaxActiveOrders - 1 - i);
    }

    active_count_ = 0;
}

void OrderBook::set_change_callback(order_change_callback_t callback) {
    LockGuard<SpinLock> guard(lock_);
    change_callback_ = std::move(callback);
}

bool OrderBook::is_managed_parent_nolock(InternalOrderId parent_id) const noexcept {
    return managed_parent_ids_.find(parent_id) != managed_parent_ids_.end();
}

OrderEntry* OrderBook::find_order_nolock(InternalOrderId order_id) {
    const auto it = id_to_index_.find(order_id);
    if (it == id_to_index_.end()) {
        return nullptr;
    }
    return &orders_[it->second];
}

const OrderEntry* OrderBook::find_order_nolock(InternalOrderId order_id) const {
    const auto it = id_to_index_.find(order_id);
    if (it == id_to_index_.end()) {
        return nullptr;
    }
    return &orders_[it->second];
}

void OrderBook::refresh_parent_from_children_nolock(InternalOrderId parent_id) {
    if (is_managed_parent_nolock(parent_id)) {
        return;
    }

    OrderEntry* parent = find_order_nolock(parent_id);
    if (!parent) {
        ErrorStatus status = ACCT_MAKE_ERROR(ErrorDomain::order, ErrorCode::OrderInvariantBroken, "OrderBook",
                                             "parent missing while refreshing split state", 0);
        record_error(status);
        ACCT_LOG_ERROR_STATUS(status);
        return;
    }

    const auto children_it = parent_to_children_.find(parent_id);
    if (children_it == parent_to_children_.end()) {
        return;
    }

    Volume total_volume_traded = 0;
    Volume total_volume_remain = 0;
    DValue total_dvalue_traded = 0;
    DValue total_fee = 0;
    TimestampNs latest_update_ns = parent->last_update_ns;

    bool all_terminal = true;
    OrderState best_progress_status = OrderState::NotSet;
    int best_progress_rank = -1;
    std::size_t new_child_count = 0;

    for (InternalOrderId child_id : children_it->second) {
        const OrderEntry* child = find_order_nolock(child_id);
        if (!child) {
            continue;
        }

        if (child->request.order_type != OrderType::New) {
            continue;
        }

        ++new_child_count;

        total_volume_traded = saturating_add(total_volume_traded, child->request.volume_traded);
        total_volume_remain = saturating_add(total_volume_remain, child->request.volume_remain);
        total_dvalue_traded = saturating_add(total_dvalue_traded, child->request.dvalue_traded);
        total_fee = saturating_add(total_fee, child->request.dfee_executed);
        latest_update_ns = std::max(latest_update_ns, child->last_update_ns);

        const OrderState child_status = child->request.order_state.load(std::memory_order_acquire);
        if (!is_terminal_state(child_status)) {
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

    auto notify_parent = [&]() {
        if (change_callback_) {
            change_callback_(*parent, order_book_event_t::ParentRefreshed);
        }
    };

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
        parent->request.order_state.store(OrderState::TraderError, std::memory_order_release);
        notify_parent();
        return;
    }

    if (all_terminal) {
        parent->request.order_state.store(OrderState::Finished, std::memory_order_release);
        notify_parent();
        return;
    }

    if (best_progress_status != OrderState::NotSet) {
        parent->request.order_state.store(best_progress_status, std::memory_order_release);
    }
    notify_parent();
}

}  // namespace acct_service
