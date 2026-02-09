#include "core/account_service.hpp"

#include <cassert>
#include <cstdio>

namespace acct_service {

account_service::account_service() = default;

account_service::~account_service() {
    stop();
    cleanup();
    state_.store(service_state_t::Stopped, std::memory_order_release);
}

bool account_service::initialize(const std::string& config_path) {
    const service_state_t current = state();
    if (current == service_state_t::Initializing || current == service_state_t::Running) {
        return false;
    }

    state_.store(service_state_t::Initializing, std::memory_order_release);
    cleanup();

    if (!init_config(config_path) || !init_shared_memory() || !init_portfolio() || !init_risk_manager() ||
        !init_order_components() || !init_event_loop()) {
        state_.store(service_state_t::Error, std::memory_order_release);
        cleanup();
        return false;
    }

    state_.store(service_state_t::Ready, std::memory_order_release);
    return true;
}

bool account_service::initialize(const acct_service::config& cfg) {
    const service_state_t current = state();
    if (current == service_state_t::Initializing || current == service_state_t::Running) {
        return false;
    }

    state_.store(service_state_t::Initializing, std::memory_order_release);
    cleanup();

    config_manager_.get() = cfg;
    if (!config_manager_.validate() || !init_shared_memory() || !init_portfolio() || !init_risk_manager() ||
        !init_order_components() || !init_event_loop()) {
        state_.store(service_state_t::Error, std::memory_order_release);
        cleanup();
        return false;
    }

    state_.store(service_state_t::Ready, std::memory_order_release);
    return true;
}

int account_service::run() {
    if (!event_loop_) {
        return -1;
    }

    const service_state_t current = state();
    if (current != service_state_t::Ready && current != service_state_t::Stopped) {
        return -1;
    }

    state_.store(service_state_t::Running, std::memory_order_release);
    event_loop_->run();

    const service_state_t after = state();
    if (after == service_state_t::Stopping || after == service_state_t::Running) {
        state_.store(service_state_t::Stopped, std::memory_order_release);
    }

    return (state() == service_state_t::Stopped) ? 0 : -1;
}

void account_service::stop() {
    const service_state_t current = state();
    if (current == service_state_t::Running) {
        state_.store(service_state_t::Stopping, std::memory_order_release);
    }

    if (event_loop_) {
        event_loop_->stop();
    }
}

service_state_t account_service::state() const noexcept { return state_.load(std::memory_order_acquire); }

const config_manager& account_service::config() const {
    return config_manager_;
}

const order_book& account_service::orders() const {
    assert(order_book_ != nullptr);
    return *order_book_;
}

const position_manager& account_service::positions() const {
    assert(position_manager_ != nullptr);
    return *position_manager_;
}

const risk_manager& account_service::risk() const {
    assert(risk_manager_ != nullptr);
    return *risk_manager_;
}

void account_service::print_stats() const {
    if (event_loop_) {
        const event_loop_stats& loop_stats = event_loop_->stats();
        std::fprintf(stderr,
            "[account_service] loop_iter=%llu orders=%llu responses=%llu idle=%llu avg_ns=%.0f\n",
            static_cast<unsigned long long>(loop_stats.total_iterations),
            static_cast<unsigned long long>(loop_stats.orders_processed),
            static_cast<unsigned long long>(loop_stats.responses_processed),
            static_cast<unsigned long long>(loop_stats.idle_iterations), loop_stats.avg_latency_ns());
    }

    if (order_book_) {
        std::fprintf(stderr, "[account_service] active_orders=%zu\n", order_book_->active_count());
    }

    if (risk_manager_) {
        const risk_stats& stats = risk_manager_->stats();
        std::fprintf(stderr,
            "[account_service] risk_checks=%llu passed=%llu rejected=%llu\n",
            static_cast<unsigned long long>(stats.total_checks), static_cast<unsigned long long>(stats.passed),
            static_cast<unsigned long long>(stats.rejected));
    }
}

bool account_service::init_config(const std::string& config_path) {
    if (!config_path.empty()) {
        return config_manager_.load_from_file(config_path);
    }
    return config_manager_.validate();
}

bool account_service::init_shared_memory() {
    const shm_config& shm_cfg = config_manager_.shm();
    const account_id_t account_id = config_manager_.account_id();
    const shm_mode mode = shm_cfg.create_if_not_exist ? shm_mode::OpenOrCreate : shm_mode::Open;

    upstream_shm_ = upstream_shm_manager_.open_upstream(shm_cfg.upstream_shm_name, mode, account_id);
    if (!upstream_shm_) {
        return false;
    }

    downstream_shm_ = downstream_shm_manager_.open_downstream(shm_cfg.downstream_shm_name, mode, account_id);
    if (!downstream_shm_) {
        return false;
    }

    trades_shm_ = trades_shm_manager_.open_trades(shm_cfg.trades_shm_name, mode, account_id);
    if (!trades_shm_) {
        return false;
    }

    positions_shm_ = positions_shm_manager_.open_positions(shm_cfg.positions_shm_name, mode, account_id);
    if (!positions_shm_) {
        return false;
    }

    return true;
}

bool account_service::init_portfolio() {
    account_info_ = std::make_unique<account_info_manager>();
    trade_records_ = std::make_unique<trade_record_manager>();
    entrust_records_ = std::make_unique<entrust_record_manager>();

    position_manager_ = std::make_unique<position_manager>(positions_shm_);
    if (!position_manager_ || !position_manager_->initialize(config_manager_.account_id())) {
        return false;
    }

    return load_account_info() && load_positions() && load_today_trades() && load_today_entrusts();
}

bool account_service::init_risk_manager() {
    if (!position_manager_) {
        return false;
    }
    risk_manager_ = std::make_unique<risk_manager>(*position_manager_, config_manager_.risk());
    return static_cast<bool>(risk_manager_);
}

bool account_service::init_order_components() {
    if (!downstream_shm_) {
        return false;
    }

    order_book_ = std::make_unique<order_book>();
    order_router_ = std::make_unique<order_router>(*order_book_, downstream_shm_, config_manager_.split());
    return order_book_ != nullptr && order_router_ != nullptr;
}

bool account_service::init_event_loop() {
    if (!upstream_shm_ || !downstream_shm_ || !trades_shm_ || !order_book_ || !order_router_ || !position_manager_ ||
        !risk_manager_) {
        return false;
    }

    event_loop_ = std::make_unique<event_loop>(config_manager_.event_loop(), upstream_shm_, downstream_shm_,
        *order_book_, *order_router_, *position_manager_, *risk_manager_);
    if (event_loop_) {
        event_loop_->set_trades_shm(trades_shm_);
    }
    return static_cast<bool>(event_loop_);
}

bool account_service::load_account_info() {
    if (!account_info_) {
        return false;
    }

    const acct_service::config& cfg = config_manager_.get();
    bool loaded = false;

    if (!cfg.config_file.empty()) {
        loaded = account_info_->load_from_config(cfg.config_file);
    }

    if (!loaded && cfg.db.enable_persistence && !cfg.db.db_path.empty()) {
        loaded = account_info_->load_from_db(cfg.db.db_path, cfg.account_id);
    }

    account_info& info = account_info_->info();
    info.account_id = cfg.account_id;
    if (!loaded) {
        info.state = account_state_t::Ready;
    }

    return true;
}

bool account_service::load_positions() {
    return position_manager_ != nullptr;
}

bool account_service::load_today_trades() {
    if (!trade_records_) {
        return false;
    }

    const acct_service::config& cfg = config_manager_.get();
    if (!cfg.db.enable_persistence || cfg.db.db_path.empty()) {
        return true;
    }

    return trade_records_->load_today_trades(cfg.db.db_path, cfg.account_id);
}

bool account_service::load_today_entrusts() {
    if (!entrust_records_) {
        return false;
    }

    const acct_service::config& cfg = config_manager_.get();
    if (!cfg.db.enable_persistence || cfg.db.db_path.empty()) {
        return true;
    }

    return entrust_records_->load_today_entrusts(cfg.db.db_path, cfg.account_id);
}

void account_service::cleanup() {
    event_loop_.reset();
    order_router_.reset();
    order_book_.reset();
    risk_manager_.reset();

    entrust_records_.reset();
    trade_records_.reset();
    position_manager_.reset();
    account_info_.reset();

    upstream_shm_ = nullptr;
    downstream_shm_ = nullptr;
    trades_shm_ = nullptr;
    positions_shm_ = nullptr;

    upstream_shm_manager_.close();
    downstream_shm_manager_.close();
    trades_shm_manager_.close();
    positions_shm_manager_.close();
}

}  // namespace acct_service
