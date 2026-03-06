#include "core/account_service.hpp"

#include <cassert>
#include <cstdio>
#include <string>

#include "common/log.hpp"
#include "shm/orders_shm.hpp"

namespace acct_service {

namespace {

ErrorStatus make_service_error(ErrorCode code, std::string_view message, int sys_errno = 0) {
    return ACCT_MAKE_ERROR(ErrorDomain::core, code, "AccountService", message, sys_errno);
}

}  // namespace

AccountService::AccountService() = default;

AccountService::~AccountService() {
    stop();
    cleanup();
    shutdown_logger();
    state_.store(ServiceState::Stopped, std::memory_order_release);
}

// 使用配置文件初始化
bool AccountService::initialize(const std::string& config_path) {
    clear_shutdown_reason();
    clear_last_error();

    const ServiceState current = state();
    if (current == ServiceState::Initializing || current == ServiceState::Running) {
        raise_service_error(make_service_error(ErrorCode::InvalidState, "initialize called in invalid state"));
        return false;
    }

    state_.store(ServiceState::Initializing, std::memory_order_release);
    cleanup();

    if (!init_config(config_path)) {
        state_.store(ServiceState::Error, std::memory_order_release);
        cleanup();
        return false;
    }

    if (!init_logger(config_manager_.log(), config_manager_.account_id())) {
        raise_service_error(make_service_error(ErrorCode::LoggerInitFailed, "failed to initialize logger"));
        state_.store(ServiceState::Error, std::memory_order_release);
        cleanup();
        return false;
    }

    if (!init_shared_memory() || !init_portfolio() || !init_risk_manager() || !init_order_components() ||
        !init_event_loop()) {
        state_.store(ServiceState::Error, std::memory_order_release);
        flush_logger(200);
        cleanup();
        return false;
    }

    state_.store(ServiceState::Ready, std::memory_order_release);
    return true;
}

int AccountService::run() {
    if (!event_loop_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "event loop not initialized"));
        return -1;
    }

    const ServiceState current = state();
    if (current != ServiceState::Ready && current != ServiceState::Stopped) {
        raise_service_error(make_service_error(ErrorCode::InvalidState, "run called in invalid state"));
        return -1;
    }

    state_.store(ServiceState::Running, std::memory_order_release);
    event_loop_->run();

    if (should_terminate_due_to_error() || should_stop_service()) {
        stop();
        state_.store(ServiceState::Error, std::memory_order_release);
        flush_logger(200);
        return -1;
    }

    const ServiceState after = state();
    if (after == ServiceState::Stopping || after == ServiceState::Running) {
        state_.store(ServiceState::Stopped, std::memory_order_release);
    }

    return (state() == ServiceState::Stopped) ? 0 : -1;
}

void AccountService::stop() {
    const ServiceState current = state();
    if (current == ServiceState::Running) {
        state_.store(ServiceState::Stopping, std::memory_order_release);
    }

    if (event_loop_) {
        event_loop_->stop();
    }
}

bool AccountService::has_fatal_error() const noexcept {
    return shutdown_reason_.load(std::memory_order_acquire) == ErrorSeverity::Fatal;
}

const ErrorStatus& AccountService::last_error() const noexcept { return last_error_; }

ServiceState AccountService::state() const noexcept { return state_.load(std::memory_order_acquire); }

const ConfigManager& AccountService::config() const { return config_manager_; }

const OrderBook& AccountService::orders() const {
    assert(order_book_ != nullptr);
    return *order_book_;
}

const PositionManager& AccountService::positions() const {
    assert(position_manager_ != nullptr);
    return *position_manager_;
}

const RiskManager& AccountService::risk() const {
    assert(risk_manager_ != nullptr);
    return *risk_manager_;
}

void AccountService::print_stats() const {
    if (event_loop_) {
        const event_loop_stats& loop_stats = event_loop_->stats();
        std::fprintf(stderr, "[AccountService] loop_iter=%llu orders=%llu responses=%llu idle=%llu avg_ns=%.0f\n",
            static_cast<unsigned long long>(loop_stats.total_iterations),
            static_cast<unsigned long long>(loop_stats.orders_processed),
            static_cast<unsigned long long>(loop_stats.responses_processed),
            static_cast<unsigned long long>(loop_stats.idle_iterations), loop_stats.avg_latency_ns());
    }

    if (order_book_) {
        std::fprintf(stderr, "[AccountService] active_orders=%zu\n", order_book_->active_count());
    }

    if (risk_manager_) {
        const RiskState& stats = risk_manager_->stats();
        std::fprintf(stderr, "[AccountService] risk_checks=%llu passed=%llu rejected=%llu\n",
            static_cast<unsigned long long>(stats.total_checks), static_cast<unsigned long long>(stats.passed),
            static_cast<unsigned long long>(stats.rejected));
    }
}

bool AccountService::init_config(const std::string& config_path) {
    if (config_path.empty()) {
        raise_service_error(make_service_error(ErrorCode::InvalidConfig, "config path must not be empty"));
        return false;
    }

    if (!config_manager_.load_from_file(config_path)) {
        raise_service_error(make_service_error(ErrorCode::ConfigParseFailed, "failed to load config file"));
        return false;
    }

    return true;
}

bool AccountService::init_shared_memory() {
    const SHMConfig& shm_cfg = config_manager_.shm();
    const acct_service::Config& cfg = config_manager_.get();
    const AccountId account_id = config_manager_.account_id();
    const shm_mode mode = shm_cfg.create_if_not_exist ? shm_mode::OpenOrCreate : shm_mode::Open;

    upstream_shm_ = upstream_shm_manager_.open_upstream(shm_cfg.upstream_shm_name, mode, account_id);
    if (!upstream_shm_) {
        raise_service_error(make_service_error(ErrorCode::ShmOpenFailed, "failed to open upstream shm"));
        return false;
    }

    downstream_shm_ = downstream_shm_manager_.open_downstream(shm_cfg.downstream_shm_name, mode, account_id);
    if (!downstream_shm_) {
        raise_service_error(make_service_error(ErrorCode::ShmOpenFailed, "failed to open downstream shm"));
        return false;
    }

    trades_shm_ = trades_shm_manager_.open_trades(shm_cfg.trades_shm_name, mode, account_id);
    if (!trades_shm_) {
        raise_service_error(make_service_error(ErrorCode::ShmOpenFailed, "failed to open trades shm"));
        return false;
    }

    const std::string dated_orders_name = make_orders_shm_name(shm_cfg.orders_shm_name, cfg.trading_day);
    orders_shm_ = orders_shm_manager_.open_orders(dated_orders_name, mode, account_id);
    if (!orders_shm_) {
        raise_service_error(make_service_error(ErrorCode::ShmOpenFailed, "failed to open orders shm"));
        return false;
    }

    positions_shm_ = positions_shm_manager_.open_positions(shm_cfg.positions_shm_name, mode, account_id);
    if (!positions_shm_) {
        raise_service_error(make_service_error(ErrorCode::ShmOpenFailed, "failed to open positions shm"));
        return false;
    }

    return true;
}

bool AccountService::init_portfolio() {
    account_info_ = std::make_unique<account_info_manager>();
    trade_records_ = std::make_unique<trade_record_manager>();
    entrust_records_ = std::make_unique<entrust_record_manager>();

    const acct_service::Config& cfg = config_manager_.get();
    position_manager_ =
        std::make_unique<PositionManager>(positions_shm_, cfg.config_file, cfg.db.db_path, cfg.db.enable_persistence);
    if (!position_manager_ || !position_manager_->initialize(config_manager_.account_id())) {
        raise_service_error(
            make_service_error(ErrorCode::ComponentUnavailable, "failed to initialize position manager"));
        return false;
    }

    return load_account_info() && load_positions() && load_today_trades() && load_today_entrusts();
}

bool AccountService::init_risk_manager() {
    if (!position_manager_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "position manager unavailable"));
        return false;
    }
    risk_manager_ = std::make_unique<RiskManager>(*position_manager_, config_manager_.risk());
    if (!risk_manager_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "failed to create risk manager"));
        return false;
    }
    return true;
}

bool AccountService::init_order_components() {
    if (!downstream_shm_ || !orders_shm_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "downstream/orders shm unavailable"));
        return false;
    }

    order_book_ = std::make_unique<OrderBook>();
    order_router_ = std::make_unique<order_router>(*order_book_, downstream_shm_, orders_shm_, config_manager_.split());
    if (!order_book_ || !order_router_) {
        raise_service_error(
            make_service_error(ErrorCode::ComponentUnavailable, "failed to initialize order components"));
        return false;
    }
    if (!order_router_->recover_downstream_active_orders(upstream_shm_)) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "failed to recover downstream active orders"));
        return false;
    }
    return true;
}

bool AccountService::init_event_loop() {
    if (!upstream_shm_ || !downstream_shm_ || !trades_shm_ || !orders_shm_ || !order_book_ || !order_router_ ||
        !position_manager_ || !risk_manager_) {
        raise_service_error(
            make_service_error(ErrorCode::ComponentUnavailable, "event loop dependencies unavailable"));
        return false;
    }

    event_loop_ = std::make_unique<EventLoop>(config_manager_.EventLoop(), upstream_shm_, downstream_shm_, trades_shm_,
        orders_shm_, *order_book_, *order_router_, *position_manager_, *risk_manager_,
        account_info_ ? &account_info_->info() : nullptr);
    if (!event_loop_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "failed to initialize event loop"));
        return false;
    }
    return true;
}

bool AccountService::load_account_info() {
    if (!account_info_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "account info manager unavailable"));
        return false;
    }

    const acct_service::Config& cfg = config_manager_.get();
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
        info.state = AccountState::Ready;
    }

    return true;
}

bool AccountService::load_positions() {
    if (!position_manager_) {
        raise_service_error(make_service_error(
            ErrorCode::ComponentUnavailable, "position manager unavailable while loading positions"));
        return false;
    }
    return true;
}

bool AccountService::load_today_trades() {
    if (!trade_records_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "trade record manager unavailable"));
        return false;
    }

    const acct_service::Config& cfg = config_manager_.get();
    if (!cfg.db.enable_persistence || cfg.db.db_path.empty()) {
        return true;
    }

    if (!trade_records_->load_today_trades(cfg.db.db_path, cfg.account_id)) {
        raise_service_error(make_service_error(ErrorCode::InternalError, "failed to load today trades"));
        return false;
    }
    return true;
}

bool AccountService::load_today_entrusts() {
    if (!entrust_records_) {
        raise_service_error(make_service_error(ErrorCode::ComponentUnavailable, "entrust record manager unavailable"));
        return false;
    }

    const acct_service::Config& cfg = config_manager_.get();
    if (!cfg.db.enable_persistence || cfg.db.db_path.empty()) {
        return true;
    }

    if (!entrust_records_->load_today_entrusts(cfg.db.db_path, cfg.account_id)) {
        raise_service_error(make_service_error(ErrorCode::InternalError, "failed to load today entrusts"));
        return false;
    }
    return true;
}

void AccountService::cleanup() {
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
    orders_shm_ = nullptr;
    positions_shm_ = nullptr;

    // 共享内存不删除只关闭映射
    upstream_shm_manager_.close();
    downstream_shm_manager_.close();
    trades_shm_manager_.close();
    orders_shm_manager_.close();
    positions_shm_manager_.close();
}

void AccountService::raise_service_error(const ErrorStatus& status) {
    last_error_ = status;
    record_error(status);
    ACCT_LOG_ERROR_STATUS(status);

    const ErrorPolicy& policy = classify(status.domain, status.code);
    ErrorSeverity expected = shutdown_reason_.load(std::memory_order_acquire);
    while (expected < policy.severity) {
        if (shutdown_reason_.compare_exchange_weak(
                expected, policy.severity, std::memory_order_acq_rel, std::memory_order_acquire)) {
            break;
        }
    }
}

bool AccountService::should_terminate_due_to_error() const noexcept {
    return shutdown_reason_.load(std::memory_order_acquire) >= ErrorSeverity::Critical;
}

}  // namespace acct_service
